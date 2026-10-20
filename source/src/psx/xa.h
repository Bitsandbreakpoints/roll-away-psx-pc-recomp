/*
 * xa.h - CD-ROM XA audio: the ADPCM sectors of the movies and the music,
 * decoded and resampled to the PC's 44.1 kHz (xa.c).
 */
#ifndef PSX_XA_H
#define PSX_XA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forgets what is still to be played (the drive stopped reading). */
void xa_reset(void);

/* Decodes one XA audio sector (all 2352 bytes) and queues its sound. */
void xa_decode_sector(const uint8_t *sector);

/*
 * Adds `frames` stereo samples of the queued sound at 44.1 kHz to `out`,
 * at volumes 0..256 (256 = as decoded).
 */
void xa_mix(int16_t *out, int frames, int volume_left, int volume_right);

#ifdef __cplusplus
}
#endif

#endif /* PSX_XA_H */
