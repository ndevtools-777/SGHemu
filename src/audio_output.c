#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#else
#include "alsa_pcm.h"
#endif
#include "audio_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OUTPUT_SLOTS = 12 };

struct AudioOutput {
#ifdef _WIN32
    HWAVEOUT wave;
    HANDLE event;
    WAVEHDR headers[OUTPUT_SLOTS];
    uint64_t underruns;
    int timer_period, wave_started;
#else
    HostPcm device;
#endif
    FILE *wav;
    uint64_t written;
    unsigned channels, slot, fill;
    int16_t samples[OUTPUT_SLOTS][AUDIO_BLOCK * 2];
    int failed, paused;
};

static void put_le32(uint8_t *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static int wav_header(AudioOutput *out)
{
    uint8_t header[44] = {0};
    uint32_t align = out->channels * 2;
    uint32_t bytes = (uint32_t)out->written * align;
    memcpy(header, "RIFF", 4);
    put_le32(header + 4, bytes + 36);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le32(header + 16, 16);
    header[20] = 1;
    header[22] = (uint8_t)out->channels;
    put_le32(header + 24, AUDIO_RATE);
    put_le32(header + 28, AUDIO_RATE * align);
    header[32] = (uint8_t)align;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, bytes);
    return !fseek(out->wav, 0, SEEK_SET) && fwrite(header, 1, sizeof(header), out->wav) == sizeof(header);
}

static int wav_write(AudioOutput *out)
{
    unsigned align = out->channels * 2;
    if (!out->wav || !out->fill) return 1;
    if (out->written + out->fill > (UINT32_MAX - 36u) / align) return 0;
    if (fwrite(out->samples[out->slot], align, out->fill, out->wav) != out->fill) return 0;
    out->written += out->fill;
    return 1;
}

static int submit(AudioOutput *out)
{
    int ok = wav_write(out);
#ifdef _WIN32
    if (ok && out->wave && !out->paused) {
        WAVEHDR *header = &out->headers[out->slot];
        unsigned previous = (out->slot + OUTPUT_SLOTS - 1) % OUTPUT_SLOTS;
        /* WaveOut plays buffers in order. If the last submission has
           finished before this one, the playback queue ran dry. */
        if (out->wave_started && (out->headers[previous].dwFlags & WHDR_DONE)) ++out->underruns;
        memset(header, 0, sizeof(*header));
        header->lpData = (LPSTR)out->samples[out->slot];
        header->dwBufferLength = out->fill * out->channels * 2;
        ok = waveOutPrepareHeader(out->wave, header, sizeof(*header)) == MMSYSERR_NOERROR &&
             waveOutWrite(out->wave, header, sizeof(*header)) == MMSYSERR_NOERROR;
        out->wave_started = ok;
    }
#else
    if (ok) ok = host_pcm_write(&out->device, out->samples[out->slot], out->fill);
#endif
    out->fill = 0;
    out->slot = (out->slot + 1) % OUTPUT_SLOTS;
#ifdef _WIN32
    /* WaveOut owns the memory until it signals completion. */
    if (ok && out->wave) {
        WAVEHDR *next = &out->headers[out->slot];
        if (next->dwFlags & WHDR_PREPARED) {
            if (out->paused && !(next->dwFlags & WHDR_DONE)) waveOutReset(out->wave);
            while (!(next->dwFlags & WHDR_DONE)) {
                if (WaitForSingleObject(out->event, 2000) != WAIT_OBJECT_0) return 0;
            }
            ok = waveOutUnprepareHeader(out->wave, next, sizeof(*next)) == MMSYSERR_NOERROR;
        }
    }
#endif
    return ok;
}

AudioOutput *audio_output_open(unsigned channels, int speakers, const char *wav)
{
    AudioOutput *out;
    if (channels != 1 && channels != 2) return NULL;
    out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->channels = channels;
    if (wav) {
        out->wav = fopen(wav, "wb");
        if (!out->wav || !wav_header(out)) goto failed;
    }
    if (speakers) {
#ifdef _WIN32
        WAVEFORMATEX format = {0};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = (WORD)channels;
        format.nSamplesPerSec = AUDIO_RATE;
        format.wBitsPerSample = 16;
        format.nBlockAlign = (WORD)(channels * 2);
        format.nAvgBytesPerSec = AUDIO_RATE * format.nBlockAlign;
        out->event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!out->event || waveOutOpen(&out->wave, WAVE_MAPPER, &format,
                                      (DWORD_PTR)out->event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) goto failed;
        out->timer_period = timeBeginPeriod(1) == TIMERR_NOERROR;
#else
        if (!host_pcm_open(&out->device, channels)) goto failed;
#endif
    }
    return out;
failed:
    audio_output_close(out);
    return NULL;
}

int audio_output_write(AudioOutput *out, const int16_t *samples, unsigned frames)
{
    if (!out) return 1;
    while (frames && !out->failed) {
        unsigned count = AUDIO_BLOCK - out->fill;
        if (count > frames) count = frames;
        memcpy(out->samples[out->slot] + out->fill * out->channels, samples,
               count * out->channels * sizeof(*samples));
        samples += count * out->channels;
        frames -= count;
        out->fill += count;
        if (out->fill == AUDIO_BLOCK && !submit(out)) out->failed = 1;
    }
    return !out->failed;
}

int audio_output_live(const AudioOutput *out)
{
    if (!out || out->failed) return 0;
#ifdef _WIN32
    return out->wave != NULL;
#else
    return out->device.device != NULL;
#endif
}

void audio_output_pause(AudioOutput *out, int paused)
{
    if (!out || out->paused == !!paused) return;
    out->paused = !!paused;
#ifdef _WIN32
    if (out->wave) {
        if (paused) {
            out->wave_started = 0;
            waveOutPause(out->wave);
        }
        else waveOutRestart(out->wave);
    }
#else
    host_pcm_pause(&out->device, paused);
#endif
}

uint64_t audio_output_underruns(const AudioOutput *out)
{
#ifdef _WIN32
    return out ? out->underruns : 0;
#else
    return out ? out->device.underruns : 0;
#endif
}

int audio_output_close(AudioOutput *out)
{
    int ok;
    if (!out) return 1;
    ok = !out->failed;
    if (out->wav) {
        if (!wav_write(out) || !wav_header(out)) ok = 0;
        if (fclose(out->wav)) ok = 0;
    }
#ifdef _WIN32
    if (out->wave) {
        unsigned i;
        waveOutReset(out->wave);
        for (i = 0; i < OUTPUT_SLOTS; ++i) {
            if (out->headers[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(out->wave, &out->headers[i], sizeof(WAVEHDR));
        }
        waveOutClose(out->wave);
    }
    if (out->event) CloseHandle(out->event);
    if (out->timer_period) timeEndPeriod(1);
#else
    host_pcm_close(&out->device);
#endif
    free(out);
    return ok;
}
