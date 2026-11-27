#include "machine.h"
#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* A service session attaches the bench loopback cable and the general DMA
   controller the firmware's factory path needs. It is off for every ordinary
   boot so the handoff state is reproduced unchanged. */
int sgh_service_mode = 0;

static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s))
        ++s;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        --end;
    *end = 0;
    return s;
}

static int number(char *s, uint32_t *v)
{
    char *end;
    unsigned long long n;
    s = trim(s);
    if (*s == '-')
        return 0;
    n = strtoull(s, &end, 0);
    if (s == end || *trim(end) || n > UINT32_MAX)
        return 0;
    *v = (uint32_t)n;
    return 1;
}

/* A profile field list has exactly the expected number of unsigned values. */
static int number_list(char *field, uint32_t *values, unsigned count, uint32_t minimum)
{
    for (unsigned i = 0; i < count; ++i) {
        if (!field)
            return 0;
        char *next = strchr(field, ',');
        if (next)
            *next++ = '\0';
        if (!number(field, &values[i]) || values[i] < minimum)
            return 0;
        field = next;
    }
    return field == NULL;
}

static uint32_t identity_read(void *ctx, uint32_t offset, unsigned width)
{
    Identity16 *id = ctx;
    if (width == 1)
        return (id->value >> (8 * offset)) & 255u;
    return id->value;
}

static void identity_write(void *ctx, uint32_t offset, unsigned width, uint32_t value)
{
    Identity16 *id = ctx;
    bus_fault_access(id->bus, 1, id->address + offset, width, value, "read-only identity fixture");
}

static uint32_t register_read(void *ctx, uint32_t offset, unsigned width)
{
    Register32 *r = ctx;
    uint32_t mask = width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1;
    return (r->value >> (offset * 8)) & mask;
}

static void register_write(void *ctx, uint32_t offset, unsigned width, uint32_t value)
{
    Register32 *r = ctx;
    if (r->irom_vector) {
        bus_fault_access(r->bus, 1, r->address + offset, width, value, "read-only internal-ROM vector");
        return;
    }
    uint32_t mask = width == 4 ? UINT32_MAX : ((1u << (width * 8)) - 1) << (offset * 8);
    r->value = (r->value & ~mask) | ((value << (offset * 8)) & mask);
}

static int image_path(char *out, size_t capacity, const char *config, const char *image)
{
    const char *slash, *back;
    size_t prefix = 0, n = strlen(image);
    if (image[0] != '/' && image[0] != '\\' && !(n >= 2 && image[1] == ':')) {
        slash = strrchr(config, '/');
        back = strrchr(config, '\\');
        if (!slash || (back && back > slash))
            slash = back;
        if (slash)
            prefix = (size_t)(slash - config) + 1;
    }
    if (prefix + n >= capacity)
        return 0;
    memcpy(out, config, prefix);
    memcpy(out + prefix, image, n + 1);
    return 1;
}

static int load_ram_image(uint8_t *data, uint32_t size, const char *config, const char *image)
{
    char path[1024];
    FILE *file;
    int ok;
    if (!*image || !image_path(path, sizeof(path), config, image))
        return 0;
    file = fopen(path, "rb");
    if (!file)
        return 0;
    /* RAM was zeroed by the caller. Reject an empty image or one that would
       extend beyond the mapping, rather than running a truncated program. */
    ok = fread(data, 1, size, file) != 0 && fgetc(file) == EOF && !ferror(file);
    return fclose(file) == 0 && ok;
}

static int load_rom_image(Machine *m, uint32_t base, char *image_spec, const char *override)
{
    if (m->rom_count == sizeof(m->roms) / sizeof(m->roms[0]))
        return 0;

    uint32_t mapped_size = 0;
    char *size_field = strchr(image_spec, ',');
    if (size_field) {
        *size_field++ = '\0';
        if (!number(size_field, &mapped_size))
            return 0;
    }

    RomImage *image = &m->roms[m->rom_count];
    if (!image_path(image->path, sizeof(image->path), override ? "" : m->config_path,
                    override ? override : trim(image_spec)))
        return 0;

    uint8_t *data;
    size_t image_size;
    if (!file_read(image->path, &data, &image_size))
        return 0;
    if (!mapped_size)
        mapped_size = (uint32_t)image_size;
    if (image_size > mapped_size || mapped_size > 32u * 1024 * 1024) {
        free(data);
        return 0;
    }

    /* Space past the file represents erased NOR. */
    if (mapped_size > image_size) {
        uint8_t *padded = realloc(data, mapped_size);
        if (!padded) {
            free(data);
            return 0;
        }
        data = padded;
        memset(data + image_size, 255, mapped_size - image_size);
    }
    if (!bus_map(&m->bus, base, mapped_size, data, 1, 1)) {
        free(data);
        return 0;
    }
    image->base = base;
    image->size = image_size;
    ++m->rom_count;
    return 1;
}

/* Firmware entry points must refer to file bytes, outside the erased padding. */
static int rom_contains(const Machine *m, uint32_t address, unsigned length)
{
    for (unsigned i = 0; i < m->rom_count; ++i) {
        const RomImage *image = &m->roms[i];
        if (address >= image->base && (uint64_t)address + length <= (uint64_t)image->base + image->size)
            return 1;
    }
    return 0;
}

enum { CX805_PLL_BASE = 0x40b00, CX805_ASIC_MODE = 0x4091e, CX805_RESET_REQUEST = 1u << 15 };

static void clock_rate(PhoneClock *c)
{
    unsigned i;
    uint32_t arm = c->reference_hz, dsp = c->reference_hz;
    c->unknown = 0;
    if (c->registers[2] & 8) {
        for (i = 0; i < c->count; ++i)
            if (c->registers[1] == c->encoding[i])
                break;
        if (i == c->count)
            c->unknown = 1;
        else {
            if ((c->registers[2] & 3) == 3)
                arm = c->arm_rates[i];
            if (c->registers[2] & 4)
                dsp = c->dsp_rates[i];
        }
    }
    if (arm != c->arm_hz)
        c->fraction = 0; /* Less than one ns lost at a source switch. */
    c->arm_hz = arm;
    c->dsp_hz = dsp;
}

static uint32_t clock_read(void *context, uint32_t offset, unsigned width)
{
    Machine *m = context;
    PhoneClock *c = &m->clock;
    uint32_t value = 0;
    unsigned i;
    if (offset + width > 6) {
        bus_fault_access(c->bus, 0, c->base + offset, width, 0, "cross-clock register access");
        return 0;
    }
    for (i = 0; i < width; ++i)
        value |= ((c->registers[(offset + i) / 2] >> (((offset + i) & 1) * 8)) & 255u) << (i * 8);
    return value;
}

static void clock_write(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    Machine *m = context;
    PhoneClock *c = &m->clock;
    unsigned i;
    if (offset + width > 6) {
        bus_fault_access(c->bus, 1, c->base + offset, width, value, "cross-clock register access");
        return;
    }
    for (i = 0; i < width; ++i) {
        unsigned reg = (offset + i) / 2, shift = ((offset + i) & 1) * 8;
        c->registers[reg] =
            (uint16_t)((c->registers[reg] & ~(255u << shift)) | (((value >> (i * 8)) & 255u) << shift));
    }
    if (m->c100.enabled && c->base == CX805_PLL_BASE && (c->registers[2] & CX805_RESET_REQUEST))
        m->c100.reset_pending = 1; /* Apply the request only at an instruction boundary. */
    clock_rate(c);
    irom_cx805_set_clock(&m->irom.boot, c->arm_hz);
}

enum { CLOCK_SLEEP_REQUEST = 1u << 2, CLOCK_PWM_ENABLE = 1u << 4 };

int machine_sleeping(const Machine *m)
{
    return !machine_powered_off(m) && m->clock.enable_base && (m->clock.enables & CLOCK_SLEEP_REQUEST);
}

static void clock_apply_enables(Machine *m)
{
    gpt_set_clock(&m->timers[1], !!(m->clock.enables & (1u << 5)));
    sds_uart_set_clock(&m->debug_uart, !!(m->clock.enables & (1u << 9)));
    sds_uart_set_clock(&m->sds_uart, !!(m->clock.enables & (1u << 10)));
    if (m->clock.enable_base)
        pwm_set_clock(&m->c100.pwm, !!(m->clock.enables & CLOCK_PWM_ENABLE));
}

static uint32_t clock_enable_read(void *context, uint32_t offset, unsigned width)
{
    Machine *m = context;
    unsigned mask = width == 1 ? 0xffu : 0xffffu;
    return (m->clock.enables >> (8 * offset)) & mask;
}

static void clock_enable_write(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    Machine *m = context;
    unsigned shift = 8 * offset;
    unsigned mask = (width == 1 ? 0xffu : 0xffffu) << shift;
    m->clock.enables = (uint16_t)((m->clock.enables & ~mask) | ((value << shift) & mask));
    clock_apply_enables(m);
}

static void clock_advance(PhoneClock *c, uint64_t cycles)
{
    const unsigned table_size = sizeof(c->step_ns) / sizeof(c->step_ns[0]);
    uint64_t elapsed_cycles = cycles - c->clock_cycles;
    c->clock_cycles = cycles;
    /* Cache quotients and remainders for small instructions. Adding the
       remainder to the carried fraction gives the same result as dividing
       again, without a 64-bit division on every x86 step. */
    if (elapsed_cycles < table_size && c->arm_hz && c->fraction < c->arm_hz) {
        if (c->table_hz != c->arm_hz) {
            for (unsigned i = 0; i < table_size; ++i) {
                uint64_t numerator = (uint64_t)i * 1000000000u;
                c->step_ns[i] = numerator / c->arm_hz;
                c->step_rem[i] = (uint32_t)(numerator % c->arm_hz);
            }
            c->table_hz = c->arm_hz;
        }
        uint32_t previous_fraction = (uint32_t)c->fraction;
        uint32_t fraction = previous_fraction + c->step_rem[elapsed_cycles];
        c->nanoseconds += c->step_ns[elapsed_cycles];
        /* Both operands are below hz. Carry-aware subtraction is exact even
           above 2 GHz, without a 64-bit addition/comparison on x86. */
        if (fraction < previous_fraction || fraction >= c->arm_hz) {
            fraction -= c->arm_hz;
            ++c->nanoseconds;
        }
        c->fraction = fraction;
        return;
    }
    uint64_t scaled = elapsed_cycles * 1000000000u + c->fraction;
    c->nanoseconds += scaled / c->arm_hz;
    c->fraction = scaled % c->arm_hz;
}

enum {
    GPIO_A,
    GPIO_B,
    GPIO_C,
    GPIO_D,
    GPIO_E,
    C100_GPIO_A_DATA = 0x4028c,
    C100_GPIO_A_CONTROL = 0x40284,
    C100_GPIO_A_MUX = 0x40d04,
    C100_GPIO_C_DATA = 0x40290,
    C100_GPIO_C_CONTROL = 0x40288,
    C100_GPIO_C_MUX = 0x40d08,
    C100_LCD_ENABLE = 1u << 7,
    C100_LCD_ENABLE_MUX = 3u << 14,
    C100_LCD_TIMER2_PULSE = 1u << 14,
    C100_LCD_BRIGHT = 1u << 4,
    C100_LCD_BRIGHT_MUX = 3u << 8,
    C100_LCD_PWM = 1u << 8,
    C100_KEYPAD_LIGHT = 1u << 4,
    C100_LCD_SELECT = 1u << 1,
    C100_LCD_SELECT_MUX = 1u << 2,
    C100_DEBUG_MUX = 3u << 10,
    C100_DEBUG_TX = 1u << 10,
    C100_CS3_FIELDS = 0xff8d,
    C100_CS3_BUS_MASK = 0x008d,
    C100_CS3_X16_ENABLED = 0x0005
};

static int cx805_a7_uses_timer(const Machine *m)
{
    return (m->c100.backlight_a->mux & C100_LCD_ENABLE_MUX) == C100_LCD_TIMER2_PULSE;
}

static unsigned cx805_a7_output(void *context)
{
    Machine *m = context;
    return m->lcd.powered && cx805_a7_uses_timer(m) && gpt_pulse_output(&m->timers[1]) ? C100_LCD_ENABLE : 0;
}

static void cx805_update_pins(Machine *m)
{
    const GpioPort *port_c = m->c100.backlight_c;
    unsigned a = gpio_port_output(m->c100.backlight_a);
    unsigned c = gpio_port_output(port_c);
    unsigned boost = (c & C100_LCD_BRIGHT) ? LCD_LIGHT_SCALE : 0;
    if ((port_c->mux & C100_LCD_BRIGHT_MUX) == C100_LCD_PWM)
        boost = pwm_output(&m->c100.pwm) * LCD_LIGHT_SCALE / PWM_FULL_SCALE;
    unsigned enable = a & C100_LCD_ENABLE ? LCD_LIGHT_SCALE : 0;
    if (cx805_a7_uses_timer(m)) {
        /* Average the pulse train for the panel, while GPIO samples the pin itself. */
        uint32_t period = gpt_pulse_period(&m->timers[1]);
        enable = period ? LCD_LIGHT_SCALE / period : 0;
    }
    lcd_set_backlight(&m->lcd, enable, boost);
    m->c100.keypad_backlight = !!(a & C100_KEYPAD_LIGHT);
    if (m->c100.wired_lcd) {
        unsigned d = gpio_port_output(&m->gpio_ports[GPIO_D]);
        m->lcd.selected = (d & C100_LCD_SELECT) && (m->c100.cs3 & C100_CS3_BUS_MASK) == C100_CS3_X16_ENABLED;
        sds_uart_set_tx_pin(&m->debug_uart, (port_c->mux & C100_DEBUG_MUX) == C100_DEBUG_TX);
    }
}

static void cx805_gpio_changed(void *context, unsigned mask, unsigned output, uint64_t ticks)
{
    Machine *m = context;
    (void)mask;
    (void)output;
    (void)ticks;
    if (m->lcd.powered)
        cx805_update_pins(m);
}

static void cx805_gpio_a_changed(void *context, unsigned mask, unsigned output, uint64_t ticks)
{
    Machine *m = context;
    if (!m->lcd.powered)
        return;
    phone_io_gpio_write(&m->phone_io, mask, output, ticks);
    cx805_update_pins(m);
}

static void cx805_pwm_changed(void *context)
{
    Machine *m = context;
    if (m->lcd.powered)
        cx805_update_pins(m);
}

static void cx805_timer_changed(void *context)
{
    Machine *m = context;
    if (m->lcd.powered && cx805_a7_uses_timer(m))
        cx805_update_pins(m);
}

static int cx805_connect_backlights(Machine *m, GpioPort *port_a, GpioPort *port_c)
{
    Pwm *pwm = &m->c100.pwm;
    if (!pwm_attach(pwm, &m->bus, 0x40a00, &m->clock.nanoseconds))
        return 0;
    pwm->output_changed = cx805_pwm_changed;
    pwm->output_context = m;
    m->c100.backlight_a = port_a;
    m->c100.backlight_c = port_c;
    port_a->output_changed = cx805_gpio_a_changed;
    port_a->output_context = m;
    if (m->timers[1].nanoseconds) {
        port_a->alternate_output = cx805_a7_output;
        m->timers[1].output_changed = cx805_timer_changed;
        m->timers[1].output_context = m;
    }
    port_c->output_changed = cx805_gpio_changed;
    port_c->output_context = m;
    cx805_gpio_a_changed(m, 255, gpio_port_output(port_a), m->bus.ticks);
    return 1;
}

static uint32_t cx805_cs3_read(void *context, uint32_t offset, unsigned width)
{
    Machine *m = context;
    return (m->c100.cs3 >> (8 * offset)) & (width == 1 ? 0xffu : 0xffffu);
}

static void cx805_cs3_write(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    Machine *m = context;
    unsigned shift = 8 * offset;
    unsigned mask = (width == 1 ? 0xffu : 0xffffu) << shift;
    m->c100.cs3 = (uint16_t)(((m->c100.cs3 & ~mask) | ((value << shift) & mask)) & C100_CS3_FIELDS);
    if (m->c100.powered)
        cx805_update_pins(m);
}

static void cx805_cut_uart_power(Machine *m)
{
    sds_uart_cut_power(&m->debug_uart);
    sds_uart_cut_power(&m->sds_uart);
    if (m->debug_uart.tx_log)
        fflush(m->debug_uart.tx_log);
    if (m->sds_uart.tx_log)
        fflush(m->sds_uart.tx_log);
}

static void cx805_power_off(Machine *m)
{
    rtc_advance(&m->rtc);
    m->c100.off_ns = m->clock.nanoseconds;
    m->c100.off_alarm_events = m->rtc.alarm_events;
    m->c100.pending_wake = C100_WAKE_NONE;
    m->pmic.key_supply = 0;
    if (m->rtc.attached)
        m->rtc.nanoseconds = &m->c100.off_ns;
    m->rtc.intc = NULL;
    m->c100.powered = m->c100.keypad_backlight = 0;
    phone_io_gpio_write(&m->phone_io, 255, 0, m->bus.ticks);
    lcd_cut_power(&m->lcd);
    cx805_cut_uart_power(m);
}

static int cx805_check_power(Machine *m)
{
    if (m->clock.nanoseconds >= m->c100.release_ns)
        m->c100.release_ns = UINT64_MAX;
    if (m->c100.release_ns != UINT64_MAX || pmic_holds_power(&m->pmic) || m->rtc.alarm_match)
        return 1;
    cx805_power_off(m);
    return 0;
}

int machine_powered_off(const Machine *m)
{
    return m->loaded && m->c100.enabled && !m->c100.powered;
}

int machine_start_standby(Machine *m)
{
    if (!m->loaded || !m->c100.enabled || !m->c100.powered || m->cpu.halted || m->cpu.instructions)
        return 0;
    if (!m->irom.boot.enabled) {
        /* A RAM upload starts at the loader handoff and cannot cold boot. */
        for (unsigned i = 0; i < m->bus.count; ++i) {
            const Region *r = &m->bus.regions[i];
            if (r->data && !r->readonly && m->reset - r->base < r->size)
                return 0;
        }
    }
    cx805_power_off(m);
    return 1;
}

static void cx805_reset_cpu(Machine *m)
{
    Arm7 *cpu = &m->cpu;
    uint64_t instructions = cpu->instructions, ticks = cpu->cycles, clocks = cpu->clock_cycles;
    int vector_undefined = cpu->vector_undefined;
    arm7_reset(cpu, &m->bus, m->reset);
    cpu->cache = m->decode_cache;
    cpu->vector_undefined = vector_undefined;
    /* Keep debugger and capture timestamps monotonic across resets. */
    cpu->instructions = instructions;
    cpu->cycles = m->bus.ticks = ticks;
    cpu->clock_cycles = m->clock.clock_cycles = clocks;
    m->bus.pc = cpu->last_pc = m->reset;
    m->bus.fault = BUS_OK;
    m->bus.wait_cycles = 0;
}

static void cx805_reset_interrupts(Intc *d, uint64_t ticks)
{
    d->enabled = d->pending = d->fiq_select = 0;
    memset(d->control, 0, sizeof(d->control));
    d->next_tick = ticks + d->period;
}

static void cx805_reset_services(Machine *m)
{
    /* Parked CPU contexts and queued guest work are no longer valid. Host
       settings, SIM contents and capture streams belong to the session. */
    Cellular *cell = &m->cellular;
    cell->head = cell->count = cell->stage = cell->boot_ready = 0;
    cell->name_ptr = cell->data_ptr = cell->native_stack = 0;
    cell->call_state = CELL_IDLE;
    cell->call_id = cell->outgoing = 0;
    cell->network_pending = !!cell->enabled;
    ++cell->revision;
    phone_net_close(&m->network);
    m->network.opened[0] = m->network.opened[1] = m->network.url_slot = 0;
    m->battery.stage = m->guest_busy = 0;
    for (unsigned i = 0; i < m->irom.count; ++i)
        m->irom.services[i].calls = 0;
}

static void cx805_reset(Machine *m, int cold)
{
    const uint64_t now = m->clock.nanoseconds;
    m->c100.reset_pending = 0;
    ++m->c100.reset_count;
    m->c100.reset_cycle = m->cpu.clock_cycles;
    if (m->rtc.attached)
        m->rtc.nanoseconds = &m->clock.nanoseconds;
    cx805_reset_cpu(m);
    if (cold) {
        for (unsigned i = 0; i < m->bus.count; ++i) {
            Region *r = &m->bus.regions[i];
            if (!r->data || r->readonly)
                continue;
            if (r->owned)
                memset(r->data, 0, r->size);
            if (!m->irom.boot.enabled && m->reset - r->base < r->size) {
                m->cpu.halted = 1;
                m->cpu.error = "Cold reset: RAM image must be loaded again";
            }
        }
    } else {
        /* The CX805 restarts with its supply still on. Keep RAM, the PMU and
           the RTC storage words, including the firmware's reset reason. */
        cx805_cut_uart_power(m);
    }
    for (unsigned i = 0; i < m->register_count; ++i)
        m->registers[i].value = m->registers[i].reset_value;
    m->c100.asic_mode.value = m->c100.asic_mode.reset_value;
    memset(m->clock.registers, 0, sizeof(m->clock.registers));
    m->clock.enables = m->clock.enable_reset;
    m->clock.fraction = 0;
    clock_rate(&m->clock);
    for (unsigned i = 0; i < m->flash_count; ++i)
        flash_reset(&m->flashes[i]);
    if (m->irom.boot.enabled) {
        irom_cx805_set_clock(&m->irom.boot, m->clock.arm_hz);
        irom_cx805_reset(&m->irom.boot);
    }

    cx805_reset_interrupts(&m->intc, m->periodic_time ? now : m->cpu.cycles);
    cx805_reset_interrupts(&m->intc_aux, m->cpu.cycles);
    rtc_connect_irq(&m->rtc, &m->intc);
    LpTimer *lp = &m->lp_timer;
    lp->control = lp->captured = lp->compare = lp->observed_enabled = lp->armed = 0;
    lp->next_event = lp->next_cal = lp->wake_due = UINT64_MAX;
    gpt_reset(&m->timers[0]);
    gpt_reset(&m->timers[1]);
    sds_uart_reset(&m->debug_uart);
    sds_uart_reset(&m->sds_uart);
    SdsDma *dma = &m->sds_dma;
    dma->next_event = UINT64_MAX;
    for (unsigned i = 0; i < dma->channels; ++i) {
        SdsDmaChannel *channel = &dma->channel[i];
        *channel = (SdsDmaChannel){.bus = &m->bus, .owner = dma, .base = channel->base};
    }
    if (m->sim.attached)
        sim_cold_reset(&m->sim);
    memset(m->dsp_mailbox.words, 0, sizeof(m->dsp_mailbox.words));
    m->dsp_mailbox.control = m->dsp_mailbox.mux = m->dsp_mailbox.pending_count = 0;

    keypad_reset(&m->keypad);
    for (unsigned key = 0; key < SGH_MAX_KEYS; ++key)
        if (m->keys & (1u << key))
            keypad_key(&m->keypad, key, 1);
    for (unsigned i = 0; i < m->gpio_count; ++i)
        gpio_port_reset(&m->gpio_ports[i]);
    pwm_reset(&m->c100.pwm);
    m->c100.cs3 = 0;
    m->battery.complete_due = UINT64_MAX;
    if (cold) {
        m->battery.complete = 0;
        if (m->battery.complete_port)
            gpio_port_input(m->battery.complete_port, m->battery.complete_bit, 0);
    }
    if (m->battery.enabled && !m->battery.adc)
        m->battery.level_pending = m->battery.cable_pending = 1;
    battery_set(&m->battery, m->battery.percent, m->battery.charging);
    cx805_reset_services(m);

    ma3_cold_reset(&m->ma3);
    PhoneIo *io = &m->phone_io;
    memset(io->phase, 0, sizeof(io->phase));
    memset(io->step, 0, sizeof(io->step));
    memset(io->amplitude, 0, sizeof(io->amplitude));
    memset(io->render_amplitude, 0, sizeof(io->render_amplitude));
    io->last_ns = now;
    io->fraction = 0;
    lcd_reset(&m->lcd);
    clock_apply_enables(m);
    cx805_update_pins(m);
    m->c100.powered = 1;
}

static void cx805_power_on(Machine *m, C100WakeSource source)
{
    uint64_t now = m->clock.nanoseconds = m->c100.off_ns;
    m->c100.pending_wake = C100_WAKE_NONE;
    m->c100.wake_source = source;
    m->c100.release_ns = UINT64_MAX;
    /* A manual start runs on the pressed key until the guest asserts HOLD.
       Charger and RTC starts get the startup pulse even without a held key. */
    if (source != C100_WAKE_KEY && m->c100.key_hold_ns < UINT64_MAX - now)
        m->c100.release_ns = now + m->c100.key_hold_ns;
    /* Register 08 has no VCHARGE bit. Stock Sysol profiles read active-low
       cause bits, unlike the active-high encoding in the CX20524 datasheet. */
    unsigned status = source == C100_WAKE_KEY ? PMIC_POWER_KEY : source == C100_WAKE_RTC ? PMIC_POWER_ALARM : 0;
    pmic_reset(&m->pmic, status ^ m->c100.pmic_wake_xor, source == C100_WAKE_KEY);
    cx805_reset(m, 1);
}

int machine_advance_off(Machine *m, uint64_t elapsed_ns)
{
    if (!machine_powered_off(m))
        return 0;
    uint64_t now = m->c100.off_ns;
    uint64_t due = m->rtc.attached ? m->rtc.alarm_due : UINT64_MAX;
    uint64_t remaining = due > now ? due - now : 0;
    m->c100.off_ns += elapsed_ns < remaining ? elapsed_ns : remaining;
    rtc_advance(&m->rtc);

    C100WakeSource source = m->rtc.alarm_events != m->c100.off_alarm_events ? C100_WAKE_RTC : m->c100.pending_wake;
    if (source == C100_WAKE_NONE)
        return 0;
    cx805_power_on(m, source);
    return 1;
}

void machine_set_battery(Machine *m, unsigned percent, int charging)
{
    if (machine_powered_off(m)) {
        if (charging && !m->battery.charging && m->c100.pending_wake == C100_WAKE_NONE)
            m->c100.pending_wake = C100_WAKE_CHARGER;
        if (!charging && m->c100.pending_wake == C100_WAKE_CHARGER)
            m->c100.pending_wake = C100_WAKE_NONE;
        /* These host inputs survive power loss. The GPIO/ADC see them at reset. */
        m->battery.percent = percent > 100 ? 100 : percent;
        m->battery.charging = !!charging;
        return;
    }
    battery_set(&m->battery, percent, charging);
}

static int cx805_attach_keypad(Machine *m)
{
    static const uint8_t switches[SGH_MAX_KEYS][2] = {
        {7, 0}, {7, 1}, {7, 2}, {6, 0}, {7, 0}, {6, 2}, {5, 0}, {5, 1}, {0, 3},
        {4, 0}, {4, 1}, {4, 2}, {3, 0}, {3, 1}, {3, 2}, {2, 0}, {2, 1}, {2, 2},
        {1, 0}, {1, 1}, {1, 2}, {5, 2},
    };
    if (!keypad_attach(&m->keypad, &m->bus, 0x40800, 8, 4))
        return 0;
    for (unsigned i = 0; i < SGH_MAX_KEYS; ++i)
        if (!keypad_map(&m->keypad, i, switches[i][0], switches[i][1]))
            return 0;
    return 1;
}

static int cx805_attach_board(Machine *m)
{
    static const struct {
        uint32_t data, control, mux;
        unsigned output_reset;
        uint16_t gpio_modes;
    } pins[] = {
        /* GRPA_DAT resets to F0xx per CX805-30 Table 7; the drivers are tri-stated. */
        [GPIO_A] = {C100_GPIO_A_DATA, C100_GPIO_A_CONTROL, C100_GPIO_A_MUX, 0xf0, 0},
        [GPIO_B] = {0x4028e, 0x40286, 0x40d06, 0, 0},
        [GPIO_C] = {C100_GPIO_C_DATA, C100_GPIO_C_CONTROL, C100_GPIO_C_MUX, 0, 0},
        [GPIO_D] = {0x40292, 0x4028a, 0x40d0a, 0, C100_LCD_SELECT_MUX},
        [GPIO_E] = {0x4029c, 0x4029a, 0x40d1c, 0, 0},
    };

    if (!m->clock.attached || !m->lcd.attached || !m->pmic.attached || m->gpio_count ||
        !bus_device(&m->bus, 0x40016, 2, cx805_cs3_read, cx805_cs3_write, m) ||
        (!m->keypad.attached && !cx805_attach_keypad(m)))
        return 0;
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        GpioPort *port = &m->gpio_ports[i];
        if (!gpio_port_attach(port, &m->bus, pins[i].data, pins[i].output_reset) ||
            !gpio_port_attach_pins(port, pins[i].control, pins[i].mux, 0, pins[i].gpio_modes))
            return 0;
        ++m->gpio_count;
    }
    m->gpio_ports[GPIO_D].output_changed = cx805_gpio_changed;
    m->gpio_ports[GPIO_D].output_context = m;
    m->phone_io.led_pins[0] = 5;
    m->phone_io.led_pins[1] = 3;
    m->phone_io.led_pins[2] = 6;
    m->phone_io.hardware_led = 1;
    m->lcd.byte_commands = 1;
    m->c100.wired_lcd = 1;
    return cx805_connect_backlights(m, &m->gpio_ports[GPIO_A], &m->gpio_ports[GPIO_C]);
}

static GpioPort *find_gpio_port(Machine *m, uint32_t base)
{
    for (unsigned i = m->gpio_count; i > 0; --i) {
        GpioPort *port = &m->gpio_ports[i - 1];
        if (port->base == base)
            return port;
    }
    return NULL;
}

static int cx805_attach_backlights(Machine *m)
{
    GpioPort *a = find_gpio_port(m, C100_GPIO_A_DATA), *c;
    if (!m->lcd.attached || !a || a->output_changed != phone_io_gpio_write || m->gpio_count == 5)
        return 0;
    c = &m->gpio_ports[m->gpio_count];
    if (!gpio_port_attach_pins(a, C100_GPIO_A_CONTROL, C100_GPIO_A_MUX, 0, 0) ||
        !gpio_port_attach(c, &m->bus, C100_GPIO_C_DATA, 0) ||
        !gpio_port_attach_pins(c, C100_GPIO_C_CONTROL, C100_GPIO_C_MUX, 0, 0))
        return 0;
    ++m->gpio_count;
    return cx805_connect_backlights(m, a, c);
}

static int cx805_attach_nor_boot(Machine *m)
{
    Flash *first = NULL, *second = NULL;
    size_t image_size = 0;
    for (unsigned i = 0; i < m->flash_count; ++i) {
        if (m->flashes[i].base == C100_NOR1)
            first = &m->flashes[i];
        else if (m->flashes[i].base == C100_NOR2)
            second = &m->flashes[i];
    }
    for (unsigned i = 0; i < m->rom_count; ++i)
        if (m->roms[i].base == C100_NOR1)
            image_size = m->roms[i].size;
    return m->clock.attached && m->reset == 0 &&
           irom_cx805_attach(&m->irom.boot, &m->bus, first, second, image_size, m->clock.arm_hz);
}

int machine_cx805_status(const Machine *m, char *text, size_t size)
{
    static const char *const levels[] = {"off", "normal", "bright", "PWM"};
    if (!m->c100.backlight_a)
        return 0;
    const char *backlight = levels[m->lcd.backlight];
    if (m->lcd.powered && cx805_a7_uses_timer(m))
        backlight = !gpt_pulse_period(&m->timers[1]) ? "A7 Timer 2 pulse (idle)"
                   : gpt_pulse_output(&m->timers[1]) ? "A7 Timer 2 pulse (high)"
                                                    : "A7 Timer 2 pulse (low)";
    const char *keypad = m->c100.keypad_backlight ? "on" : "off";
    const char *motor = ma3_motor_on(&m->ma3) && m->lcd.powered ? "on" : "off";
    if (!m->c100.enabled)
        return snprintf(text, size,
                        "C100 LCD backlight=%s, keypad backlight=%s, LCD contrast=0x%02x, vibrator=%s",
                        backlight, keypad, (unsigned)m->lcd.contrast, motor);
    const char *power = "standby";
    if (m->c100.powered) {
        if (m->pmic.key_supply || m->c100.release_ns != UINT64_MAX) {
            power = m->c100.wake_source == C100_WAKE_CHARGER ? "charger startup"
                    : m->c100.wake_source == C100_WAKE_RTC ? "RTC startup"
                                                         : "key held";
        } else if (m->rtc.alarm_match && !pmic_holds_power(&m->pmic))
            power = "RTC alarm";
        else
            power = "latched";
    }
    static const char *const last_boot[] = {"", ", last boot=Power key", ", last boot=charger", ", last boot=RTC"};
    return snprintf(
        text, size,
        "C100 power=%s, LCD select=%s, LCD backlight=%s, keypad backlight=%s, RGB=%u, LCD contrast=0x%02x, vibrator=%s%s",
        power, m->lcd.selected ? "on" : "off", backlight, keypad, m->phone_io.led,
        (unsigned)m->lcd.contrast, motor, last_boot[m->c100.wake_source]);
}

void machine_destroy(Machine *m)
{
    if (m->audio.ops)
        m->audio.ops->stop(&m->audio);
    phone_net_close(&m->network);
    ma3_destroy(&m->ma3);
    arm7_cache_destroy(m->decode_cache);
    phone_io_close(&m->phone_io);
    bus_destroy(&m->bus);
    free(m->pixels);
    free(m->unplaced_rom);
    memset(m, 0, sizeof(*m));
}

int machine_load(Machine *m, const char *path, const char *rom_override, FILE *log)
{
    return machine_load_pair(m, path, rom_override, NULL, log);
}

int machine_load_pair(Machine *m, const char *path, const char *rom_override, const char *second_override,
                      FILE *log)
{
    FILE *f;
    char line[2048];
    unsigned line_no = 0;
    int boot_image = 0, reset_set = 0, valid = 1, second_used = 0, vector_undefined = 0;
    int c100_backlights = 0;
    machine_destroy(m);
    cellular_init(&m->cellular);
    m->bus.log = log;
    m->audio = audio_silent();
    m->width = 128;
    m->height = 128;
    if (strlen(path) >= sizeof(m->config_path)) {
        strcpy(m->error, "Model path is too long");
        return 0;
    }
    strcpy(m->config_path, path);
    f = fopen(path, "r");
    if (!f) {
        snprintf(m->error, sizeof(m->error), "Cannot open model config: %.180s", path);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *key = trim(line), *value, *comma;
        uint32_t base, size;
        ++line_no;
        if (!*key || *key == '#')
            continue;
        value = strchr(key, '=');
        if (!value) {
            valid = 0;
            break;
        }
        *value++ = 0;
        key = trim(key);
        value = trim(value);
        if (!strcmp(key, "phone_io") || !strcmp(key, "phone_tones")) {
            int tones_only = !strcmp(key, "phone_tones");
            uint32_t fields[7];
            if (!number_list(value, fields, tones_only ? 3 : 7, 0)) {
                valid = 0;
                break;
            }
            m->phone_io.tone_entry = fields[0];
            m->phone_io.tone_stop = fields[1];
            m->phone_io.tone_table = fields[2];
            if (!tones_only) {
                m->phone_io.gpio_entry = fields[3];
                if (!m->phone_io.hardware_led) {
                    m->phone_io.led_pins[0] = fields[4];
                    m->phone_io.led_pins[1] = fields[5];
                    m->phone_io.led_pins[2] = fields[6];
                }
            }
            m->phone_io.configured = 1;
            continue;
        }
        if (!strcmp(key, "name")) {
            if (strlen(value) >= sizeof(m->name)) {
                valid = 0;
                break;
            }
            strcpy(m->name, value);
            continue;
        }
        if (!strcmp(key, "reset")) {
            if (!number(value, &m->reset)) {
                valid = 0;
                break;
            }
            reset_set = 1;
            continue;
        }
        if (!strcmp(key, "c100_power_key_ms")) {
            if (m->c100.enabled || !number(value, &size)) {
                valid = 0;
                break;
            }
            m->c100.enabled = 1;
            m->c100.key_hold_ns = (uint64_t)size * 1000000u;
            continue;
        }
        if (!strcmp(key, "pmic_wake_xor")) {
            if (m->c100.pmic_wake_xor || !number(value, &size) || !size || size > 15) {
                valid = 0;
                break;
            }
            m->c100.pmic_wake_xor = (uint8_t)size;
            continue;
        }
        if (!strcmp(key, "c100_backlights")) {
            if (c100_backlights || strcmp(value, "1")) {
                valid = 0;
                break;
            }
            c100_backlights = 1;
            continue;
        }
        if (!strcmp(key, "c100_nor_boot")) {
            if (m->irom.boot.enabled || strcmp(value, "1")) {
                valid = 0;
                break;
            }
            m->irom.boot.enabled = 1;
            continue;
        }
        if (!strcmp(key, "exceptions")) {
            if (strcmp(value, "arm7")) {
                valid = 0;
                break;
            }
            vector_undefined = 1;
            continue;
        }
        if (!strcmp(key, "audio")) {
            if (strcmp(value, "silent")) {
                valid = 0;
                break;
            }
            continue;
        }
        if (!strcmp(key, "cellular_select")) {
            if (m->cellular.radio_select || !number(value, &base) || !base || (base & 1)) {
                valid = 0;
                break;
            }
            m->cellular.radio_select = base;
            continue;
        }
        if (!strcmp(key, "sim_present")) {
            uint32_t present;
            if (!m->sim.attached || !number(value, &present) || present > 1) {
                valid = 0;
                break;
            }
            m->sim.present = (int)present;
            continue;
        }
        if (!strcmp(key, "sim_card")) {
            char card_path[1024];
            if (!m->sim.attached || !image_path(card_path, sizeof(card_path), path, value) ||
                !sim_card_load(&m->sim.card, card_path)) {
                valid = 0;
                break;
            }
            m->sim.card.response_size = 0;
            continue;
        }
        if (!strcmp(key, "diagnostic")) {
            if (strcmp(value, "continue")) {
                valid = 0;
                break;
            }
            m->bringup = 1;
            continue;
        }
        if (!strcmp(key, "intc")) {
            uint32_t address;
            if (m->intc.attached || !number(value, &address) || !intc_attach(&m->intc, &m->bus, address)) {
                valid = 0;
                break;
            }
            continue;
        }
        if (!strcmp(key, "intc_aux")) {
            uint32_t address;
            if (!m->intc.attached || m->intc_aux.attached || !number(value, &address) ||
                !intc_attach(&m->intc_aux, &m->bus, address)) {
                valid = 0;
                break;
            }
            m->intc.secondary = &m->intc_aux;
            continue;
        }
        if (!strcmp(key, "unplaced_rom")) {
            if (m->unplaced_rom ||
                !image_path(m->unplaced_path, sizeof(m->unplaced_path), second_override ? "" : path,
                            second_override ? second_override : value) ||
                !file_read(m->unplaced_path, &m->unplaced_rom, &m->unplaced_size)) {
                valid = 0;
                break;
            }
            second_used = 1;
            continue;
        }
        comma = strchr(value, ',');
        if (!comma) {
            valid = 0;
            break;
        }
        *comma++ = 0;
        if (!number(value, &base)) {
            valid = 0;
            break;
        }
        if (!strcmp(key, "rom")) {
            const char *override = NULL;
            if (m->rom_count == 0)
                override = rom_override;
            else if (m->rom_count == 1)
                override = second_override;
            if (!load_rom_image(m, base, comma, override)) {
                valid = 0;
                break;
            }
            if (m->rom_count == 2)
                second_used = 1;
            boot_image = 1;
        } else if (!strcmp(key, "irom_service")) {
            if (!irom_add(&m->irom, base, trim(comma))) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "irom_version")) {
            char *next = strchr(comma, ',');
            uint32_t third;
            if (!next || m->irom.version_configured) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || size > 65535 || !number(next, &third) || (third & 1) ||
                !irom_add(&m->irom, base, "version")) {
                valid = 0;
                break;
            }
            m->irom.version = (uint16_t)size;
            m->irom.version_return = third;
            m->irom.version_configured = 1;
        } else if (!strcmp(key, "keypad") || !strcmp(key, "key")) {
            char *next = strchr(comma, ',');
            uint32_t column;
            if (!next) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !number(next, &column)) {
                valid = 0;
                break;
            }
            if (!strcmp(key, "keypad")) {
                if (!keypad_attach(&m->keypad, &m->bus, base, size, column)) {
                    valid = 0;
                    break;
                }
            } else if (base >= SGH_MAX_KEYS || !keypad_map(&m->keypad, base, size, column)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "ma3")) {
            char *next = strchr(comma, ',');
            uint32_t quantum;
            if (!next) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !number(next, &quantum) ||
                !ma3_attach(&m->ma3, &m->bus, &m->intc, base, size, quantum)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "sim")) {
            char *next = strchr(comma, ',');
            uint32_t dma;
            if (!next) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !number(next, &dma) ||
                !sim_attach(&m->sim, &m->bus, &m->intc, base, size, dma)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "nv_provision")) {
            char *next = strchr(comma, ',');
            uint32_t marker;
            if (!next || m->nv_trigger || !base || (base & 1)) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !size || (size & 1) || !number(next, &marker) || !marker ||
                (marker & 1)) {
                valid = 0;
                break;
            }
            m->nv_trigger = base;
            m->nv_format = size;
            m->nv_marker = marker;
        } else if (!strcmp(key, "import")) {
            char *second = strchr(comma, ','), *third;
            uint32_t alloc, search, write;
            if (!second || m->import_trigger || !base || (base & 1)) {
                valid = 0;
                break;
            }
            *second++ = 0;
            third = strchr(second, ',');
            if (!third) {
                valid = 0;
                break;
            }
            *third++ = 0;
            if (!number(comma, &alloc) || !alloc || (alloc & 1) || !number(second, &search) || !search ||
                (search & 1) || !number(third, &write) || !write || (write & 1)) {
                valid = 0;
                break;
            }
            m->import_trigger = base;
            m->import_alloc = alloc;
            m->import_search = search;
            m->import_write = write;
        } else if (!strcmp(key, "java_import")) {
            uint32_t fields[5];
            if (m->java_ready || !base || (base & 1) || !number_list(comma, fields, 5, 1) ||
                (fields[0] & 3) || (fields[1] & 1) || (fields[2] & 1)) {
                valid = 0;
                break;
            }
            m->java_ready = base;
            m->java_globals = fields[0];
            m->java_set_jar = fields[1];
            m->java_send_ui = fields[2];
            m->java_message = fields[3];
            m->java_min_instructions = fields[4];
        } else if (!strcmp(key, "cellular")) {
            uint32_t fields[8];
            if (m->cellular.ready || !base || (base & 1) || !number_list(comma, fields, 8, 1) ||
                (fields[7] & 3)) {
                valid = 0;
                break;
            }
            for (unsigned j = 0; j < 7; ++j) {
                if (fields[j] & 1) {
                    valid = 0;
                    break;
                }
            }
            if (!valid)
                break;
            m->cellular.ready = base;
            m->cellular.alloc = fields[0];
            m->cellular.release = fields[1];
            m->cellular.send_ui = fields[2];
            m->cellular.dial = fields[3];
            m->cellular.accept = fields[4];
            m->cellular.disconnect = fields[5];
            m->cellular.name_lookup = fields[6];
            m->cellular.services = fields[7];
        } else if (!strcmp(key, "cellular_alert")) {
            if (!base || (base & 1) || m->cellular.alert_request || !number(comma, &size) || size) {
                valid = 0;
                break;
            }
            m->cellular.alert_request = base;
        } else if (!strcmp(key, "cellular_poll")) {
            if (!base || (base & 1) || m->cellular.poll || !number(comma, &size) || size) {
                valid = 0;
                break;
            }
            m->cellular.poll = base;
        } else if (!strcmp(key, "java_net")) {
            uint32_t *entry = m->network.java.entry;
            if (entry[0] || !base || (base & 1) || !number_list(comma, entry + 1, JN_COUNT - 1, 1)) {
                valid = 0;
                break;
            }
            entry[0] = base;
            m->network.java.first = UINT32_MAX;
            for (unsigned j = 0; j < JN_COUNT; ++j) {
                uint32_t pc = entry[j];
                if (pc & 1) {
                    valid = 0;
                    break;
                }
                if (j == JN_NOTIFY)
                    continue;
                if (pc < m->network.java.first)
                    m->network.java.first = pc;
                if (pc > m->network.java.last)
                    m->network.java.last = pc;
            }
            if (!valid)
                break;
        } else if (!strcmp(key, "wap")) {
            uint32_t fields[7];
            if (m->network.txn_start || !base || (base & 1) || !number_list(comma, fields, 7, 1) ||
                (fields[5] & 3) || (fields[6] & 3)) {
                valid = 0;
                break;
            }
            for (unsigned j = 0; j < 5; ++j) {
                if (fields[j] & 1) {
                    valid = 0;
                    break;
                }
            }
            if (!valid)
                break;
            m->network.txn_start = base;
            m->network.send_pdu = fields[0];
            m->network.recv_pdu = fields[1];
            m->network.bearer_state = fields[2];
            m->network.post_event = fields[3];
            m->network.data_arrived = fields[4];
            m->network.wap_base = fields[5];
            m->network.driver_base = fields[6];
        } else if (!strcmp(key, "cellular_sms")) {
            uint32_t fields[3];
            if (m->cellular.sms_send || !base || (base & 1) || !number_list(comma, fields, 3, 1) ||
                (fields[0] & 1) || (fields[1] & 1) || (fields[2] & 3)) {
                valid = 0;
                break;
            }
            m->cellular.sms_send = base;
            m->cellular.sms_reply = fields[0];
            m->cellular.send_message = fields[1];
            m->cellular.sr_task = fields[2];
        } else if (!strcmp(key, "java_buffers")) {
            uint32_t fields[4];
            if (m->java_buffer_get || !base || (base & 1) || !number_list(comma, fields, 4, 1) ||
                (fields[0] & 1) || (fields[1] & 3) || (fields[2] & 3) || fields[3] > 0x100000u ||
                (fields[3] & 3) || (uint64_t)fields[2] + 3u * fields[3] > UINT64_C(0x100000000)) {
                valid = 0;
                break;
            }
            m->java_buffer_get = base;
            m->java_buffer_set = fields[0];
            m->java_buffer_table = fields[1];
            m->java_buffer_base = fields[2];
            m->java_buffer_size = fields[3];
        } else if (!strcmp(key, "nvram")) {
            if (m->nvram_size || !number(comma, &size) || !size || (base & 1) || (size & 1)) {
                valid = 0;
                break;
            }
            m->nvram_base = base;
            m->nvram_size = size;
        } else if (!strcmp(key, "userfs")) {
            char *next = strchr(comma, ','), *last;
            uint32_t capacity, stride;
            if (!next || m->fs_header) {
                valid = 0;
                break;
            }
            *next++ = 0;
            last = strchr(next, ',');
            if (!last) {
                valid = 0;
                break;
            }
            *last++ = 0;
            if (!base || !number(comma, &size) || !number(next, &capacity) || !number(last, &stride) ||
                !capacity || capacity > 100 || stride != 28) {
                valid = 0;
                break;
            }
            m->fs_header = base;
            m->fs_table = size;
            m->fs_capacity = capacity;
            m->fs_stride = stride;
        } else if (!strcmp(key, "lp_timer")) {
            uint32_t fields[4];
            if (!number_list(comma, fields, 4, 0) ||
                !lp_timer_attach(&m->lp_timer, &m->bus, &m->intc, &m->clock.nanoseconds, base, fields[0],
                                 fields[1], fields[2], fields[3])) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "dsp_mailbox")) {
            if (!number(comma, &size) || !dsp_mailbox_attach(&m->dsp_mailbox, &m->bus, base, size)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "battery_adc")) {
            char *next = strchr(comma, ',');
            uint32_t temperature;
            if (m->battery.enabled || !m->dsp_mailbox.attached || !next) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (base > 255 || !number(comma, &size) || size > 255 || base > size ||
                !number(next, &temperature) || temperature > 255) {
                valid = 0;
                break;
            }
            m->battery.adc = &m->dsp_mailbox;
            m->battery.adc_empty = base;
            m->battery.adc_full = size;
            m->battery.adc_temperature = temperature;
            m->battery.enabled = 1;
            battery_set(&m->battery, 100, 0);
        } else if (!strcmp(key, "gpio_port")) {
            if (m->gpio_count == 5 || !number(comma, &size) ||
                !gpio_port_attach(&m->gpio_ports[m->gpio_count], &m->bus, base, size)) {
                valid = 0;
                break;
            }
            ++m->gpio_count;
        } else if (!strcmp(key, "gpio_led")) {
            uint32_t pins[3];
            GpioPort *port = find_gpio_port(m, base);
            if (!port || m->phone_io.hardware_led || !number_list(comma, pins, 3, 0) || pins[0] >= 8 ||
                pins[1] >= 8 || pins[2] >= 8 || pins[0] == pins[1] || pins[0] == pins[2] ||
                pins[1] == pins[2]) {
                valid = 0;
                break;
            }
            for (unsigned j = 0; j < 3; ++j)
                m->phone_io.led_pins[j] = pins[j];
            m->phone_io.hardware_led = 1;
            port->output_changed = phone_io_gpio_write;
            port->output_context = &m->phone_io;
            phone_io_gpio_write(&m->phone_io, 255, port->output, 0);
        } else if (!strcmp(key, "charger_complete")) {
            uint32_t fields[5];
            GpioPort *port = find_gpio_port(m, base);
            if (!port || port->output_changed || m->battery.complete_port || !m->clock.attached ||
                !number_list(comma, fields, 5, 0) || fields[0] >= 8 || fields[1] >= 8 ||
                fields[0] == fields[1] || fields[2] > 1 || fields[3] >= 32 ||
                !(fields[2] ? m->intc_aux.attached : m->intc.attached)) {
                valid = 0;
                break;
            }
            m->battery.complete_port = port;
            m->battery.complete_intc = fields[2] ? &m->intc_aux : &m->intc;
            m->battery.complete_bit = fields[0];
            m->battery.enable_bit = fields[1];
            m->battery.complete_irq = fields[3];
            m->battery.complete_delay_ms = fields[4];
            m->battery.complete_due = UINT64_MAX;
            m->battery.nanoseconds = &m->clock.nanoseconds;
            gpio_port_input(port, fields[0], 0);
            port->output_changed = battery_charger_output;
            port->output_context = &m->battery;
            battery_set(&m->battery, m->battery.percent, (int)m->battery.charging);
        } else if (!strcmp(key, "charger_input")) {
            if (!m->gpio_count || !m->intc.attached || m->battery.charger_port || base >= 8 ||
                !number(comma, &size) || size >= 32) {
                valid = 0;
                break;
            }
            m->battery.charger_port = &m->gpio_ports[m->gpio_count - 1];
            m->battery.charger_intc = &m->intc;
            m->battery.charger_bit = base;
            m->battery.charger_irq = size;
            gpio_port_input(m->battery.charger_port, base, !m->battery.charging);
        } else if (!strcmp(key, "battery_report")) {
            char *next = strchr(comma, ',');
            uint32_t report;
            if (m->battery.enabled || !base || (base & 1) || !next) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !size || (size & 1) || !number(next, &report) || !report ||
                (report & 1)) {
                valid = 0;
                break;
            }
            m->battery.ready = base;
            m->battery.alloc = size;
            m->battery.report = report;
            m->battery.enabled = 1;
            m->battery.percent = 100;
            m->battery.level_pending = 1;
        } else if (!strcmp(key, "battery_guest")) {
            uint32_t fields[5];
            if (m->battery.enabled || !base || (base & 1) || !number_list(comma, fields, 5, 1) ||
                (fields[0] & 1) || (fields[1] & 1) || (fields[2] & 1) || (fields[3] & 1)) {
                valid = 0;
                break;
            }
            m->battery.ready = base;
            m->battery.alloc = fields[0];
            m->battery.report = fields[1];
            m->battery.detect = fields[2];
            m->battery.gpio = fields[3];
            m->battery.pin = fields[4];
            m->battery.enabled = 1;
            m->battery.percent = 100;
            m->battery.level_pending = 1;
        } else if (!strcmp(key, "rtc")) {
            if (!number(comma, &size) || !rtc_attach(&m->rtc, &m->bus, base, size, &m->clock.nanoseconds)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "clock")) {
            if (m->clock.attached || base & 1 || !number(comma, &size) || size < 1000000 ||
                size > 100000000) {
                valid = 0;
                break;
            }
            m->clock.bus = &m->bus;
            m->clock.base = base;
            m->clock.reference_hz = m->clock.arm_hz = m->clock.dsp_hz = size;
            if (!bus_device(&m->bus, base, 6, clock_read, clock_write, m)) {
                valid = 0;
                break;
            }
            m->clock.attached = 1;
        } else if (!strcmp(key, "clock_mode")) {
            char *next = strchr(comma, ',');
            uint32_t dsp;
            unsigned index = m->clock.count;
            if (!m->clock.attached || !next || index == 8 || base > 65535) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !number(next, &dsp) || size < 1000000 || size > 100000000 ||
                dsp < 1000000 || dsp > 100000000) {
                valid = 0;
                break;
            }
            m->clock.encoding[index] = (uint16_t)base;
            m->clock.arm_rates[index] = size;
            m->clock.dsp_rates[index] = dsp;
            ++m->clock.count;
        } else if (!strcmp(key, "clock_control")) {
            if (m->clock.enable_base || !base || (base & 1) || !number(comma, &size) || size > 0xffff ||
                !bus_device(&m->bus, base, 2, clock_enable_read, clock_enable_write, m)) {
                valid = 0;
                break;
            }
            m->clock.enable_base = base;
            m->clock.enables = m->clock.enable_reset = (uint16_t)size;
        } else if (!strcmp(key, "uart")) {
            int debug_port = !strcmp(trim(comma), "debug");
            SdsUart *uart = debug_port ? &m->debug_uart : &m->sds_uart;
            if ((!debug_port && strcmp(trim(comma), "sds")) || !m->clock.attached ||
                !sds_uart_attach_timed(uart, &m->bus, &m->intc, base, &m->clock.nanoseconds,
                                       m->clock.reference_hz, debug_port)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "timer")) {
            uint32_t number_on_chip, hz = m->clock.reference_hz, etu_divider = 0;
            char *next = strchr(comma, ',');
            if (next) {
                *next++ = 0;
                char *divider = strchr(next, ',');
                if (divider)
                    *divider++ = 0;
                if (!number(next, &hz) || !hz || (divider && (!number(divider, &etu_divider) || !etu_divider))) {
                    valid = 0;
                    break;
                }
            }
            if (!m->clock.attached || !number(comma, &number_on_chip) || number_on_chip < 1 ||
                number_on_chip > 2 ||
                !gpt_attach(&m->timers[number_on_chip - 1], &m->bus, &m->intc, base, number_on_chip,
                            &m->clock.nanoseconds, &m->cpu.clock_cycles, hz, etu_divider)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "sds_uart")) {
            uint32_t cable;
            if (m->sds_uart.attached || !m->intc.attached || !number(comma, &cable) || cable > 1 ||
                !sds_uart_attach(&m->sds_uart, &m->bus, &m->intc, base, (int)cable)) {
                valid = 0;
                break;
            }
            /* The model always carries the port; the bench cable only exists in a
               service session, so a normal boot never loops a byte back. */
            m->sds_uart.cable = sgh_service_mode ? (int)cable : 0;
        } else if (!strcmp(key, "sds_dma")) {
            uint32_t channels, start_delay = 0, byte_time = 0;
            char *next = strchr(comma, ',');
            char *more = next ? strchr(next + 1, ',') : 0;
            if (more)
                *more++ = 0;
            if (next)
                *next++ = 0;
            /* The general DMA controller shares its register block with the SIM
               device's channel and only matters for the service loopback, so it
               is left unmapped outside a service session; that keeps a normal
               boot byte-for-byte unchanged. */
            if (!sgh_service_mode)
                continue;
            if (m->sds_dma.attached || !number(comma, &channels) || !channels ||
                channels > SDS_DMA_MAX_CHANNELS || (next && !number(next, &start_delay)) ||
                (more && !number(more, &byte_time)) ||
                !sds_dma_attach(&m->sds_dma, &m->bus, m->sds_uart.attached ? &m->sds_uart : 0, base, channels,
                                start_delay, byte_time)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "periodic_irq") || !strcmp(key, "periodic_time_irq")) {
            m->periodic_time = !strcmp(key, "periodic_time_irq");
            if (!m->intc.attached || base > 31 || !number(comma, &size) || !size) {
                valid = 0;
                break;
            }
            m->intc.periodic_bit = 1u << base;
            m->intc.period = size;
            m->intc.next_tick = size;
        } else if (!strcmp(key, "flash")) {
            char *next = strchr(comma, ',');
            uint32_t device;
            if (!next || m->flash_count == 4) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !number(next, &device) || size > 65535 || device > 65535 ||
                !flash_attach(&m->flashes[m->flash_count], &m->bus, base, (uint16_t)size, (uint16_t)device)) {
                valid = 0;
                break;
            }
            ++m->flash_count;
        } else if (!strcmp(key, "flash_geometry")) {
            char *next = strchr(comma, ','), *last;
            uint32_t length, block_size;
            unsigned i;
            if (!next) {
                valid = 0;
                break;
            }
            *next++ = 0;
            last = strchr(next, ',');
            if (!last) {
                valid = 0;
                break;
            }
            *last++ = 0;
            if (!number(comma, &size) || !number(next, &length) || !number(last, &block_size)) {
                valid = 0;
                break;
            }
            for (i = 0; i < m->flash_count; ++i)
                if (m->flashes[i].base == base)
                    break;
            if (i == m->flash_count || !flash_geometry(&m->flashes[i], size, length, block_size)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "lcd_s6b33")) {
            char *next = strchr(comma, ',');
            uint32_t origin;
            if (!next || m->lcd.attached) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &size) || !number(next, &origin) ||
                !lcd_attach(&m->lcd, &m->bus, base, size, origin)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "pmic")) {
            if (m->pmic.attached || !number(comma, &size) || !pmic_attach(&m->pmic, &m->bus, base, size)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "pmic_register")) {
            if (!m->pmic.attached || base > 255 || !number(comma, &size) || size > 255) {
                valid = 0;
                break;
            }
            m->pmic.regs[base] = m->pmic.reset_regs[base] = (uint8_t)size;
        } else if (!strcmp(key, "irq_gate")) {
            if ((base & 1) || m->irq_gate_mask || !number(comma, &size) || !size || size > 65535) {
                valid = 0;
                break;
            }
            m->irq_gate_address = base;
            m->irq_gate_mask = size;
        } else if (!strcmp(key, "register32") || !strcmp(key, "irom_vector")) {
            Register32 *r;
            if ((base & 3) || !number(comma, &size) || m->register_count == SGH_MAX_IDENTITIES) {
                valid = 0;
                break;
            }
            r = &m->registers[m->register_count++];
            r->bus = &m->bus;
            r->address = base;
            r->value = r->reset_value = size;
            r->irom_vector = !strcmp(key, "irom_vector");
            if (r->irom_vector && (base < 0x80000 || base >= 0x82000 || !(size & 1))) {
                valid = 0;
                break;
            }
            if (!bus_device(&m->bus, base, 4, register_read, register_write, r)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "bus_abort")) {
            if (!number(comma, &size) || !bus_map_abort(&m->bus, base, size)) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "ram")) {
            char *image = strchr(comma, ',');
            if (image)
                *image++ = 0;
            if (!number(comma, &size) || !size || size > 64u * 1024 * 1024) {
                valid = 0;
                break;
            }
            uint8_t *data = calloc(size, 1);
            if (!data || (image && !load_ram_image(data, size, path, trim(image))) ||
                !bus_map(&m->bus, base, size, data, 0, 1)) {
                free(data);
                valid = 0;
                break;
            }
            if (image)
                boot_image = 1;
        } else if (!strcmp(key, "alias")) {
            char *next = strchr(comma, ',');
            uint32_t source;
            unsigned i;
            int found = 0;
            if (!next) {
                valid = 0;
                break;
            }
            *next++ = 0;
            if (!number(comma, &source) || !number(next, &size)) {
                valid = 0;
                break;
            }
            for (i = 0; i < m->bus.count; ++i) {
                Region *r = &m->bus.regions[i];
                if (r->data && source >= r->base && (uint64_t)source + size <= (uint64_t)r->base + r->size) {
                    found = bus_map(&m->bus, base, size, r->data + source - r->base, r->readonly, 0);
                    break;
                }
            }
            if (!found) {
                valid = 0;
                break;
            }
        } else if (!strcmp(key, "display")) {
            if (!number(comma, &size) || !base || !size || base > 512 || size > 512) {
                valid = 0;
                break;
            }
            m->width = base;
            m->height = size;
        } else if (!strcmp(key, "identity16")) {
            Identity16 *id;
            if ((base & 1) || !number(comma, &size) || size > 65535 ||
                m->identity_count == SGH_MAX_IDENTITIES) {
                valid = 0;
                break;
            }
            id = &m->identities[m->identity_count++];
            id->bus = &m->bus;
            id->address = base;
            id->value = (uint16_t)size;
            if (!bus_device(&m->bus, base, 2, identity_read, identity_write, id)) {
                valid = 0;
                break;
            }
        } else {
            valid = 0;
            break;
        }
    }
    if (ferror(f) || (m->lp_timer.attached && !m->clock.attached))
        valid = 0;
    if ((m->debug_uart.nanoseconds || m->sds_uart.nanoseconds || m->timers[1].nanoseconds) &&
        !m->clock.enable_base)
        valid = 0;
    fclose(f);
    if (valid) {
        unsigned i, j;
        for (i = 0; i < m->irom.count; ++i) {
            IromService *s = &m->irom.services[i];
            if (s->kind == IROM_VERSION && !m->irom.version_configured)
                valid = 0;
            for (j = 0; j < m->bus.count; ++j) {
                Region *r = &m->bus.regions[j];
                if (s->address >= r->base && (uint64_t)s->address < (uint64_t)r->base + r->size)
                    valid = 0;
            }
        }
    }
    if (valid) {
        unsigned i, j;
        for (i = 0; i < m->register_count; ++i)
            if (m->registers[i].irom_vector) {
                for (j = 0; j < m->irom.count; ++j)
                    if (m->irom.services[j].address == (m->registers[i].value & ~1u))
                        break;
                if (j == m->irom.count)
                    valid = 0;
            }
    }
    if (valid && m->network.java.first) {
        unsigned i, j;
        if (!m->cellular.ready || !m->cellular.poll)
            valid = 0;
        for (i = 0; i < JN_COUNT; ++i) {
            uint32_t pc = m->network.java.entry[i];
            for (j = 0; j < i; ++j)
                if (pc == m->network.java.entry[j])
                    valid = 0;
            if (!rom_contains(m, pc, 2))
                valid = 0;
        }
    }
    if (valid && (m->cellular.ready || m->cellular.sms_send || m->network.txn_start || m->cellular.poll)) {
        unsigned i, j;
        uint32_t code[] = {m->cellular.ready,         m->cellular.alloc,        m->cellular.release,
                           m->cellular.send_ui,       m->cellular.dial,         m->cellular.accept,
                           m->cellular.disconnect,    m->cellular.name_lookup,  m->cellular.sms_send,
                           m->cellular.sms_reply,     m->cellular.send_message, m->cellular.radio_select,
                           m->cellular.alert_request, m->cellular.poll,         m->network.txn_start,
                           m->network.send_pdu,       m->network.recv_pdu,      m->network.bearer_state,
                           m->network.post_event,     m->network.data_arrived};
        uint32_t ram[] = {m->cellular.services, m->cellular.sr_task, m->network.wap_base,
                          m->network.driver_base};
        const unsigned sizes[] = {64, 4, 0x140, 20};
        if (!m->cellular.ready)
            valid = 0;
        for (i = 0; i < sizeof(code) / sizeof(code[0]); ++i) {
            if (code[i] && !rom_contains(m, code[i], 2))
                valid = 0;
        }
        for (i = 0; i < 4; ++i) {
            int found = 0;
            if (!ram[i])
                continue;
            for (j = 0; j < m->bus.count; ++j) {
                Region *r = &m->bus.regions[j];
                if (r->data && !r->readonly && !r->read && !r->write && ram[i] >= r->base &&
                    (uint64_t)ram[i] + sizes[i] <= (uint64_t)r->base + r->size)
                    found = 1;
            }
            if (!found)
                valid = 0;
        }
    }
    if (valid && m->java_buffer_get) {
        int table = 0;
        if (!rom_contains(m, m->java_buffer_get, 2) || !rom_contains(m, m->java_buffer_set, 2))
            valid = 0;
        for (unsigned i = 0; i < m->bus.count; ++i) {
            Region *r = &m->bus.regions[i];
            if (r->data && !r->readonly && !r->read && !r->write && m->java_buffer_table >= r->base &&
                (uint64_t)m->java_buffer_table + 3u * 0x98u <= (uint64_t)r->base + r->size)
                table = 1;
            if ((uint64_t)m->java_buffer_base < (uint64_t)r->base + r->size &&
                (uint64_t)r->base < (uint64_t)m->java_buffer_base + 3u * m->java_buffer_size)
                valid = 0;
        }
        if (!table)
            valid = 0;
    }
    if (valid && m->battery.enabled && !m->battery.detect && !m->battery.charger_port)
        valid = 0;
    if (valid && m->periodic_time && !m->clock.attached)
        valid = 0;
    if (valid && m->nvram_size) {
        unsigned i, j;
        for (i = 0; i < m->flash_count; ++i) {
            Flash *flash = &m->flashes[i];
            if (m->nvram_base < flash->base ||
                (uint64_t)m->nvram_base + m->nvram_size > (uint64_t)flash->base + flash->size)
                continue;
            for (j = 0; j < m->rom_count; ++j) {
                RomImage *image = &m->roms[j];
                if (image->base == flash->base &&
                    (uint64_t)m->nvram_base >= (uint64_t)image->base + image->size)
                    m->nvram_data = flash->data + m->nvram_base - flash->base;
            }
        }
        if (!m->nvram_data)
            valid = 0; /* Never overlay supplied firmware bytes. */
    }
    if (valid && m->nv_trigger) {
        if (!m->nvram_data || !rom_contains(m, m->nv_trigger, 4) || !rom_contains(m, m->nv_format, 4) ||
            !rom_contains(m, m->nv_marker, 4))
            valid = 0;
    }
    if (valid && m->irq_gate_mask) {
        unsigned i;
        for (i = 0; i < m->register_count; ++i) {
            Register32 *r = &m->registers[i];
            if (m->irq_gate_address >= r->address &&
                (uint64_t)m->irq_gate_address + 2 <= (uint64_t)r->address + 4) {
                m->irq_gate_register = r;
                break;
            }
        }
        if (!m->intc.attached || !m->irq_gate_register)
            valid = 0;
    }
    if (valid && c100_backlights)
        valid = cx805_attach_backlights(m);
    else if (valid && m->c100.enabled)
        valid = cx805_attach_board(m);
    if (m->c100.pmic_wake_xor && !m->c100.enabled)
        valid = 0;
    if (valid && m->c100.enabled) {
        valid = m->clock.attached && m->lcd.attached && m->pmic.attached;
        if (valid && m->clock.base == CX805_PLL_BASE) {
            Register32 *mode = &m->c100.asic_mode;
            *mode = (Register32){.bus = &m->bus, .address = CX805_ASIC_MODE, .value = 0x47, .reset_value = 0x47};
            valid = bus_device(&m->bus, mode->address, 2, register_read, register_write, mode);
        }
        m->c100.release_ns = m->c100.key_hold_ns;
        /* Compatibility profiles enter after board bring-up, with the PMU
           status used by the existing handoff. A wake supplies a new cause. */
        if (m->c100.wired_lcd)
            m->pmic.regs[PMIC_POWER_STATUS] = PMIC_POWER_KEY;
        m->c100.powered = 1;
    }
    if (valid && m->irom.boot.enabled)
        valid = cx805_attach_nor_boot(m);
    if (!valid || !boot_image || !reset_set || (m->clock.enable_base && !m->clock.attached) ||
        (rom_override && !m->rom_count) ||
        (second_override && !second_used)) {
        snprintf(m->error, sizeof(m->error), "Invalid model or firmware at %.160s:%u", path, line_no);
        bus_destroy(&m->bus);
        free(m->unplaced_rom);
        m->unplaced_rom = NULL;
        m->unplaced_size = 0;
        return 0;
    }
    clock_apply_enables(m);
    /* Stock profiles keep their established polling and Power-key timing. */
    if (m->c100.wired_lcd)
        keypad_connect_irq(&m->keypad, &m->intc);
    rtc_connect_irq(&m->rtc, &m->intc);
    m->pixels = calloc((size_t)m->width * m->height, sizeof(*m->pixels));
    if (!m->pixels) {
        strcpy(m->error, "Cannot allocate LCD framebuffer");
        bus_destroy(&m->bus);
        free(m->unplaced_rom);
        m->unplaced_rom = NULL;
        m->unplaced_size = 0;
        return 0;
    }
    if (m->lcd.attached) {
        if (m->height > 256 || m->width + m->lcd.origin_y > 256) {
            free(m->pixels);
            m->pixels = NULL;
            bus_destroy(&m->bus);
            free(m->unplaced_rom);
            m->unplaced_rom = NULL;
            strcpy(m->error, "LCD viewport exceeds controller RAM");
            return 0;
        }
        lcd_connect(&m->lcd, m->pixels, m->width, m->height);
    }
    arm7_reset(&m->cpu, &m->bus, m->reset);
    m->cpu.vector_undefined = vector_undefined;
    m->decode_cache = arm7_cache_create();
    m->cpu.cache = m->decode_cache; /* allocation failure uses uncached decoding */
    if (m->ma3.attached) {
        if (m->clock.attached)
            ma3_set_clock(&m->ma3, &m->clock.nanoseconds, 1000000000);
        else
            ma3_set_clock(&m->ma3, &m->cpu.cycles, MA3_DEFAULT_TICKS_HZ);
        if (m->phone_io.configured)
            ma3_set_mixer(&m->ma3, phone_io_mix, &m->phone_io);
    }
    m->loaded = 1;
    if (!*m->name)
        strcpy(m->name, "SGH model");
    return 1;
}

/* Only the recovered Java working-buffer helpers are extended. The native
   compiler, file format, registry, flash writer and VM still run unchanged.
   Disabled by default: these larger buffers are an emulator compatibility
   feature, not a claim about the C100's physical memory or package limits. */
static int java_buffer_step(Machine *m)
{
    Arm7 *c = &m->cpu;
    unsigned handle = c->r[0];
    uint32_t pc = c->r[15], lr = c->r[14];
    if (!m->java_extended || !m->java_buffer_get || handle >= 3 ||
        (pc != m->java_buffer_get && pc != m->java_buffer_set))
        return 0;
    if (!(c->cpsr & ARM_T)) {
        c->halted = 1;
        c->error = "Java buffer helper: expected Thumb";
        return -1;
    }
    if (!m->java_buffer_data) {
        uint8_t *data = calloc(3u, m->java_buffer_size);
        if (!data || !bus_map(&m->bus, m->java_buffer_base, 3u * m->java_buffer_size, data, 0, 1)) {
            free(data);
            c->halted = 1;
            c->error = "Cannot map extended Java working buffers";
            return -1;
        }
        m->java_buffer_data = data;
    }
    if (pc == m->java_buffer_set) {
        memset(m->java_buffer_data + (size_t)handle * m->java_buffer_size, 0, m->java_buffer_size);
        bus_write(&m->bus, m->java_buffer_table + handle * 0x98u + 0x94u, 4,
                  m->java_buffer_base + handle * m->java_buffer_size);
        if (m->bus.fault) {
            c->halted = 1;
            c->error = "Java working-buffer table is not writable";
            return -1;
        }
        c->r[0] = 1;
    } else
        c->r[0] = m->java_buffer_size;
    c->cpsr = (c->cpsr & ~ARM_T) | ((lr & 1) ? ARM_T : 0);
    c->r[15] = lr & ((lr & 1) ? ~1u : ~3u);
    ++c->instructions;
    ++c->cycles;
    return 1;
}

/* Bring-up can acknowledge an unsupported access and keep executing. */
static inline int bus_fault_allows_step(Bus *bus, int continuation)
{
    if (!bus->fault)
        return 1;
    if (!continuation)
        return 0;
    bus->fault = 0;
    return 1;
}

static inline int machine_finish_step(Machine *m, int continuation)
{
    if (!bus_fault_allows_step(&m->bus, continuation))
        return 0;
    if (m->c100.reset_pending)
        cx805_reset(m, 0);
    return 1;
}

static uint64_t earlier(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static uint64_t machine_time_deadline(const Machine *m)
{
    uint64_t due = UINT64_MAX;
    if (m->c100.enabled)
        due = earlier(due, m->c100.release_ns);
    if (m->debug_uart.nanoseconds)
        due = earlier(due, m->debug_uart.tx_due);
    if (m->sds_uart.nanoseconds)
        due = earlier(due, m->sds_uart.tx_due);
    for (unsigned i = 0; i < 2; ++i)
        if (m->timers[i].nanoseconds)
            due = earlier(due, m->timers[i].next_ns);
    if (m->lp_timer.attached)
        due = earlier(due, m->lp_timer.next_event);
    if (m->rtc.attached)
        due = earlier(due, m->rtc.alarm_due);
    if (m->c100.enabled && m->rtc.attached)
        due = earlier(due, m->rtc.alarm_end);
    if (m->battery.complete_port)
        due = earlier(due, m->battery.complete_due);
    if (m->phone_io.output && !m->phone_io.mixed)
        due = earlier(due, m->phone_io.last_ns + 1000000u);
    if (m->ma3.attached && m->clock.attached)
        due = earlier(due, m->ma3.next_event);
    if (m->periodic_time && m->intc.period && (m->intc.enabled & m->intc.periodic_bit))
        due = earlier(due, m->intc.next_tick);
    return due;
}

int machine_step(Machine *m, int continuation)
{
    Arm7 *cpu = &m->cpu;
    if (!m->loaded || machine_powered_off(m))
        return 0;

    const uint64_t now_ns = m->clock.nanoseconds;
    const uint64_t previous_clocks = cpu->clock_cycles;
    if (m->rtc.attached && (now_ns >= m->rtc.alarm_due || now_ns >= m->rtc.alarm_end))
        rtc_advance(&m->rtc);
    if (m->c100.enabled && !cx805_check_power(m))
        return 0;
    if (m->c100.reset_pending) {
        cx805_reset(m, 0);
        return 1;
    }
    if (cpu->halted)
        return 0;
    const int sleeping = machine_sleeping(m);
    if (m->irom.boot.pending) {
        int booted = irom_cx805_boot(&m->irom.boot, cpu);
        clock_advance(&m->clock, cpu->clock_cycles);
        if (booted < 0) {
            char reason[160];
            snprintf(reason, sizeof(reason), "IROM NOR boot: %s", cpu->error);
            strcpy(m->error, reason);
            cpu->error = m->error;
        }
        return booted > 0;
    }
    Cellular *cellular = &m->cellular;
    /* Only drain host queues at native scheduler/event-loop entry points. */
    if (!sleeping && (cellular->stage || cpu->r[15] == cellular->ready ||
                      (cellular->poll && cpu->r[15] == cellular->poll))) {
        if (!cellular->boot_ready && m->sim.card.commands) {
            /* This native startup query follows loading the handset UI records,
               also used by first-boot provisioning to recognize settled defaults. */
            for (unsigned i = 0; i < m->irom.count; ++i) {
                if (m->irom.services[i].kind == IROM_VERSION && m->irom.services[i].calls) {
                    cellular->boot_ready = 1;
                    break;
                }
            }
        }
        if (!cellular->stage) {
            phone_net_advance(&m->network, cellular);
            java_net_advance(&m->network.java, cellular, cpu, m->network.mode && cellular->enabled);
        }
        if (!cellular_advance(cellular, cpu, m->guest_busy || m->battery.stage, m->sim.present)) {
            cpu->error = cellular->error;
            cpu->halted = 1;
            return 0;
        }
    }

    /* Deliver events due at the current time before executing the next instruction. */
    if (m->ma3.attached && *m->ma3.clock >= m->ma3.next_event)
        ma3_advance(&m->ma3, *m->ma3.clock);
    if (m->phone_io.output && !m->phone_io.mixed && now_ns - m->phone_io.last_ns >= 1000000u)
        phone_io_advance(&m->phone_io, now_ns);
    if (!sleeping && m->phone_io.configured && (cpu->cpsr & ARM_T) &&
        (cpu->r[15] == m->phone_io.tone_entry || cpu->r[15] == m->phone_io.tone_stop ||
         cpu->r[15] == m->phone_io.gpio_entry)) {
        /* Finish the old tone through the time of the guest driver call. */
        if (m->ma3.output)
            ma3_advance(&m->ma3, *m->ma3.clock);
        phone_io_advance(&m->phone_io, now_ns);
        phone_io_observe(&m->phone_io, cpu);
    }

    sim_tick(&m->sim, cpu->cycles);
    for (unsigned i = 0; i < 2; ++i) {
        Gpt *timer = &m->timers[i];
        if (timer->nanoseconds && (now_ns >= timer->next_ns || cpu->clock_cycles >= timer->next_core_cycle))
            gpt_advance(timer);
    }
    if (m->debug_uart.nanoseconds && now_ns >= m->debug_uart.tx_due)
        sds_uart_advance(&m->debug_uart);
    if (m->sds_uart.nanoseconds && now_ns >= m->sds_uart.tx_due)
        sds_uart_advance(&m->sds_uart);
    if (m->sds_dma.attached && cpu->cycles >= m->sds_dma.next_event)
        sds_dma_advance(&m->sds_dma, cpu->cycles);
    if (*m->ma3.error) {
        cpu->halted = 1;
        cpu->error = m->ma3.error;
        return 0;
    }
    if (m->lp_timer.attached && (now_ns >= m->lp_timer.next_event ||
                                 (m->intc.enabled & m->lp_timer.cal_mask) != m->lp_timer.observed_enabled))
        lp_timer_advance(&m->lp_timer);
    if (m->battery.complete_port && now_ns >= m->battery.complete_due)
        battery_advance(&m->battery);

    intc_advance_time(&m->intc, cpu, m->periodic_time ? now_ns : cpu->cycles);
    /* Sample the hardware latch, not a synthetic guest bus access. Pending
       sources remain latched while firmware critical sections close the gate. */
    if (m->irq_gate_register) {
        unsigned shift = (m->irq_gate_address - m->irq_gate_register->address) * 8;
        uint32_t latch = m->irq_gate_register->value >> shift;
        if (!(latch & m->irq_gate_mask))
            cpu->irq = 0;
    }
    if (sleeping) {
        if (!bus_fault_allows_step(&m->bus, continuation))
            return 0;
        if (cpu->irq || cpu->fiq)
            m->clock.enables &= (uint16_t)~CLOCK_SLEEP_REQUEST;
        else {
            /* The ARM clock is frozen. Use the reference/low-power time
               domain, with a bounded interval for input and host pacing. */
            uint64_t due = machine_time_deadline(m);
            uint64_t idle_limit = now_ns + 1000000u;
            m->clock.nanoseconds = due > now_ns ? earlier(idle_limit, due) : idle_limit;
        }
        return 1; /* Re-sample pending interrupts and CPU masks on the next step. */
    }
    if (!bus_fault_allows_step(&m->bus, continuation))
        return 0;

    /* Multi-step host-injected native calls borrow a parked task's CPU state.
       Treat that transaction as atomic: an IRQ-driven task switch can otherwise
       reuse its stack before caller restoration. Device latches/timers continue
       advancing; pending IRQ/FIQ is delivered immediately after the return. */
    if (cellular->stage) {
        cpu->irq = 0;
        cpu->fiq = 0;
    }

    /* Enter an enabled IRQ/FIQ before intercepting a firmware service call. */
    int hle = 0;
    int fiq_pending = cpu->fiq && !(cpu->cpsr & ARM_F);
    int irq_pending = cpu->irq && !(cpu->cpsr & ARM_I);
    if (!fiq_pending && !irq_pending) {
        uint32_t pc = cpu->r[15];
        PhoneNetwork *network = &m->network;
        /* Keep packet/event stack frames off the interpreter's normal path. */
        if ((cpu->cpsr & ARM_T) &&
            ((network->txn_start &&
              (pc == network->txn_start || pc == network->send_pdu || pc == network->recv_pdu ||
               pc == network->bearer_state || pc == network->post_event)) ||
             java_net_may_call(&network->java, pc)))
            hle = phone_net_hle(network, cellular, cpu);
        if (!hle && (cpu->cpsr & ARM_T) && cellular->enabled && cellular->ready &&
            (pc == cellular->radio_select || pc == cellular->name_lookup || pc == cellular->dial ||
             pc == cellular->accept || pc == cellular->disconnect || pc == cellular->sms_send ||
             pc == cellular->sms_reply || pc == cellular->alert_request))
            hle = cellular_hle(cellular, cpu);
        if (!hle && m->java_extended && (pc == m->java_buffer_get || pc == m->java_buffer_set))
            hle = java_buffer_step(m);
        if (!hle && m->battery.enabled && pc == m->battery.gpio)
            hle = battery_gpio(&m->battery, cpu);
        if (!hle && irom_may_call(&m->irom, pc))
            hle = irom_step(&m->irom, cpu);
    }
    if (hle < 0 || (!hle && !arm7_step(cpu)))
        return 0;
    if (hle && cpu->clock_cycles == previous_clocks)
        ++cpu->clock_cycles; /* Atomic HLE timing remains provisional. */
    if (m->clock.attached)
        clock_advance(&m->clock, cpu->clock_cycles);
    if (m->audio.ops->advance)
        m->audio.ops->advance(&m->audio, cpu->cycles);
    return machine_finish_step(m, continuation);
}

enum { RUN_HOOK_BITS = 2048, RUN_HOOK_WORDS = RUN_HOOK_BITS / 32 };

static inline unsigned run_hook_bit(uint32_t pc)
{
    return (pc >> 1) & (RUN_HOOK_BITS - 1);
}

static void mark_run_hook(uint32_t stops[RUN_HOOK_WORDS], uint32_t pc)
{
    unsigned bit = run_hook_bit(pc);
    if (pc)
        stops[bit / 32] |= 1u << (bit % 32);
}

unsigned machine_run(Machine *m, unsigned count, int continuation)
{
    if (count < 2)
        return count && machine_step(m, continuation) ? 1u : 0u;
    uint32_t stops[RUN_HOOK_WORDS] = {0};
    unsigned done = 0, i;
    uint32_t hooks[] = {m->cellular.ready,         m->cellular.poll,        m->cellular.radio_select,
                        m->cellular.name_lookup,   m->cellular.dial,        m->cellular.accept,
                        m->cellular.disconnect,    m->cellular.sms_send,    m->cellular.sms_reply,
                        m->cellular.alert_request, m->network.txn_start,    m->network.send_pdu,
                        m->network.recv_pdu,       m->network.bearer_state, m->network.post_event,
                        m->phone_io.tone_entry,    m->phone_io.tone_stop,   m->phone_io.gpio_entry,
                        m->java_buffer_get,        m->java_buffer_set,      m->battery.gpio};
    /* Hash live hook addresses into a bitset. A collision can end a batch
       early, but every configured hook is still caught. */
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i)
        mark_run_hook(stops, hooks[i]);
    for (i = 0; i < m->irom.count; ++i)
        mark_run_hook(stops, m->irom.services[i].address);
    for (i = 0; i < JN_COUNT; ++i)
        mark_run_hook(stops, m->network.java.entry[i]);
    while (done < count) {
        uint32_t epoch = m->bus.device_epoch;
        int sleeping = machine_sleeping(m);
        if (!machine_step(m, continuation))
            return done;
        ++done;
        if (sleeping || m->irom.boot.pending || epoch != m->bus.device_epoch || m->cellular.stage ||
            m->battery.stage || m->guest_busy || (m->battery.enabled && !m->battery.adc) ||
            m->audio.ops->advance || m->bus.access_log)
            continue;
        /* Stop at the first deadline in any clock domain: emulated time,
           legacy scheduler ticks, or ARM core cycles. The next machine_step
           delivers any due events before executing another instruction. */
        uint64_t ns_due = machine_time_deadline(m);
        uint64_t cycle_due = UINT64_MAX;
        uint64_t core_due = UINT64_MAX;
        if (m->sim.attached)
            cycle_due = earlier(cycle_due, m->sim.next_event);
        if (m->sds_dma.attached)
            cycle_due = earlier(cycle_due, m->sds_dma.next_event);
        for (i = 0; i < 2; ++i) {
            const Gpt *timer = &m->timers[i];
            if (timer->nanoseconds)
                core_due = earlier(core_due, timer->next_core_cycle);
        }
        if (m->ma3.attached && !m->clock.attached)
            cycle_due = earlier(cycle_due, m->ma3.next_event);
        if (!m->periodic_time && m->intc.period && (m->intc.enabled & m->intc.periodic_bit))
            cycle_due = earlier(cycle_due, m->intc.next_tick);
        while (done < count && m->clock.nanoseconds < ns_due && m->cpu.cycles < cycle_due &&
               m->cpu.clock_cycles < core_due) {
            unsigned bit = run_hook_bit(m->cpu.r[15]);
            if (stops[bit / 32] & (1u << (bit % 32)))
                break;
            /* The reference controller slides a disabled periodic deadline
               every step. Preserve that state too, not just delivered IRQs. */
            if (m->intc.attached && m->intc.period && !(m->intc.enabled & m->intc.periodic_bit))
                m->intc.next_tick =
                    (m->periodic_time ? m->clock.nanoseconds : m->cpu.cycles) + m->intc.period;
            if (!arm7_step(&m->cpu))
                return done;
            if (m->clock.attached)
                clock_advance(&m->clock, m->cpu.clock_cycles);
            if (!machine_finish_step(m, continuation))
                return done;
            ++done;
            if (epoch != m->bus.device_epoch)
                break;
        }
    }
    return done;
}

void machine_key(Machine *m, unsigned key, int down)
{
    if (key >= SGH_MAX_KEYS)
        return;
    unsigned mask = 1u << key;
    int was_held = m->keys & mask;
    if (down)
        m->keys |= mask;
    else
        m->keys &= ~mask;
    if (machine_powered_off(m)) {
        if (key == SGH_KEY_POWER && down && !was_held)
            cx805_power_on(m, C100_WAKE_KEY);
        return;
    }
    keypad_key(&m->keypad, key, down);
    if (key == SGH_KEY_POWER && !down && m->pmic.key_supply) {
        m->pmic.key_supply = 0;
        rtc_advance(&m->rtc);
        cx805_check_power(m);
    }
}

void machine_pattern(Machine *m, int enabled)
{
    unsigned x, y;
    if (!m->pixels)
        return;
    for (y = 0; y < m->height; ++y)
        for (x = 0; x < m->width; ++x) {
            static const uint32_t colors[] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                              0xff00ff, 0xff0000, 0x0000ff, 0};
            m->pixels[(size_t)y * m->width + x] = enabled ? colors[(x * 8 / m->width) & 7] : 0;
        }
}
