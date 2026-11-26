#ifndef SGH_MA3_SYNTH_H
#define SGH_MA3_SYNTH_H

#include <stdint.h>

enum { MA3_VOICE_END = 0x140, MA3_RAM_BASE = 0x4000, MA3_RAM_END = 0x6000 };

typedef struct {
    float level, attack, decay, sustain, sustain_decay, release;
    unsigned stage;
} Ma3Envelope;

typedef struct {
    float gain;
    unsigned pitch_high, pitch_low, channel, gate;
} Ma3Voice;

typedef struct {
    Ma3Envelope envelope;
    uint32_t phase, step;
    float ratio, gain, previous;
    unsigned wave, feedback;
} Ma3Operator;

typedef struct {
    Ma3Voice voice;
    Ma3Operator operators[4];
    unsigned algorithm;
} Ma3FmVoice;

typedef struct {
    Ma3Voice voice;
    Ma3Envelope envelope;
    uint32_t phase, step;
    unsigned start, loop, end, format, preset;
    unsigned decoded, adpcm_step, loop_step;
    int32_t predictor, loop_predictor;
    int streaming;
    float gain;
} Ma3PcmVoice;

typedef struct {
    uint8_t ram[MA3_RAM_END - MA3_RAM_BASE];
    Ma3FmVoice fm[32];
    Ma3PcmVoice pcm[8];
    uint32_t active_fm, active_pcm;
    float sine[1024], level[64], gain[32], attack[16], decay[16], sustain[16];
    float channel_gain[17], pan[17];
    unsigned volume;
} Ma3Synth;

void ma3_synth_init(Ma3Synth *s);
void ma3_synth_reset(Ma3Synth *s);
void ma3_synth_write(Ma3Synth *s, const uint8_t *parameters, unsigned address, unsigned value);
void ma3_synth_ram(Ma3Synth *s, unsigned address, uint8_t value);
unsigned ma3_synth_cursor(const Ma3Synth *s, unsigned voice);
void ma3_synth_render(Ma3Synth *s, int16_t *left, int16_t *right, unsigned frames);

#endif
