/*
 * mdec.h - the PS1's movie decoder (MDEC) in software (mdec.c): it takes
 * the run-length codes LIBPRESS's DecDCTvlc makes from a movie frame and
 * hands out the decoded 16x16 pixel macroblocks, one after the other, as
 * the chip does.
 */
#ifndef PSX_MDEC_H
#define PSX_MDEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDEC_MACROBLOCK         16      /* pixels per side */

/* New input: `halfwords` 16-bit codes (they stay where they are). */
void mdec_start(const uint16_t *codes, int halfwords);

/*
 * Decodes the next `count` macroblocks into `out`, each 16 lines of 16
 * pixels: 24-bit (R, G, B bytes) or 15-bit (with bit 15 set if
 * `set_bit15`).  Macroblocks past the end of the codes are grey.
 */
void mdec_output(uint8_t *out, int count, int depth_24bit, int set_bit15);

#ifdef __cplusplus
}
#endif

#endif /* PSX_MDEC_H */
