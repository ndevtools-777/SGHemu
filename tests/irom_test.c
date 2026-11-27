#include "machine.h"
#include "c100_nor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "IROM test line %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static void reset(Arm7 *c, Bus *b, uint32_t pc)
{
    unsigned i;
    arm7_reset(c, b, pc);
    c->cpsr |= ARM_T | ARM_N | ARM_C;
    for (i = 3; i < 15; ++i)
        c->r[i] = 0x12340000 + i;
    c->r[14] = 0x2001;
}

static void fixture(const char *services)
{
    FILE *f = fopen("build/irom-fixture.bin", "wb");
    static const uint8_t bytes[32] = {0xfe, 0xff, 0xff, 0xea};
    CHECK(f != NULL);
    CHECK(fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes));
    CHECK(fclose(f) == 0);
    f = fopen("build/irom-fixture.cfg", "w");
    CHECK(f != NULL);
    fputs("rom=0x1000000,irom-fixture.bin\nram=0x1000,0x200\nreset=0x1000000\nintc=0x40100\naudio=silent\n",
          f);
    fputs(services, f);
    CHECK(fclose(f) == 0);
}

static void nor_fixture(Machine *m, const uint32_t *code, size_t size)
{
    FILE *f = fopen("build/irom-nor.bin", "wb");
    CHECK(f && fwrite(code, 1, size, f) == size && !fclose(f));
    f = fopen("build/irom-nor.cfg", "w");
    CHECK(f);
    fputs("rom=0x1000000,irom-nor.bin,0x10000\nflash=0x1000000,0xffff,0xffff\n"
          "rom=0x2000000,irom-nor.bin,0x10000\nflash=0x2000000,0xffff,0xffff\n"
          "reset=0\nc100_nor_boot=1\nram=0x20,0x2fe0\nexceptions=arm7\nclock=0x40b00,50700000\n"
          "lcd_s6b33=0xc00000,0xc80000,0\npmic=0x40300,0x90\nc100_power_key_ms=100\n",
          f);
    CHECK(!fclose(f));
    CHECK(machine_load(m, "build/irom-nor.cfg", NULL, NULL));
    CHECK(m->irom.boot.pending && m->irom.boot.vectors->aborts && !m->irom.boot.vectors->data);
    CHECK(bus_read(&m->bus, 0x40020, 4) == 0 && m->cpu.r[15] == 0);
}

static void step_cycles(Machine *m, unsigned cycles)
{
    uint64_t before = m->cpu.clock_cycles;
    CHECK(machine_step(m, 0) && m->cpu.clock_cycles - before == cycles);
}

static void nor_boot(Machine *m)
{
    uint32_t code[0x84 / 4] = {
        [0] = 0xea400013, /* B 0x01000054 when fetched via the low alias. */
        [1] = 0xea40001d, /* Undefined vector branches to 0x01000080. */
        [C100_NOR_ENTRY / 4] = 0xea000005,
        [C100_NOR_LENGTH / 4] = sizeof(code),
        [C100_NOR_MARKER / 4] = 0xcbcb,
        [C100_NOR_CODE / 4] = 0xe3a0202a, /* MOV r2, #42. */
        [0x58 / 4] = 0xe12fff10,          /* BX r0. */
        [0x60 / 4] = 0x680c232b,          /* MOVS r3, #43; LDR r4, [r1]. */
        [0x64 / 4] = 0xde00880d,          /* LDRH r5, [r1]; undefined. */
        [0x80 / 4] = 0xeafffffe,
    };
    nor_fixture(m, code, sizeof(code));
    CHECK(machine_start_standby(m));
    machine_key(m, SGH_KEY_POWER, 1);
    CHECK(!machine_powered_off(m) && m->irom.boot.pending && m->pmic.key_supply);
    machine_key(m, SGH_KEY_POWER, 0);
    CHECK(machine_powered_off(m) && !m->cpu.instructions && !m->lcd.powered);
    machine_key(m, SGH_KEY_POWER, 1);
    /* 250 ns rounded up, followed by one halfword and three word reads. */
    step_cycles(m, 13 + 8 + 3 * 16);
    CHECK(!m->cpu.instructions && m->cpu.last_pc == 0x80000 && m->cpu.r[15] == C100_NOR1 + C100_NOR_ENTRY);
    CHECK(bus_read(&m->bus, 0x40020, 4) == 0x06050605 && bus_read(&m->bus, 0, 4) == code[0]);
    CHECK(!m->irom.boot.vectors->aborts && !m->bus.faults);
    m->cpu.r[0] = C100_NOR1 + 0x61;
    m->cpu.r[1] = C100_NOR2;
    step_cycles(m, 18); /* 32-bit instruction fetch uses two x16 beats. */
    CHECK(m->cpu.r[15] == C100_NOR1 + C100_NOR_CODE);
    step_cycles(m, 16);
    CHECK(m->cpu.r[2] == 42);
    step_cycles(m, 18);
    CHECK(m->cpu.cpsr & ARM_T);
    step_cycles(m, 8); /* Thumb fetch uses one beat. */
    CHECK(m->cpu.r[3] == 43);
    step_cycles(m, 25); /* Word load uses two additional beats. */
    CHECK(m->cpu.r[4] == code[0]);
    bus_write(&m->bus, 0x40022, 2, 5); /* CS6 still needs four clocks (65 ns at 50.7 MHz). */
    step_cycles(m, 13);
    CHECK(m->cpu.r[5] == (code[0] & 0xffff));
    step_cycles(m, 10);
    CHECK(m->cpu.undefined_count == 1 && m->cpu.r[15] == 4 && !(m->cpu.cpsr & ARM_T));
    step_cycles(m, 18);
    CHECK(m->cpu.r[15] == C100_NOR1 + 0x80 && !m->bus.faults);
    bus_write(&m->bus, 0x40020, 1, 0); /* Invalid CS5 also makes the low alias inaccessible. */
    CHECK(m->irom.boot.vectors->aborts);
    CHECK(machine_step(m, 0) && m->cpu.prefetch_abort_count == 1 && m->cpu.r[15] == 0x0c);
    bus_write(&m->bus, 0x40020, 2, 5);
    CHECK(!m->irom.boot.vectors->aborts && bus_read(&m->bus, 4, 4) == code[1]);

    bus_write(&m->bus, 0x40b04, 2, 0x8000);
    CHECK(machine_step(m, 0) && m->c100.reset_count == 3 && m->irom.boot.pending);
    CHECK(m->cpu.r[15] == 0 && m->irom.boot.vectors->aborts && m->pmic.key_supply);
    step_cycles(m, 13 + 8 + 3 * 16);
    CHECK(m->cpu.r[15] == C100_NOR1 + C100_NOR_ENTRY && !m->bus.faults);

    /* A request latched before the batch still needs the modeled IROM handoff. */
    const uint64_t instructions = m->cpu.instructions;
    bus_write(&m->bus, 0x40b04, 2, 0x8000);
    CHECK(machine_run(m, 2, 0) == 2 && m->c100.reset_count == 4 && !m->irom.boot.pending);
    CHECK(m->cpu.r[15] == C100_NOR1 + C100_NOR_ENTRY && m->cpu.instructions == instructions &&
          !m->cpu.prefetch_abort_count && !m->bus.faults);

    static const struct {
        unsigned offset;
        uint32_t value;
        const char *error;
    } bad[] = {
        {C100_NOR_MARKER, 0, "missing CBCB"},
        {C100_NOR_LENGTH, C100_NOR_CODE, "invalid first bank length"},
        {C100_NOR_LENGTH, sizeof(code) + 4, "invalid first bank length"},
        {C100_NOR_ENTRY, 0xe1a00000, "expected ARM branch"},
        {C100_NOR_ENTRY, 0xeaffffef, "entry branch does not reach code"},
        {C100_NOR_ENTRY, 0xea000011, "entry branch does not reach code"},
        {C100_NOR_CODE, UINT32_MAX, "entry branch does not reach code"},
    };
    nor_fixture(m, code, C100_NOR_CODE);
    CHECK(!machine_step(m, 0) && strstr(m->cpu.error, "truncated first bank header"));
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        uint32_t old = code[bad[i].offset / 4];
        code[bad[i].offset / 4] = bad[i].value;
        nor_fixture(m, code, sizeof(code));
        code[bad[i].offset / 4] = old;
        CHECK(!machine_step(m, 0) && m->cpu.halted && !m->cpu.instructions && !m->bus.faults);
        CHECK(strstr(m->cpu.error, "IROM NOR boot: ") && strstr(m->cpu.error, bad[i].error));
        CHECK(m->irom.boot.vectors->aborts && !m->irom.boot.vectors->data);
    }
}

int main(void)
{
    Bus bus = {0};
    Arm7 c;
    Irom rom = {0};
    Machine m = {0};
    uint8_t ram[512], source[128];
    unsigned i;
    uint32_t flags;
    memset(ram, 0xcc, sizeof(ram));
    for (i = 0; i < sizeof(source); ++i)
        source[i] = (uint8_t)(i * 13);
    CHECK(bus_map(&bus, 0x1000, sizeof(ram), ram, 0, 0));
    CHECK(bus_map(&bus, 0x2000, sizeof(source), source, 1, 0));
    CHECK(irom_add(&rom, 0x8077c, "memcpy"));
    CHECK(!irom_add(&rom, 0x8077c, "memcpy"));
    CHECK(!irom_add(&rom, 0x81fe1, "version"));
    CHECK(!irom_add(&rom, 0x81fe0, "unknown"));
    CHECK(irom_add(&rom, 0x81fe0, "version"));
    rom.version = 0x1234;
    rom.version_return = 0x2000;
    rom.version_configured = 1;
    CHECK(!irom_may_call(&rom, 0x8077a) && irom_may_call(&rom, 0x8077c));
    CHECK(irom_may_call(&rom, 0x81fe0) && !irom_may_call(&rom, 0x81fe2));
    {
        Irom bounds = {0};
        CHECK(!irom_may_call(&bounds, 0));
        CHECK(irom_add(&bounds, UINT32_MAX - 1, "memcpy"));
        CHECK(irom_add(&bounds, 0, "version"));
        CHECK(irom_may_call(&bounds, 0) && irom_may_call(&bounds, UINT32_MAX - 1));
    }
    reset(&c, &bus, 0x8077c);
    flags = c.cpsr;
    c.r[0] = 0x1001;
    c.r[1] = 0x2003;
    c.r[2] = 17;
    CHECK(irom_step(&rom, &c) == 1);
    CHECK(!memcmp(ram + 1, source + 3, 17));
    CHECK(ram[0] == 0xcc && ram[18] == 0xcc);
    CHECK(c.r[0] == 0x1001 && c.r[15] == 0x2000 && c.cpsr == flags && c.instructions == 1 && c.cycles == 1);
    for (i = 3; i < 14; ++i)
        CHECK(c.r[i] == 0x12340000 + i);
    reset(&c, &bus, 0x8077c);
    c.r[0] = 0x1020;
    c.r[1] = 0x1001;
    c.r[2] = 17;
    c.r[14] = 0x2006;
    CHECK(irom_step(&rom, &c) == 1);
    CHECK(!(c.cpsr & ARM_T) && c.r[15] == 0x2004);
    CHECK(!memcmp(ram + 32, ram + 1, 17));
    reset(&c, &bus, 0x8077c);
    c.r[0] = c.r[1] = 0xffffffff;
    c.r[2] = 0;
    CHECK(irom_step(&rom, &c) == 1 && c.r[0] == 0xffffffff); /* Zero bytes require no memory access. */
    reset(&c, &bus, 0x8077c);
    c.r[0] = 0x1000;
    c.r[1] = 0x1001;
    c.r[2] = 8;
    CHECK(irom_step(&rom, &c) == -1 && c.halted && !c.instructions);
    CHECK(ram[0] == 0xcc);
    reset(&c, &bus, 0x8077c);
    c.r[0] = 0x2000;
    c.r[1] = 0x1000;
    c.r[2] = 1;
    CHECK(irom_step(&rom, &c) == -1 && source[0] == 0); /* Cannot write ROM. */
    reset(&c, &bus, 0x8077c);
    c.r[0] = 0x1000;
    c.r[1] = 0xfffffff0;
    c.r[2] = 32;
    CHECK(irom_step(&rom, &c) == -1 && ram[0] == 0xcc);
    reset(&c, &bus, 0x81fe0);
    c.r[0] = 0x1080;
    c.r[1] = 0xffffffff;
    c.r[2] = 0xffffffff;
    flags = c.cpsr;
    CHECK(irom_step(&rom, &c) == 1);
    CHECK(ram[128] == 0x34 && ram[129] == 0x12 && ram[130] == 0xcc && ram[131] == 0xcc);
    CHECK(c.r[1] == 0xffffffff && c.r[2] == 0xffffffff && c.cpsr == flags);
    reset(&c, &bus, 0x81fe0);
    c.r[0] = 0x1080;
    c.r[14] = 0x2041;
    CHECK(irom_step(&rom, &c) == -1 &&
          ram[128] == 0x34); /* Unverified caller cannot obtain fake BIST results. */
    reset(&c, &bus, 0x81fe0);
    c.r[0] = 0x1081;
    CHECK(irom_step(&rom, &c) == -1);
    reset(&c, &bus, 0x81fe0);
    rom.version_configured = 0;
    CHECK(irom_step(&rom, &c) == -1);
    reset(&c, &bus, 0x80020);
    CHECK(irom_step(&rom, &c) == 0 && !c.halted && !c.instructions);
    reset(&c, &bus, 0x8077c);
    c.cpsr &= ~ARM_T;
    CHECK(irom_step(&rom, &c) == -1);
    fixture("irom_version=0x81fe0,0xffff,0x1000000\nirom_service=0x8077c,memcpy\n");
    CHECK(machine_load(&m, "build/irom-fixture.cfg", NULL, NULL));
    m.cpu.r[15] = 0x81fe0;
    m.cpu.r[0] = 0x1080;
    m.cpu.r[14] = 0x1000001;
    m.cpu.cpsr &= ~0x80u;
    m.intc.pending = m.intc.enabled = 8;
    CHECK(machine_step(&m, 0));
    CHECK(m.cpu.r[15] == 0x18 && m.cpu.instructions == 0 && m.irom.services[0].calls == 0);
    CHECK(machine_load(&m, "build/irom-fixture.cfg", NULL, NULL));
    m.cpu.r[15] = 0x81fe0;
    m.cpu.r[0] = 0x1080;
    m.cpu.r[14] = 0x1000001;
    m.cpu.cpsr |= ARM_T;
    CHECK(machine_step(&m, 0));
    CHECK(bus_read(&m.bus, 0x1080, 2) == 0xffff && m.cpu.r[15] == 0x1000000 && m.irom.services[0].calls == 1);
    fixture("irom_service=0x81fe0,version\n");
    CHECK(!machine_load(&m, "build/irom-fixture.cfg", NULL, NULL));
    fixture("irom_service=0x1000000,memcpy\n");
    CHECK(!machine_load(&m, "build/irom-fixture.cfg", NULL, NULL));
    fixture("irom_version=0x81fe0,0x10000,0x1000000\n");
    CHECK(!machine_load(&m, "build/irom-fixture.cfg", NULL, NULL));
    fixture("irom_version=0x81fe0,0xffff,0x1000001\n");
    CHECK(!machine_load(&m, "build/irom-fixture.cfg", NULL, NULL));
    nor_boot(&m);
    machine_destroy(&m);
    bus_destroy(&bus);
    printf("Internal-ROM HLE and cold NOR boot: %u checks passed.\n", checks);
    return 0;
}
