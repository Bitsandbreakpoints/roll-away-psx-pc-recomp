/*
 * audio.h - sound output (XAudio2), 44.1 kHz 16-bit stereo.
 */
#ifndef PLATFORM_AUDIO_H
#define PLATFORM_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FALSE: no sound device (the game runs silent). */
int audio_init(void);
void audio_shutdown(void);

/* Queues `frames` interleaved stereo samples. */
void audio_submit(const int16_t *samples, int frames);

/* Real time: a thread of its own keeps a few frames of `frames` samples
 * queued, asking fill() for each (not together with audio_submit()).
 * FALSE without a sound device. */
typedef void (*AudioFill)(int16_t *samples, int frames);

int audio_stream_start(AudioFill fill, int frames);
void audio_stream_stop(void);

/* Drops what is queued (leaving the game). */
void audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_AUDIO_H */
