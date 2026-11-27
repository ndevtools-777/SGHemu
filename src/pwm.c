#include "pwm.h"

#define NS_PER_SECOND UINT64_C(1000000000)

enum { PWM_ENABLE = 1, PWM_READ_SHIFT = 2, PATTERN_CLOCK_HZ = 1950000, CHANNEL_SIZE = 12 };

static uint64_t reference_tick(const Pwm *p)
{
    uint64_t ns = *p->nanoseconds;
    return ns / NS_PER_SECOND * PATTERN_CLOCK_HZ + ns % NS_PER_SECOND * PATTERN_CLOCK_HZ / NS_PER_SECOND;
}

static uint64_t pwm_tick(const Pwm *p)
{
    return p->clock_elapsed + (p->clock_enabled ? reference_tick(p) - p->clock_start : 0);
}

static uint32_t shift_pattern(const Pwm *p, const PwmGenerator *g)
{
    if (!(g->control & PWM_ENABLE) || !g->divider)
        return g->pattern;
    unsigned shift = (unsigned)((pwm_tick(p) - g->pattern_start) / g->divider) & 31u;
    return shift ? (g->pattern >> shift) | (g->pattern << (32 - shift)) : g->pattern;
}

static int valid(Pwm *p, uint32_t offset, unsigned width, int write, uint32_t value)
{
    if ((offset & 15) + width <= CHANNEL_SIZE && !(offset & (width - 1)))
        return 1;
    bus_fault_access(p->bus, write, p->base + offset, width, value, "invalid PWM register access");
    return 0;
}

static uint32_t read_pwm(void *context, uint32_t offset, unsigned width)
{
    Pwm *p = context;
    const PwmGenerator *g = &p->generator[offset / 16];
    uint32_t value = 0;
    unsigned start = offset & 15;
    if (!valid(p, offset, width, 0, 0))
        return 0;
    const uint16_t registers[] = {g->control, g->duty, g->divider, g->count};
    if (start >= 8) {
        uint32_t pattern = g->control & PWM_READ_SHIFT ? shift_pattern(p, g) : g->pattern;
        return (pattern >> ((start - 8) * 8)) & (width == 4 ? UINT32_MAX : (1u << (width * 8)) - 1);
    }
    for (unsigned i = 0; i < width; ++i) {
        unsigned byte = start + i;
        value |= (uint32_t)((registers[byte / 2] >> ((byte & 1) * 8)) & 255u) << (i * 8);
    }
    return value;
}

static void write_pwm(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    Pwm *p = context;
    PwmGenerator *g = &p->generator[offset / 16];
    unsigned start = offset & 15;
    if (!valid(p, offset, width, 1, value))
        return;
    if (start >= 8) {
        unsigned shift = (start - 8) * 8;
        uint32_t mask = width == 4 ? UINT32_MAX : ((1u << (width * 8)) - 1) << shift;
        g->pattern = (g->pattern & ~mask) | ((value << shift) & mask);
        g->pattern_start = pwm_tick(p);
    } else {
        static const uint16_t masks[] = {3, 255, 1023, 255};
        uint16_t *registers[] = {&g->control, &g->duty, &g->divider, &g->count};
        unsigned old_control = g->control, old_divider = g->divider;
        for (unsigned i = 0; i < width; ++i) {
            unsigned byte = start + i, index = byte / 2, shift = (byte & 1) * 8;
            unsigned data = (value >> (i * 8)) & 255u;
            *registers[index] = (uint16_t)(((*registers[index] & ~(255u << shift)) | (data << shift)) & masks[index]);
        }
        if (((g->control ^ old_control) & PWM_ENABLE) || g->divider != old_divider)
            g->pattern_start = pwm_tick(p);
    }
    if (p->output_changed)
        p->output_changed(p->output_context);
}

int pwm_attach(Pwm *p, Bus *bus, uint32_t base, const uint64_t *nanoseconds)
{
    if (p->nanoseconds || !nanoseconds || (base & 3) ||
        !bus_device(bus, base, 16 + CHANNEL_SIZE, read_pwm, write_pwm, p))
        return 0;
    p->bus = bus;
    p->base = base;
    p->nanoseconds = nanoseconds;
    pwm_reset(p);
    return 1;
}

void pwm_reset(Pwm *p)
{
    if (!p->nanoseconds)
        return;
    memset(p->generator, 0, sizeof(p->generator));
    p->clock_start = reference_tick(p);
    p->clock_elapsed = 0;
    p->clock_enabled = 1;
}

void pwm_set_clock(Pwm *p, int enabled)
{
    if (!p->nanoseconds || p->clock_enabled == enabled)
        return;
    p->clock_elapsed = pwm_tick(p);
    p->clock_start = reference_tick(p);
    p->clock_enabled = enabled;
    if (p->output_changed)
        p->output_changed(p->output_context);
}

unsigned pwm_output(const Pwm *p)
{
    unsigned total = 0;
    if (!p->clock_enabled)
        return 0;
    for (unsigned i = 0; i < 2; ++i) {
        const PwmGenerator *g = &p->generator[i];
        if (!(g->control & PWM_ENABLE))
            continue;
        unsigned count = g->count ? g->count : 100;
        unsigned high_ticks = g->duty < count ? g->duty + 1u : count;
        unsigned pattern_high = 0;
        if (g->divider) {
            for (uint32_t pattern = g->pattern; pattern; pattern &= pattern - 1)
                ++pattern_high;
        } else
            pattern_high = g->pattern & 1u ? 32u : 0u;
        total += PWM_FULL_SCALE * high_ticks * pattern_high / (64u * count);
    }
    return total;
}
