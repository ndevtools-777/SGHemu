#include "gpt.h"

#define TIMER_MASK 0x03ffffffu
#define NS_PER_SECOND UINT64_C(1000000000)

enum { MODE_MASK = 3, PULSE = 1, SELECT_CLOCK = 1 << 2, STOP = 1 << 3, IRQ_ENABLE = 1 << 4, LOAD_HALF = 1 << 5 };

static int uses_arm_clock(const Gpt *timer)
{
    return timer->irq_mask == 2 && (timer->control & SELECT_CLOCK);
}

static int counting(const Gpt *timer)
{
    /* Event counting and pulse measurement need external inputs. */
    return timer->clock_enabled && !(timer->control & STOP) && (timer->control & MODE_MASK) < 2 &&
           (!(timer->control & SELECT_CLOCK) || uses_arm_clock(timer) || timer->etu_divider);
}

static uint32_t clock_divider(const Gpt *timer)
{
    return (timer->control & SELECT_CLOCK) && timer->etu_divider ? timer->etu_divider : 1;
}

static uint64_t current_tick(const Gpt *timer)
{
    uint64_t ns = *timer->nanoseconds;
    if (uses_arm_clock(timer))
        return *timer->arm_cycles;
    uint64_t ticks = ns / NS_PER_SECOND * timer->reference_hz +
                     ns % NS_PER_SECOND * timer->reference_hz / NS_PER_SECOND;
    return ticks / clock_divider(timer);
}

static uint32_t ticks_to_expiry(const Gpt *timer)
{
    return timer->counter.value ? timer->counter.value : 1;
}

uint32_t gpt_pulse_period(const Gpt *timer)
{
    if ((timer->control & MODE_MASK) != PULSE || !counting(timer))
        return 0;
    return timer->period.value ? timer->period.value : 1;
}

int gpt_pulse_output(const Gpt *timer)
{
    uint32_t period = gpt_pulse_period(timer);
    if (!period)
        return 0;
    uint64_t now = current_tick(timer), first = timer->last_tick + ticks_to_expiry(timer);
    /* Model a high pulse for one input tick. Sampling the phase analytically
       gives the same result even when one ARM instruction crosses many pulses. */
    return now == timer->last_expiry || (now >= first && !((now - first) % period));
}

static void schedule(Gpt *timer)
{
    uint64_t next_tick;
    timer->next_ns = timer->next_core_cycle = UINT64_MAX;
    if (!counting(timer) || !(timer->control & IRQ_ENABLE))
        return;
    next_tick = timer->last_tick + ticks_to_expiry(timer);
    if (uses_arm_clock(timer))
        timer->next_core_cycle = next_tick;
    else {
        uint32_t hz = timer->reference_hz;
        next_tick *= clock_divider(timer);
        timer->next_ns = next_tick / hz * NS_PER_SECOND + (next_tick % hz * NS_PER_SECOND + hz - 1) / hz;
    }
}

void gpt_advance(Gpt *timer)
{
    uint64_t now = current_tick(timer);
    uint64_t elapsed = now - timer->last_tick;
    uint32_t remaining = ticks_to_expiry(timer);
    timer->last_tick = now;
    if (elapsed && counting(timer)) {
        if (elapsed < remaining)
            timer->counter.value -= (uint32_t)elapsed;
        else {
            uint32_t period = timer->period.value;
            uint32_t interval = period ? period : 1;
            elapsed -= remaining;
            timer->last_expiry = now - elapsed % interval;
            if (timer->control & IRQ_ENABLE) {
                timer->intc->pending |= timer->irq_mask;
                timer->expirations += 1 + elapsed / interval;
            }
            timer->counter.value = period ? period - (uint32_t)(elapsed % period) : 0;
        }
    }
    schedule(timer);
}

void gpt_set_clock(Gpt *timer, int enabled)
{
    if (!timer->nanoseconds || timer->clock_enabled == enabled)
        return;
    gpt_advance(timer);
    timer->clock_enabled = enabled;
    timer->last_expiry = UINT64_MAX;
    schedule(timer);
    if (timer->output_changed)
        timer->output_changed(timer->output_context);
}

static uint32_t register_pair(const Gpt *timer, unsigned offset)
{
    if (offset < 4)
        return timer->period.value;
    if (offset < 8)
        return timer->period.holding;
    if (offset < 12)
        return timer->counter.value;
    return timer->control;
}

static uint32_t read_timer(void *context, uint32_t offset, unsigned width)
{
    Gpt *timer = context;
    uint32_t value = 0;
    unsigned i;
    if (offset & (width - 1)) {
        bus_fault_access(timer->bus, 0, timer->base + offset, width, 0, "unaligned timer read");
        return 0;
    }
    gpt_advance(timer);
    for (i = 0; i < width; ++i) {
        unsigned address = offset + i;
        uint32_t pair = register_pair(timer, address);
        value |= ((pair >> (8 * (address & 3))) & 0xffu) << (8 * i);
    }
    return value;
}

static uint32_t write_part(uint32_t old, unsigned offset, unsigned width, uint32_t value)
{
    unsigned shift = 8 * offset;
    uint32_t mask = width == 4 ? UINT32_MAX : ((1u << (8 * width)) - 1) << shift;
    return ((old & ~mask) | ((value << shift) & mask)) & TIMER_MASK;
}

static void write_timer_value(Gpt *timer, TimerValue *target, unsigned offset, unsigned width, uint32_t value)
{
    unsigned halves = width == 4 ? 3u : 1u << (offset / 2);
    /* TLS=0 waits for both halves. TLS=1 can change one half of a live count. */
    if (!target->halves_written)
        target->holding = target->value;
    target->holding = write_part(target->holding, offset, width, value);
    target->halves_written |= halves;
    if ((timer->control & LOAD_HALF) || target->halves_written == 3) {
        target->value = target->holding;
        target->halves_written = 0;
    }
}

static void write_timer(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    Gpt *timer = context;
    unsigned byte = offset & 3;
    if (offset & (width - 1)) {
        bus_fault_access(timer->bus, 1, timer->base + offset, width, value, "unaligned timer write");
        return;
    }
    gpt_advance(timer);
    if (offset < 4)
        write_timer_value(timer, &timer->period, byte, width, value);
    else if (offset < 8)
        timer->period.holding = write_part(timer->period.holding, byte, width, value);
    else if (offset < 12)
        write_timer_value(timer, &timer->counter, byte, width, value);
    else {
        /* The implemented control bits all live in the low byte. */
        if (!byte) {
            if ((timer->control ^ value) & (MODE_MASK | SELECT_CLOCK | STOP))
                timer->last_expiry = UINT64_MAX;
            timer->control = (uint16_t)(value & 0x7f);
        }
        timer->last_tick = current_tick(timer);
    }
    schedule(timer);
    if (timer->output_changed)
        timer->output_changed(timer->output_context);
}

void gpt_reset(Gpt *timer)
{
    if (!timer->nanoseconds)
        return;
    timer->period = (TimerValue){0};
    timer->counter = (TimerValue){.value = TIMER_MASK};
    timer->control = STOP;
    timer->clock_enabled = timer->irq_mask == 1;
    timer->last_tick = current_tick(timer);
    timer->last_expiry = UINT64_MAX;
    schedule(timer);
}

int gpt_attach(Gpt *timer, Bus *bus, Intc *intc, uint32_t base, unsigned number, const uint64_t *nanoseconds,
               const uint64_t *arm_cycles, uint32_t reference_hz, uint32_t etu_divider)
{
    if (timer->nanoseconds || !intc->attached || (base & 3) || number < 1 || number > 2 || !nanoseconds ||
        !arm_cycles || !reference_hz || (number != 1 && etu_divider))
        return 0;
    if (!bus_device(bus, base, 0x0e, read_timer, write_timer, timer))
        return 0;
    memset(timer, 0, sizeof(*timer));
    timer->bus = bus;
    timer->intc = intc;
    timer->base = base;
    timer->nanoseconds = nanoseconds;
    timer->arm_cycles = arm_cycles;
    timer->reference_hz = reference_hz;
    timer->etu_divider = etu_divider;
    timer->irq_mask = 1u << (number - 1);
    gpt_reset(timer);
    return 1;
}
