#ifndef SGH_PWM_H
#define SGH_PWM_H
#include "bus.h"

enum { PWM_FULL_SCALE = 256 };

typedef struct {
    uint64_t pattern_start;
    uint32_t pattern;
    uint16_t control, duty, divider, count;
} PwmGenerator;

typedef struct {
    Bus *bus;
    const uint64_t *nanoseconds;
    uint32_t base;
    PwmGenerator generator[2];
    uint64_t clock_start, clock_elapsed;
    int clock_enabled;
    void (*output_changed)(void *context);
    void *output_context;
} Pwm;

int pwm_attach(Pwm *p, Bus *bus, uint32_t base, const uint64_t *nanoseconds);
void pwm_reset(Pwm *p);
void pwm_set_clock(Pwm *p, int enabled);
/* Average high time of the multiplexed annunciator pin, scaled to PWM_FULL_SCALE. */
unsigned pwm_output(const Pwm *p);
#endif
