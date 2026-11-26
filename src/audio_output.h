#ifndef SGH_AUDIO_OUTPUT_H
#define SGH_AUDIO_OUTPUT_H

#include <stdint.h>

enum { AUDIO_RATE = 48000, AUDIO_BLOCK = 480 };
typedef struct AudioOutput AudioOutput;

/* Buffer interleaved signed 16-bit PCM for WaveOut/ALSA and/or a WAV file. */
AudioOutput *audio_output_open(unsigned channels, int speakers, const char *wav);
int audio_output_write(AudioOutput *output, const int16_t *samples, unsigned frames);
int audio_output_close(AudioOutput *output);
int audio_output_live(const AudioOutput *output);
void audio_output_pause(AudioOutput *output, int paused);
uint64_t audio_output_underruns(const AudioOutput *output);

#endif
