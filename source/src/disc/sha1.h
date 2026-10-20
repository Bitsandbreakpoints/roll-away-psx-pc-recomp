/*
 * sha1.h - SHA-1 (FIPS 180-1), used only to recognise the disc image.
 */
#ifndef DISC_SHA1_H
#define DISC_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[5];
    uint64_t length;                /* bytes */
    uint8_t block[64];
    uint32_t used;
} Sha1;

void sha1_init(Sha1 *ctx);
void sha1_update(Sha1 *ctx, const void *data, size_t size);
void sha1_final(Sha1 *ctx, uint8_t digest[20]);

#ifdef __cplusplus
}
#endif

#endif /* DISC_SHA1_H */
