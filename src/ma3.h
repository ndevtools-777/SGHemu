#ifndef SGH_MA3_H
#define SGH_MA3_H

#include "intc.h"
#include "audio_output.h"
#include "ma3_synth.h"
#include "ma_detune.h"
#include "ma_water.h"

enum { MA3_DEFAULT_TICKS_HZ = 2166667, MA3_FIFO_BYTES = 512 };

typedef struct {
    unsigned phase, parts, full_ram;
    uint32_t delay, address, length;
} Ma3Packet;

typedef struct { uint64_t due, period; } Ma3Timer;

typedef struct {
    uint64_t frames, nonzero, underruns;
    unsigned peak;
    int failed;
} Ma3Stats;

typedef void (*Ma3Mixer)(void *context, int16_t *left, int16_t *right, unsigned frames);

typedef struct {
    Bus *bus;
    Intc *intc;
    uint32_t base, irq_mask;
    const uint64_t *clock;
    uint32_t ticks_hz, sequence_quantum_ns;
    uint64_t now, next_event; /* now is in ns; next_event is in the bound clock's units. */
    uint64_t rendered_frames; /* Chip frames since the clock's origin, before output effects. */
    int attached;

    uint8_t parameter_ram[0x400], registers[4][64];
    unsigned selector, control, bank, irq_flags;
    uint8_t responses[3];
    unsigned response_ready;
    Ma3Packet immediate, fifo_input, delayed;
    uint8_t fifo[MA3_FIFO_BYTES];
    unsigned fifo_head, fifo_count;
    uint64_t fifo_due, sequence_due, sequence_wait, sequence_period;
    Ma3Timer timers[2];

    Ma3Synth synth;
    AudioOutput *output;
    Ma3Mixer mixer;
    void *mixer_context;
    Ma3Stats stats;
    MaDetune detune;
    MaWater water_pcm;
    int water;
    uint32_t water_rng, led_rng;
    unsigned water_sample_burst, water_scale;
    uint64_t water_next, water_faults;
    unsigned led_on, led_color;
    uint64_t led_tick, led_events;

    uint64_t reads, writes, fifo_bytes, irq_events, ram_reads, commands, unsupported;
    uint64_t fifo_clears, partial_drops;
    FILE *trace;
    char error[128], output_error[128];
} Ma3;

int ma3_attach(Ma3 *m, Bus *bus, Intc *intc, uint32_t base, uint32_t irq_mask, uint32_t sequence_quantum_ns);
/* Bind before execution. A PLL clock uses nanoseconds (1000000000 Hz). */
void ma3_set_clock(Ma3 *m, const uint64_t *ticks, uint32_t ticks_hz);
void ma3_advance(Ma3 *m, uint64_t ticks);
int ma3_open_audio(Ma3 *m, int speakers, const char *wav);
int ma3_close_audio(Ma3 *m);
void ma3_set_mixer(Ma3 *m, Ma3Mixer mixer, void *context);
void ma3_set_detune(Ma3 *m, int enabled);
void ma3_set_water(Ma3 *m, int enabled);
Ma3Stats ma3_stats(const Ma3 *m);
void ma3_destroy(Ma3 *m);

#endif
