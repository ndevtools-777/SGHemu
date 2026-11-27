#ifndef SGH_GPT_H
#define SGH_GPT_H
#include "intc.h"

typedef struct {
    uint32_t value, holding;
    unsigned halves_written;
} TimerValue;

typedef struct {
    Bus *bus;
    Intc *intc;
    const uint64_t *nanoseconds, *arm_cycles;
    uint32_t base, reference_hz, irq_mask, etu_divider;
    TimerValue period, counter;
    uint16_t control;
    uint64_t last_tick, last_expiry, next_ns, next_core_cycle, expirations;
    int clock_enabled;
    void (*output_changed)(void *context);
    void *output_context;
} Gpt;

int gpt_attach(Gpt *timer, Bus *bus, Intc *intc, uint32_t base, unsigned number, const uint64_t *nanoseconds,
               const uint64_t *arm_cycles, uint32_t reference_hz, uint32_t etu_divider);
void gpt_set_clock(Gpt *timer, int enabled);
void gpt_reset(Gpt *timer);
void gpt_advance(Gpt *timer);
int gpt_pulse_output(const Gpt *timer);
/* Number of input ticks per pulse once running, or zero if the output is off. */
uint32_t gpt_pulse_period(const Gpt *timer);
#endif
