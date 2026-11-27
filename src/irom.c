#include "irom.h"
#include <string.h>

int irom_add(Irom *rom, uint32_t address, const char *name)
{
    unsigned i, kind;
    if (address & 1 || rom->count == IROM_MAX_SERVICES)
        return 0;
    if (!strcmp(name, "memcpy"))
        kind = IROM_MEMCPY;
    else if (!strcmp(name, "version"))
        kind = IROM_VERSION;
    else
        return 0;
    for (i = 0; i < rom->count; ++i)
        if (rom->services[i].address == address)
            return 0;
    if (!rom->count || address < rom->first_address)
        rom->first_address = address;
    if (!rom->count || address > rom->last_address)
        rom->last_address = address;
    rom->services[rom->count].address = address;
    rom->services[rom->count++].kind = kind;
    return 1;
}

/* HLE accepts ordinary memory, never MMIO or flash-command destinations.
   Validate all ranges before modifying the guest; do not silently continue
   through an invalid pointer just because bring-up ignores unknown MMIO. */
static int memory(Bus *bus, uint32_t address, uint32_t size, int write)
{
    uint64_t end = (uint64_t)address + size;
    unsigned i;
    if (end > UINT64_C(0x100000000))
        return 0;
    if (!size)
        return 1;
    for (i = 0; i < bus->count; ++i) {
        Region *r = &bus->regions[i];
        if (address >= r->base && end <= (uint64_t)r->base + r->size && r->data && !r->aborts && !r->read &&
            (!write || (!r->readonly && !r->write)))
            return 1;
        /* ROM flash has a read callback; use bus reads so its current mode
           remains observable, but never treat a device-only region as RAM. */
        if (!write && address >= r->base && end <= (uint64_t)r->base + r->size && r->data && !r->aborts && r->readonly)
            return 1;
    }
    return 0;
}

static int bad(Arm7 *c, const char *reason)
{
    c->halted = 1;
    c->error = reason;
    return -1;
}

enum { C100_CS5 = 0x40020, CS_FIELDS = 0xff8d, CS_BUS_MASK = 0x008d, CS_X16_ACTIVE_LOW = 5 };
enum { C100_IROM_BASE = 0x00080000, NOR_VECTORS = 0x20 };

static unsigned cycles_for_ns(uint32_t hz, unsigned ns)
{
    return (unsigned)(((uint64_t)hz * ns + 999999999u) / 1000000000u);
}

static void update_nor_bus(C100NorBoot *boot)
{
    unsigned minimum = cycles_for_ns(boot->arm_hz, 65); /* LRS1828 tAVAV, read and write. */
    for (unsigned i = 0; i < 2; ++i) {
        if (!boot->banks[i])
            continue;
        Region *region = boot->banks[i]->region;
        uint16_t cs = boot->chip_select[i];
        /* CX805-30 Table 6: wait field + 1 strobe clocks, plus the
           setup and hold fields, rounded up to whole ARM clocks. */
        unsigned cycles = ((cs >> 8) & 15) + ((cs >> 12) & 3) + (cs >> 14) + 2;
        region->read16_cycles = region->write16_cycles = (uint8_t)(cycles > minimum ? cycles : minimum);
        region->aborts = (cs & CS_BUS_MASK) != CS_X16_ACTIVE_LOW;
    }
    Region *first = boot->banks[0]->region;
    boot->vectors->aborts = !boot->vectors->data || first->aborts;
    boot->vectors->read16_cycles = first->read16_cycles;
}

static uint32_t read_nor_cs(void *context, uint32_t offset, unsigned width)
{
    C100NorBoot *boot = context;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned byte = offset + i;
        value |= ((boot->chip_select[byte / 2] >> (8 * (byte % 2))) & 255u) << (8 * i);
    }
    return value;
}

static void write_nor_cs(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    C100NorBoot *boot = context;
    for (unsigned i = 0; i < width; ++i) {
        unsigned byte = offset + i, shift = 8 * (byte % 2);
        uint16_t *cs = &boot->chip_select[byte / 2];
        *cs = (uint16_t)(((*cs & ~(255u << shift)) | (((value >> (8 * i)) & 255u) << shift)) & CS_FIELDS);
    }
    update_nor_bus(boot);
}

int irom_cx805_attach(C100NorBoot *boot, Bus *bus, Flash *first, Flash *second, size_t image_size,
                      uint32_t arm_hz)
{
    if (!first || first->size < NOR_VECTORS || first->size > C100_NOR_BANK_SIZE || image_size > first->size ||
        (second && second->size > C100_NOR_BANK_SIZE) ||
        !bus_device(bus, C100_CS5, 4, read_nor_cs, write_nor_cs, boot) || !bus_map_abort(bus, 0, NOR_VECTORS))
        return 0;
    boot->banks[0] = first;
    boot->banks[1] = second;
    boot->vectors = &bus->regions[bus->count - 1];
    boot->vectors->readonly = 1;
    boot->vectors->read = first->region->read;
    boot->vectors->context = first;
    boot->image_size = image_size;
    boot->arm_hz = arm_hz;
    irom_cx805_reset(boot);
    return 1;
}

void irom_cx805_reset(C100NorBoot *boot)
{
    boot->pending = 1;
    boot->vectors->data = NULL;
    boot->chip_select[0] = boot->chip_select[1] = 0;
    update_nor_bus(boot);
}

void irom_cx805_set_clock(C100NorBoot *boot, uint32_t arm_hz)
{
    if (boot->banks[0] && boot->arm_hz != arm_hz) {
        boot->arm_hz = arm_hz;
        update_nor_bus(boot);
    }
}

static uint32_t read_nor_header(void *context, uint32_t offset, unsigned width)
{
    Bus *bus = context;
    /* Unlike an ARM instruction's access, the modeled IROM read has no
       baseline bus cycle in the core's timing. Charge the whole transfer. */
    ++bus->wait_cycles;
    return bus_read(bus, C100_NOR1 + offset, width);
}

int irom_cx805_boot(C100NorBoot *boot, Arm7 *cpu)
{
    Bus *bus = cpu->bus;
    boot->pending = 0;
    bus->pc = cpu->last_pc = C100_IROM_BASE;
    bus->ticks = cpu->cycles;
    bus->wait_cycles = 0;
    /* 100 ns after stable F-VCC plus 150 ns from F-RST high. The
       IROM polls/loader exchange are not part of this NOR handoff model. */
    cpu->clock_cycles += cycles_for_ns(boot->arm_hz, 250);
    /* Conservative initial x16 timing; the firmware can reprogram CS5/6. */
    boot->chip_select[0] = boot->chip_select[1] = 0x0605;
    update_nor_bus(boot);
    uint32_t target;
    const char *error = cx805_nor_header(bus, read_nor_header, boot->image_size, &target);
    cpu->clock_cycles += bus->wait_cycles;
    if (error)
        return bad(cpu, error);
    /* Provisional vector window, published only after the header passes. */
    boot->vectors->data = boot->banks[0]->data;
    update_nor_bus(boot);
    cpu->r[15] = C100_NOR1 + C100_NOR_ENTRY;
    return 1;
}

int irom_step(Irom *rom, Arm7 *c)
{
    IromService *service = NULL;
    unsigned i;
    uint32_t destination = c->r[0], source = c->r[1], size = c->r[2], result, return_address = c->r[14];
    for (i = 0; i < rom->count; ++i)
        if (rom->services[i].address == c->r[15]) {
            service = &rom->services[i];
            break;
        }
    if (!service)
        return 0;
    c->last_pc = c->r[15];
    c->last_opcode = 0;
    c->bus->pc = c->last_pc;
    c->bus->ticks = c->cycles;
    if (!(c->cpsr & ARM_T))
        return bad(c, "internal-ROM service: expected Thumb entry");
    if (service->kind == IROM_MEMCPY) {
        if (size > 32u * 1024 * 1024 || !memory(c->bus, source, size, 0) ||
            !memory(c->bus, destination, size, 1))
            return bad(c, "internal-ROM memcpy: invalid memory range");
        if (size && destination != source && (uint64_t)destination < (uint64_t)source + size &&
            (uint64_t)source < (uint64_t)destination + size)
            return bad(c, "internal-ROM memcpy: overlapping buffers");
        if (destination != source)
            for (i = 0; i < size; ++i)
                bus_write(c->bus, destination + i, 1, bus_read(c->bus, source + i, 1));
        result = destination;
    } else {
        if (!rom->version_configured || return_address != (rom->version_return | 1u))
            return bad(c, "internal-ROM version query: unverified caller contract");
        if ((destination & 1) || !memory(c->bus, destination, 2, 1))
            return bad(c, "internal-ROM version query: invalid output pointer");
        /* UHGetIromVersion consumes only this halfword. Its other two output
           buffers are discarded. Do not invent their widths or BIST results.
           Restrict this HLE to the decoded caller; other BIST uses must stop. */
        bus_write(c->bus, destination, 2, rom->version);
        result = destination; /* Scalar return is not consumed by this caller. */
    }
    if (c->bus->fault)
        return bad(c, "internal-ROM service: bus fault");
    c->r[0] = result;
    /* ARMv4T interworking return, equivalent to BX LR. Flags and banked
       registers are preserved; the shared CPU interpreter is unchanged. */
    c->cpsr = (c->cpsr & ~ARM_T) | ((return_address & 1) ? ARM_T : 0);
    c->r[15] = return_address & ((return_address & 1) ? ~1u : ~3u);
    ++service->calls;
    ++c->instructions;
    ++c->cycles; /* One atomic scheduler step, not cycle accurate. */
    return 1;
}
