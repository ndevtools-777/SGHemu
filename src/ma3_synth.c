#include "ma3_synth.h"
#include "adpcm.h"
#include "audio_output.h"
#include <math.h>
#include <string.h>

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum { ADPCM4, PCM16, PCM8 };

void ma3_synth_reset(Ma3Synth *s)
{
    unsigned i;
    memset(s->ram, 0, sizeof(s->ram));
    memset(s->fm, 0, sizeof(s->fm));
    memset(s->pcm, 0, sizeof(s->pcm));
    s->active_fm = s->active_pcm = s->volume = 0;
    for (i = 0; i < 17; ++i) {
        s->channel_gain[i] = i < 16 ? s->gain[24] : 1.0f;
        s->pan[i] = 0.0f;
    }
}

void ma3_synth_init(Ma3Synth *s)
{
    unsigned i;
    for (i = 0; i < 1024; ++i) s->sine[i] = (float)sin(i * (6.283185307179586 / 1024));
    for (i = 0; i < 64; ++i) s->level[i] = (float)pow(2.0, -(double)i / 8.0);
    for (i = 1; i < 32; ++i) s->gain[i] = s->level[31 - i];
    /* Approximate amplitude changes per sample at 48 kHz. Measured release
       roughly doubles in speed per step and levels off at the top. */
    for (i = 1; i < 16; ++i) {
        s->attack[i] = (float)(pow(2.0, -(15 - (double)i) * 0.8) / 48.0);
        s->decay[i] = 0.000008f * (float)(1u << (i - 1));
    }
    s->decay[15] = 0.08f;
    for (i = 0; i < 16; ++i) s->sustain[i] = s->level[i * 4];
    ma3_synth_reset(s);
}

static void envelope_load(const Ma3Synth *s, Ma3Envelope *env, const uint8_t *data)
{
    env->decay = s->decay[data[0] >> 4] * (data[0] & 1u ? 4.0f : 1.0f);
    env->release = s->decay[data[1] >> 4];
    env->sustain = s->sustain[data[1] & 15u];
    env->attack = s->attack[data[2] >> 4];
    env->sustain_decay = s->decay[data[2] & 15u];
}

static void envelope_key(Ma3Envelope *env, unsigned gate, unsigned damp)
{
    if (gate) {
        env->level = 0;
        env->stage = ENV_ATTACK;
    } else if (damp) {
        env->level = 0;
        env->stage = ENV_OFF;
    } else if (env->stage != ENV_OFF) {
        /* Instrument flags don't suppress key-off. Release rate zero holds. */
        env->stage = ENV_RELEASE;
    }
}

static float envelope_step(Ma3Envelope *env)
{
    switch (env->stage) {
    case ENV_ATTACK:
        env->level += env->attack;
        if (env->level >= 1.0f) {
            env->level = 1.0f;
            env->stage = ENV_DECAY;
        }
        break;
    case ENV_DECAY:
        env->level *= 1.0f - env->decay;
        if (env->level <= env->sustain) env->stage = ENV_SUSTAIN;
        break;
    case ENV_SUSTAIN:
        env->level *= 1.0f - env->sustain_decay;
        break;
    case ENV_RELEASE:
        env->level *= 1.0f - env->release;
        break;
    }
    if (env->stage != ENV_ATTACK && env->level < 0.00025f) {
        env->stage = ENV_OFF;
        env->level = 0;
    }
    return env->level;
}

static unsigned fnum(const Ma3Voice *voice)
{
    return ((voice->pitch_high & 0x38u) << 4) | voice->pitch_low;
}

static void fm_pitch(Ma3FmVoice *fm)
{
    uint32_t step = fnum(&fm->voice) << ((fm->voice.pitch_high & 7u) + 12u);
    unsigned i;
    for (i = 0; i < 4; ++i) {
        Ma3Operator *op = &fm->operators[i];
        op->step = (uint32_t)(uint64_t)(step * (double)op->ratio);
    }
}

/* Small, independently voiced FM families for addresses in instrument ROM.
   These are original patches in the upload format, not a Yamaha ROM dump. */
static const uint8_t fm_presets[][16] = {
    {0x78, 0, 0x80, 0x86, 0xf6, 0x38, 0, 0x20, 2, 0x50, 0x66, 0xf4, 0, 0, 0x10, 0}, /* piano */
    {0x78, 0, 0xc0, 0xc8, 0xf9, 0x30, 0, 0x70, 0, 0x90, 0xb8, 0xf7, 0, 0, 0x10, 0}, /* mallet */
    {0x78, 1, 0x00, 0x90, 0xe0, 0x28, 0, 0x20, 0, 0x00, 0x90, 0xe0, 0, 0, 0x10, 0}, /* organ */
    {0x78, 0, 0x40, 0x82, 0x90, 0x30, 0, 0x10, 1, 0x60, 0x80, 0xa0, 0, 0, 0x10, 0}, /* reed */
    {0x78, 1, 0x10, 0x90, 0x90, 0x22, 0, 0x11, 1, 0x10, 0x80, 0xa0, 0, 0, 0x12, 0}, /* strings */
    {0x78, 0, 0xa0, 0x89, 0xf4, 0x2c, 0, 0x10, 1, 0x40, 0x68, 0xf3, 0, 0, 0x10, 0}, /* bass */
    {0x78, 0, 0xb0, 0xc8, 0xf8, 0x0c, 0, 0xb2, 5, 0xc0, 0xc8, 0xf7, 0x08, 0, 0x10, 0}, /* percussion */
};

/* Original ROM-voice approximations based on the level, harmonics and decay
   of individual reference notes. Each operator has its own waveform,
   envelope and frequency ratio. The same loader handles guest patches. */
#define ROM_OP_SHAPED(attack, decay, release, level, multiple, wave, feedback) \
    (decay) << 4, ((release) << 4) | 15, (attack) << 4, (level) << 2, \
    0, (multiple) << 4, ((wave) << 3) | (feedback)
#define ROM_OP(attack, decay, release, level, multiple) \
    ROM_OP_SHAPED(attack, decay, release, level, multiple, 0, 0)
#define ROM_SILENT 0, 0, 0, 0, 0, 0, 0

static const struct {
    unsigned address;
    uint8_t patch[30];
} fm_rom_voices[] = {
    /* A half-wave modulator with feedback gives the opening mallet its buzz. */
    {0x800, {0x78, 3, ROM_OP(15, 5, 8, 18, 3), ROM_OP(15, 4, 8, 21, 2),
                      ROM_OP_SHAPED(15, 4, 8, 4, 1, 1, 1), ROM_OP(15, 3, 8, 0, 1)}},
    /* Metallic overtones outlast the low part of the note. */
    {0x820, {0x78, 5, ROM_OP(15, 1, 7, 0, 1), ROM_OP(15, 2, 7, 10, 5),
                      ROM_SILENT, ROM_OP(15, 5, 7, 8, 1)}},
    /* Four audible harmonics, with a strong octave and quick release. */
    {0x840, {0x78, 2, ROM_OP(15, 0, 8, 5, 1), ROM_OP(15, 2, 8, 1, 2),
                      ROM_OP(15, 3, 8, 10, 3), ROM_OP(15, 3, 8, 21, 4)}},
    /* Long lead and its brighter companion, both with a 7:1 modulator. */
    {0x8a0, {0x78, 0, ROM_OP(15, 3, 5, 34, 7), ROM_OP(15, 3, 4, 1, 1)}},
    {0x8b0, {0x78, 0, ROM_OP(15, 4, 5, 14, 7), ROM_OP(15, 3, 6, 0, 1)}},
    /* A bright bell strike, with less ring from the lower carrier on release. */
    {0x8e0, {0x78, 5, ROM_OP(15, 2, 4, 19, 7), ROM_OP(15, 2, 4, 4, 6),
                      ROM_OP(15, 1, 4, 0, 7), ROM_OP(15, 4, 3, 6, 8)}},
    /* The closing bass keeps its fundamental after the overtones fade. */
    {0xae0, {0x78, 5, ROM_OP(15, 3, 4, 20, 2), ROM_OP(15, 2, 3, 2, 1),
                      ROM_SILENT, ROM_OP(15, 2, 3, 0, 1)}},
    /* Supporting parts swell into the note instead of striking immediately. */
    {0xb20, {0x78, 5, ROM_OP(8, 0, 5, 10, 3), ROM_OP(8, 0, 4, 9, 2),
                      ROM_SILENT, ROM_OP(8, 1, 4, 12, 3)}},
    {0xb60, {0x78, 5, ROM_OP(10, 2, 5, 1, 1), ROM_OP(8, 3, 5, 0, 4),
                      ROM_SILENT, ROM_OP(8, 3, 5, 0, 3)}},
    {0xd90, {0x78, 2, ROM_OP(9, 1, 5, 8, 1), ROM_OP(6, 1, 5, 8, 1),
                      ROM_OP(6, 1, 5, 4, 2), ROM_OP(6, 1, 5, 10, 3)}},
};

#undef ROM_SILENT
#undef ROM_OP
#undef ROM_OP_SHAPED

static const uint8_t *fm_instrument(const Ma3Synth *s, unsigned address)
{
    static const uint8_t families[] = {0, 1, 2, 5, 4, 4, 3, 3, 3, 2, 4, 2, 1, 1, 6, 6};
    const uint8_t *patch;
    unsigned bytes;
    if (address < MA3_RAM_BASE) {
        unsigned group = address >= 0x800 ? (address - 0x800) / 0x80 : 0;
        unsigned i;
        for (i = 0; i < sizeof(fm_rom_voices) / sizeof(*fm_rom_voices); ++i) {
            if (fm_rom_voices[i].address == address) return fm_rom_voices[i].patch;
        }
        return fm_presets[families[group < sizeof(families) ? group : sizeof(families) - 1]];
    }
    if (address > MA3_RAM_END - 2) return NULL;
    patch = s->ram + address - MA3_RAM_BASE;
    bytes = (patch[1] & 7u) < 2 ? 16u : 30u;
    return address <= MA3_RAM_END - bytes ? patch : NULL;
}

static void fm_load(const Ma3Synth *s, Ma3FmVoice *fm, unsigned address)
{
    static const float multiples[] = {0.5f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15};
    const uint8_t *patch = fm_instrument(s, address);
    unsigned i, count;
    if (!patch) return;
    fm->algorithm = patch[1] & 7u;
    count = fm->algorithm < 2 ? 2u : 4u;
    for (i = 0; i < count; ++i) {
        const uint8_t *data = patch + 2 + i * 7;
        Ma3Operator *op = &fm->operators[i];
        int detune = (int)(data[5] & 3u) * ((data[5] & 4u) ? -1 : 1);
        envelope_load(s, &op->envelope, data);
        op->gain = s->level[data[3] >> 2];
        /* Small per-operator offset, fitted near middle C. */
        op->ratio = multiples[data[5] >> 4] * (1.0f + 0.00021f * (float)detune);
        op->wave = data[6] >> 3;
        op->feedback = (i & 1u) ? 0u : data[6] & 7u;
    }
    fm_pitch(fm);
}

static void fm_key(Ma3Synth *s, unsigned index, unsigned value)
{
    Ma3FmVoice *fm = &s->fm[index];
    unsigned gate = value & 0x40u;
    unsigned i;
    if (gate != fm->voice.gate) {
        for (i = 0; i < 4; ++i) {
            Ma3Operator *op = &fm->operators[i];
            if (gate) {
                op->phase = 0;
                op->previous = 0.0f;
            }
            envelope_key(&op->envelope, gate, value & 0x10u);
        }
        if (gate) s->active_fm |= UINT32_C(1) << index;
        fm->voice.gate = gate;
    }
}

static unsigned big_endian16(const uint8_t *bytes)
{
    return ((unsigned)bytes[0] << 8) | bytes[1];
}

static unsigned pcm_bytes(unsigned format, unsigned samples)
{
    if (format == ADPCM4) return (samples + 1) / 2;
    return format == PCM16 ? samples * 2 : samples;
}

static void pcm_load(const Ma3Synth *s, Ma3PcmVoice *pcm, unsigned address)
{
    const uint8_t *data;
    pcm->preset = pcm->end = 0;
    pcm->streaming = 0;
    if (address < MA3_RAM_BASE) {
        /* Short synthesized percussion at the ROM wave addresses. Give these
           the same pitch, gate and channel path as uploaded PCM. */
        static const uint8_t percussion[] = {0, 0, 0x80, 0x88, 0xf6, 0, 0};
        if (address < 0x800) return;
        pcm->preset = 1 + (address >> 4) % 4;
        pcm->format = PCM8;
        pcm->end = pcm->loop = 1200;
        pcm->gain = 0.45f;
        envelope_load(s, &pcm->envelope, percussion + 2);
        return;
    }
    if (address > MA3_RAM_END - 13) return;
    data = s->ram + address - MA3_RAM_BASE;
    pcm->format = data[1] & 3u;
    pcm->start = big_endian16(data + 7);
    pcm->loop = big_endian16(data + 9);
    pcm->end = big_endian16(data + 11);
    if (pcm->start < MA3_RAM_BASE || pcm->start + pcm_bytes(pcm->format, pcm->end) > MA3_RAM_END)
        pcm->end = 0;
    if (pcm->loop > pcm->end) pcm->loop = pcm->end;
    pcm->gain = s->level[data[5] >> 2];
    envelope_load(s, &pcm->envelope, data + 2);
}

static void pcm_key(Ma3Synth *s, unsigned index, unsigned value)
{
    Ma3PcmVoice *pcm = &s->pcm[index];
    unsigned gate = value & 0x40u;
    if (gate == pcm->voice.gate) return;
    pcm->voice.gate = gate;
    envelope_key(&pcm->envelope, gate, value & 0x10u);
    if (gate) {
        pcm->phase = pcm->decoded = 0;
        pcm->predictor = pcm->loop_predictor = 0;
        pcm->adpcm_step = pcm->loop_step = 127;
        if (pcm->end) s->active_pcm |= UINT32_C(1) << index;
    }
}

void ma3_synth_write(Ma3Synth *s, const uint8_t *parameters, unsigned address, unsigned value)
{
    unsigned offset, index;
    Ma3Voice *voice;
    Ma3FmVoice *fm = NULL;
    Ma3PcmVoice *pcm = NULL;
    if (address >= 0xe0) {
        offset = address - 0xe0;
        index = 16 + offset / 6;
        fm = &s->fm[index];
    } else if (address < 0x60) {
        offset = address;
        index = offset / 6;
        fm = &s->fm[index];
    } else if (address < 0x90) {
        offset = address - 0x60;
        index = offset / 6;
        pcm = &s->pcm[index];
    } else {
        if (address < 0xa0) s->channel_gain[address - 0x90] = s->gain[value >> 2];
        else if (address < 0xb0) s->pan[address - 0xa0] = ((float)(value >> 2) - 15.0f) / 16.0f;
        return;
    }
    voice = fm ? &fm->voice : &pcm->voice;
    switch (offset % 6) {
    case 1: { /* The guest writes a 14-bit word address, high part first. */
        unsigned instrument = ((unsigned)parameters[address - 1] << 8) | (value << 1);
        if (fm) fm_load(s, fm, instrument);
        else pcm_load(s, pcm, instrument);
        break;
    }
    case 2:
        voice->gain = s->gain[value >> 2];
        break;
    case 3:
        voice->pitch_high = value;
        break;
    case 4:
        voice->pitch_low = value;
        if (fm) fm_pitch(fm);
        else pcm->step = (fnum(voice) + 1024u) << ((voice->pitch_high & 7u) + 1u);
        break;
    case 5:
        voice->channel = voice->pitch_high & 0x40u ? 16 : value & 15u;
        if (fm) fm_key(s, index, value);
        else pcm_key(s, index, value);
        break;
    }
}

void ma3_synth_ram(Ma3Synth *s, unsigned address, uint8_t value)
{
    unsigned i;
    if (address < MA3_RAM_BASE || address >= MA3_RAM_END) return;
    s->ram[address - MA3_RAM_BASE] = value;
    /* A rewritten loop is a streaming ring. Decode fresh data on each lap
       and carry the ADPCM predictor across the wrap, as across any refill. */
    for (i = 0; i < 8 && s->active_pcm; ++i) {
        Ma3PcmVoice *pcm = &s->pcm[i];
        if ((s->active_pcm & (UINT32_C(1) << i)) && pcm->format == ADPCM4 &&
            address >= pcm->start && address < pcm->start + pcm_bytes(pcm->format, pcm->end))
            pcm->streaming = 1;
    }
}

unsigned ma3_synth_cursor(const Ma3Synth *s, unsigned voice)
{
    const Ma3PcmVoice *pcm = &s->pcm[voice];
    unsigned sample = pcm->phase >> 16;
    /* Both ADPCM nibbles belong to the same byte; do not round up like a
       buffer length. The firmware refills 32-byte blocks behind this cursor. */
    unsigned byte = pcm->format == ADPCM4 ? sample / 2 : pcm_bytes(pcm->format, sample);
    unsigned offset = (byte + 0x20u) & 0x3ffu;
    return 0x40u | (pcm->format < PCM8 ? (offset >> 4) & 0x3eu : offset >> 5);
}

static float fm_wave(const Ma3Synth *s, unsigned type, unsigned index)
{
    unsigned phase = index & 1023u;
    float wave = s->sine[phase];
    /* Approximate the alternate waveforms with rectified and harmonic sine
       shapes. All operators, including unmodulated carriers, use one table. */
    switch (type & 7u) {
    case 1: return phase < 512 ? wave : 0.0f;
    case 2: return fabsf(wave);
    case 3: return phase < 256 ? wave : 0.0f;
    case 4: return s->sine[(index * 2) & 1023u];
    case 5: return phase < 512 ? s->sine[(index * 2) & 1023u] : 0.0f;
    case 6: return phase < 512 ? 1.0f : -1.0f;
    case 7: return (float)phase / 512.0f - 1.0f;
    default: return wave;
    }
}

static float fm_sample(Ma3Synth *s, unsigned index)
{
    /* Each mask names earlier operators that modulate this operator. The
       last column names the carriers whose output is heard. */
    static const uint8_t algorithms[8][5] = {
        {0, 1, 0, 0, 2}, {0, 0, 0, 0, 3},
        {0, 0, 0, 0, 15}, {0, 0, 2, 5, 8},
        {0, 1, 2, 4, 8}, {0, 1, 0, 4, 10},
        {0, 0, 2, 4, 9}, {0, 0, 2, 0, 13}
    };
    Ma3FmVoice *fm = &s->fm[index];
    const uint8_t *routing = algorithms[fm->algorithm];
    unsigned count = fm->algorithm < 2 ? 2u : 4u;
    float values[4], sum = 0.0f;
    unsigned i, active = 0;
    for (i = 0; i < count; ++i) {
        Ma3Operator *op = &fm->operators[i];
        float mod = 0.0f;
        float level = envelope_step(&op->envelope);
        unsigned j, phase;
        for (j = 0; j < i; ++j) if (routing[i] & (1u << j)) mod += values[j];
        mod += op->previous * (float)op->feedback * 0.25f;
        phase = (op->phase >> 22) + (unsigned)(int)(mod * 512.0f);
        values[i] = fm_wave(s, op->wave, phase) * level * op->gain;
        op->previous = values[i];
        op->phase += op->step;
        if (op->envelope.stage != ENV_OFF) active = 1;
        if (routing[4] & (1u << i)) sum += values[i];
    }
    if (!active) s->active_fm &= ~(UINT32_C(1) << index);
    return sum;
}

static int32_t adpcm_sample(const Ma3Synth *s, const Ma3PcmVoice *pcm,
                            unsigned index, unsigned *step, int32_t predictor)
{
    unsigned byte = s->ram[pcm->start - MA3_RAM_BASE + index / 2];
    return sgh_adpcm_next(byte >> ((index & 1u) * 4), step, predictor);
}

static void adpcm_decode_to(const Ma3Synth *s, Ma3PcmVoice *pcm, unsigned last)
{
    while (pcm->decoded <= last) {
        if (pcm->decoded == pcm->loop && !pcm->streaming) {
            pcm->loop_predictor = pcm->predictor;
            pcm->loop_step = pcm->adpcm_step;
        }
        pcm->predictor = adpcm_sample(s, pcm, pcm->decoded, &pcm->adpcm_step, pcm->predictor);
        ++pcm->decoded;
    }
}

static int32_t pcm_value(const Ma3Synth *s, const Ma3PcmVoice *pcm, unsigned index)
{
    const uint8_t *sample;
    unsigned raw;
    if (pcm->preset) {
        uint32_t noise = (index + pcm->preset * 613u) * UINT32_C(0x9e3779b9);
        float tone = s->sine[(index * (6u + pcm->preset * 3u)) & 1023u];
        float random = (float)(noise >> 16) / 32768.0f - 1.0f;
        float fade = (float)(pcm->end - index) / (float)pcm->end;
        if (pcm->preset & 1u) tone = 0.75f * random + 0.25f * tone;
        return (int32_t)(tone * fade * fade * 18000.0f);
    }
    sample = s->ram + pcm->start - MA3_RAM_BASE;
    if (pcm->format != PCM16) {
        raw = sample[index];
        return (raw < 128 ? (int)raw : (int)raw - 256) * 256;
    }
    raw = sample[index * 2] | ((unsigned)sample[index * 2 + 1] << 8);
    return raw < 32768 ? (int)raw : (int)raw - 65536;
}

static float pcm_sample(Ma3Synth *s, unsigned voice)
{
    Ma3PcmVoice *pcm = &s->pcm[voice];
    unsigned index = pcm->phase >> 16;
    unsigned next = index + 1, step;
    int32_t a, b;
    float level = envelope_step(&pcm->envelope);
    if (index >= pcm->end || pcm->envelope.stage == ENV_OFF) {
        s->active_pcm &= ~(UINT32_C(1) << voice);
        return 0.0f;
    }
    if (next == pcm->end) next = pcm->loop < pcm->end ? pcm->loop : index;
    if (pcm->format == ADPCM4) {
        /* Interpolate without committing a future nibble. At a live ring
           boundary hold the tail until the decoder actually wraps. */
        adpcm_decode_to(s, pcm, index);
        a = b = pcm->predictor;
        if (next > index) {
            step = pcm->adpcm_step;
            b = adpcm_sample(s, pcm, next, &step, a);
        } else if (next < index && !pcm->streaming) {
            step = pcm->loop_step;
            b = adpcm_sample(s, pcm, next, &step, pcm->loop_predictor);
        }
    } else {
        a = pcm_value(s, pcm, index);
        b = pcm_value(s, pcm, next);
    }
    pcm->phase += pcm->step;
    while ((pcm->phase >> 16) >= pcm->end && pcm->loop < pcm->end) {
        pcm->phase -= (pcm->end - pcm->loop) << 16;
        if (pcm->format == ADPCM4) {
            if (pcm->streaming) adpcm_decode_to(s, pcm, pcm->end - 1);
            else {
                pcm->predictor = pcm->loop_predictor;
                pcm->adpcm_step = pcm->loop_step;
            }
            pcm->decoded = pcm->loop;
        }
    }
    return ((float)a + (float)(b - a) * (float)((pcm->phase - pcm->step) & 0xffffu) / 65536.0f) *
           (level * pcm->gain / 32768.0f);
}

static void add_voice(const Ma3Synth *s, const Ma3Voice *voice, float sample, float *left, float *right)
{
    float pan = s->pan[voice->channel];
    sample *= voice->gain * s->channel_gain[voice->channel];
    *left += sample * (1.0f - (pan > 0.0f ? pan : 0.0f));
    *right += sample * (1.0f + (pan < 0.0f ? pan : 0.0f));
}

static int16_t clip(float sample)
{
    if (sample > 32767.0f) return 32767;
    if (sample < -32768.0f) return -32768;
    return (int16_t)sample;
}

void ma3_synth_render(Ma3Synth *s, int16_t *left, int16_t *right, unsigned frames)
{
    float gain = s->gain[s->volume] * 32768.0f;
    unsigned i, voice;
    for (i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        uint32_t active = s->active_fm;
        for (voice = 0; active; ++voice, active >>= 1) {
            if (active & 1u) add_voice(s, &s->fm[voice].voice, fm_sample(s, voice) * 0.39f, &l, &r);
        }
        active = s->active_pcm;
        for (voice = 0; active; ++voice, active >>= 1) {
            if (active & 1u) add_voice(s, &s->pcm[voice].voice, pcm_sample(s, voice) * 0.90f, &l, &r);
        }
        left[i] = clip(l * gain);
        right[i] = clip(r * gain);
    }
}
