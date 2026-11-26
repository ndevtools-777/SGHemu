#ifndef SGH_ALSA_PCM_H
#define SGH_ALSA_PCM_H

#include <stdint.h>

typedef struct {
    struct _snd_pcm *device;
    unsigned channels, underruns;
    int paused;
} HostPcm;

int host_pcm_open(HostPcm *pcm, unsigned channels);
int host_pcm_write(HostPcm *pcm, const int16_t *samples, unsigned frames);
void host_pcm_pause(HostPcm *pcm, int paused);
void host_pcm_close(HostPcm *pcm);

#endif
