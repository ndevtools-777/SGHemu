#define _POSIX_C_SOURCE 200809L
#include "alsa_pcm.h"
#include <alloca.h>
#include <alsa/asoundlib.h>

int host_pcm_open(HostPcm *pcm, unsigned channels)
{
    snd_pcm_sw_params_t *params;
    snd_pcm_uframes_t buffer, period;
    snd_pcm_sw_params_alloca(&params);
    if (snd_pcm_open(&pcm->device, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
        return 0;
    pcm->channels = channels;
    if (snd_pcm_set_params(pcm->device, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, channels, 48000, 1,
                           80000) < 0 ||
        snd_pcm_get_params(pcm->device, &buffer, &period) < 0 ||
        snd_pcm_sw_params_current(pcm->device, params) < 0 ||
        snd_pcm_sw_params_set_start_threshold(pcm->device, params, period) < 0 ||
        snd_pcm_sw_params(pcm->device, params) < 0) {
        host_pcm_close(pcm);
        return 0;
    }
    return 1;
}

int host_pcm_write(HostPcm *pcm, const int16_t *samples, unsigned frames)
{
    int waited = 0, recovered = 0;
    if (!pcm->device || pcm->paused)
        return 1;
    while (frames) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm->device, samples, frames);
        if (written > 0) {
            frames -= (unsigned)written;
            samples += (unsigned)written * pcm->channels;
        } else if (written == -EPIPE || written == -ESTRPIPE) {
            if (recovered || snd_pcm_prepare(pcm->device) < 0)
                return 0;
            if (written == -EPIPE)
                ++pcm->underruns;
            recovered = 1;
        } else if (written == 0 || written == -EAGAIN || written == -EINTR) {
            /* At most one short wait for a full queue; never stall the phone
               behind a suspended or overloaded host audio server. */
            if (waited || snd_pcm_wait(pcm->device, 20) <= 0)
                return 1;
            waited = 1;
        } else
            return 0;
    }
    return 1;
}

void host_pcm_pause(HostPcm *pcm, int paused)
{
    if (pcm->paused == !!paused)
        return;
    pcm->paused = !!paused;
    if (!pcm->device)
        return;
    if (paused)
        snd_pcm_drop(pcm->device);
    else
        snd_pcm_prepare(pcm->device);
}

void host_pcm_close(HostPcm *pcm)
{
    if (!pcm->device)
        return;
    snd_pcm_close(pcm->device);
    pcm->device = NULL;
}
