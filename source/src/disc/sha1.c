/*
 * sha1.c - SHA-1 (FIPS 180-1).
 */
#include <string.h>
#include "disc/sha1.h"

#define ROL(x, n)               (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(Sha1 *ctx, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t) p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

void sha1_init(Sha1 *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->length = 0;
    ctx->used = 0;
}

void sha1_update(Sha1 *ctx, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *) data;
    ctx->length += size;
    while (size > 0) {
        size_t n = 64 - ctx->used;
        if (n > size) {
            n = size;
        }
        memcpy(ctx->block + ctx->used, p, n);
        ctx->used += (uint32_t) n;
        p += n;
        size -= n;
        if (ctx->used == 64) {
            sha1_block(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void sha1_final(Sha1 *ctx, uint8_t digest[20])
{
    uint64_t bits = ctx->length * 8;
    uint8_t pad = 0x80;
    uint8_t len[8];
    int i;

    sha1_update(ctx, &pad, 1);
    pad = 0;
    while (ctx->used != 56) {
        sha1_update(ctx, &pad, 1);
    }
    for (i = 0; i < 8; i++) {
        len[i] = (uint8_t) (bits >> (56 - 8 * i));
    }
    sha1_update(ctx, len, 8);
    for (i = 0; i < 20; i++) {
        digest[i] = (uint8_t) (ctx->state[i / 4] >> (24 - 8 * (i % 4)));
    }
}
