/*
 * mdec.c - the movie decoder (MDEC) in software, after psx-spx ("MDEC").
 * The dequantisation, the integer inverse DCT and the colour conversion
 * come from the D PC port (same author).
 *
 * The codes: per 8x8 block a DC code (quantisation scale << 10 | DC),
 * then (run << 10 | level) codes, ended by 0xFE00.  Six blocks per 16x16
 * macroblock: Cr, Cb, then the four Y blocks.  The output is the
 * macroblock's pixels line by line.
 */
#include <math.h>
#include <string.h>
#include "psx/mdec.h"

#define BLOCK_SIZE              8
#define BLOCK_COEFFS            64
#define BLOCKS_PER_MACROBLOCK   6
#define CODE_END_OF_BLOCK       0xFE00u
#define CODE_RUN_SHIFT          10
#define SIGNED_10_BIT(x)        ((int) ((x) & 0x3FF) - (((x) & 0x200) ? 0x400 : 0))
#define COEFF_MIN               (-0x400)
#define COEFF_MAX               0x3FF

/* zigzag index -> position in the block (row * 8 + column) */
static const uint8_t ZIGZAG[BLOCK_COEFFS] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* MPEG-1's default intra quantisation matrix (row-major), with 2 for the
 * DC coefficient: what LIBPRESS's DecDCTReset loads */
static const uint8_t DEFAULT_QUANT[BLOCK_COEFFS] = {
    2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

/*
 * The inverse DCT in integers as the chip does it: the scale table
 * 32768 * C(u) * cos((2y + 1) u pi / 16), rounded down, C(0) = 1/sqrt(2);
 * two passes with 64-bit sums, the result from bit 32 up, rounded, as a
 * 9-bit signed value clamped to -128..127.
 */
#define SCALE_ONE               32768.0
#define SAMPLE_MIN              (-128)
#define SAMPLE_MAX              127
#define RESULT_SHIFT            32
#define BYTE_OFFSET             128     /* unsigned output */

static int16_t s_scale[BLOCK_COEFFS];   /* [u * 8 + y] */
static int s_scale_ready;

static const uint16_t *s_codes;
static int s_count, s_position;

static void build_scale_table(void)
{
    for (int u = 0; u < BLOCK_SIZE; u++) {
        double c = u == 0 ? sqrt(0.5) : 1.0;
        for (int y = 0; y < BLOCK_SIZE; y++) {
            s_scale[u * BLOCK_SIZE + y] =
                (int16_t) floor(SCALE_ONE * c * cos((2 * y + 1) * u * M_PI / 16));
        }
    }
    s_scale_ready = 1;
}

void mdec_start(const uint16_t *codes, int halfwords)
{
    s_codes = codes;
    s_count = codes ? halfwords : 0;
    s_position = 0;
}

static int sign_extend_9(int64_t v)
{
    int x = (int) (v & 0x1FF);
    return x & 0x100 ? x - 0x200 : x;
}

/* Decodes one block's codes into 64 samples (-128..127), [y * 8 + x];
 * zeros when the codes have run out. */
static void decode_block(int16_t out[BLOCK_COEFFS])
{
    int16_t coeff[BLOCK_COEFFS] = { 0 };
    uint16_t code;

    /* (the MDEC skips padding codes before a block) */
    do {
        if (s_position >= s_count) {
            memset(out, 0, sizeof(int16_t) * BLOCK_COEFFS);
            return;
        }
        code = s_codes[s_position++];
    } while (code == CODE_END_OF_BLOCK);
    int qscale = code >> CODE_RUN_SHIFT;
    int k = 0;
    int value = SIGNED_10_BIT(code) * DEFAULT_QUANT[0];
    for (;;) {
        if (qscale == 0) {
            value = SIGNED_10_BIT(code) * 2;    /* special mode: no matrix, no zigzag */
        }
        value = value < COEFF_MIN ? COEFF_MIN : value > COEFF_MAX ? COEFF_MAX : value;
        coeff[qscale ? ZIGZAG[k] : k] = (int16_t) value;
        if (s_position >= s_count) {
            break;
        }
        code = s_codes[s_position++];
        if (code == CODE_END_OF_BLOCK) {
            break;
        }
        k += (code >> CODE_RUN_SHIFT) + 1;
        if (k >= BLOCK_COEFFS) {
            break;
        }
        value = (SIGNED_10_BIT(code) * DEFAULT_QUANT[ZIGZAG[k]] * qscale + 4) / 8;
    }
    /* columns (vertical frequencies -> lines), then lines */
    int64_t temp[BLOCK_COEFFS];
    for (int x = 0; x < BLOCK_SIZE; x++) {
        for (int y = 0; y < BLOCK_SIZE; y++) {
            int64_t sum = 0;
            for (int u = 0; u < BLOCK_SIZE; u++) {
                sum += (int64_t) coeff[u * BLOCK_SIZE + x] * s_scale[u * BLOCK_SIZE + y];
            }
            temp[x + y * BLOCK_SIZE] = sum;
        }
    }
    for (int x = 0; x < BLOCK_SIZE; x++) {
        for (int y = 0; y < BLOCK_SIZE; y++) {
            int64_t sum = 0;
            for (int u = 0; u < BLOCK_SIZE; u++) {
                sum += temp[u + y * BLOCK_SIZE] * s_scale[u * BLOCK_SIZE + x];
            }
            int v = sign_extend_9((sum >> RESULT_SHIFT) + ((sum >> (RESULT_SHIFT - 1)) & 1));
            out[x + y * BLOCK_SIZE] = (int16_t) (v < SAMPLE_MIN ? SAMPLE_MIN
                                                 : v > SAMPLE_MAX ? SAMPLE_MAX : v);
        }
    }
}

static int to_byte(int v)
{
    return (v < SAMPLE_MIN ? SAMPLE_MIN : v > SAMPLE_MAX ? SAMPLE_MAX : v) + BYTE_OFFSET;
}

void mdec_output(uint8_t *out, int count, int depth_24bit, int set_bit15)
{
    int16_t block[BLOCKS_PER_MACROBLOCK][BLOCK_COEFFS];
    if (!s_scale_ready) {
        build_scale_table();
    }
    for (int m = 0; m < count; m++) {
        for (int b = 0; b < BLOCKS_PER_MACROBLOCK; b++) {
            decode_block(block[b]);
        }
        /* YCbCr -> RGB (ITU-R BT.601, as the MDEC: the colour terms cut
         * towards zero) */
        for (int y = 0; y < MDEC_MACROBLOCK; y++) {
            for (int x = 0; x < MDEC_MACROBLOCK; x++) {
                int c = (y / 2) * BLOCK_SIZE + x / 2;
                int cr = block[0][c], cb = block[1][c];
                int q = (y / BLOCK_SIZE) * 2 + x / BLOCK_SIZE;
                int luma = block[2 + q][(y % BLOCK_SIZE) * BLOCK_SIZE + x % BLOCK_SIZE];
                int red = to_byte(luma + (int16_t) (1.402f * cr));
                int green = to_byte(luma + (int16_t) (-0.3437f * cb + -0.7143f * cr));
                int blue = to_byte(luma + (int16_t) (1.772f * cb));
                if (depth_24bit) {
                    *out++ = (uint8_t) red;
                    *out++ = (uint8_t) green;
                    *out++ = (uint8_t) blue;
                } else {
                    uint16_t p = (uint16_t) (red >> 3 | (green >> 3) << 5 | (blue >> 3) << 10
                                             | (set_bit15 ? 0x8000 : 0));
                    *out++ = (uint8_t) p;
                    *out++ = (uint8_t) (p >> 8);
                }
            }
        }
    }
}
