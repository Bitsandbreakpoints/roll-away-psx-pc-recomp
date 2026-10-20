/*
 * xa.c - CD-ROM XA audio (ADPCM), as the CD drive plays it, after psx-spx
 * ("CDROM XA Audio ADPCM Compression").
 *
 * A Form 2 audio sector holds 18 sound groups of 128 bytes: 16 bytes of
 * parameters (shift and filter per unit) and 112 bytes of samples.  With
 * 4 bits per sample a group has 8 units of 28 samples; mono plays them one
 * after the other, stereo alternates left and right.  The decoded sound
 * (18.9 or 37.8 kHz) waits in a queue and is resampled when it is mixed at
 * 44.1 kHz: Lanczos (a = 4), which keeps the treble as the console's own
 * filter does (linear interpolation dulled 1-4 kHz by about 2 dB against
 * DuckStation in the D PC port, same author).
 */
#include <math.h>
#include <string.h>
#include "psx/xa.h"

/* the sector */
#define SUBHEADER_OFFSET        16
#define SUBHEADER_CODING        3
#define DATA_OFFSET             24
#define GROUPS                  18
#define GROUP_SIZE              128
#define GROUP_PARAMETERS        4       /* the units' parameters start here */
#define GROUP_SAMPLES           16      /* the samples start here */
#define SAMPLES_PER_UNIT        28

/* the coding byte */
#define CODING_STEREO           0x01
#define CODING_HALF_RATE        0x04    /* 18.9 kHz instead of 37.8 kHz */
#define CODING_8BIT             0x10

#define RATE_FULL               37800
#define RATE_HALF               18900
#define OUTPUT_RATE             44100

#define UNITS_4BIT              8
#define UNITS_8BIT              4
#define SHIFT_BASE_4BIT         12
#define SHIFT_BASE_8BIT         8
#define FILTER_SHIFT            4
#define FILTER_MASK             3
#define SHIFT_MASK              0x0F

static const int FILTER_POS[] = { 0, 60, 115, 98, 122 };
static const int FILTER_NEG[] = { 0, 0, -52, -55, -60 };

/* the resampler: LANCZOS_TAPS samples around the position, the kernel
 * in a table of LANCZOS_PHASES positions between two samples */
#define LANCZOS_A               4
#define LANCZOS_TAPS            (2 * LANCZOS_A)
#define LANCZOS_PHASES          256
#define KERNEL_ONE              16384   /* fixed point of the table */
static int16_t s_kernel[LANCZOS_PHASES][LANCZOS_TAPS];
static int s_kernel_ready;

static double lanczos(double x)
{
    if (x == 0) {
        return 1;
    }
    if (fabs(x) >= LANCZOS_A) {
        return 0;
    }
    double px = M_PI * x;
    return LANCZOS_A * sin(px) * sin(px / LANCZOS_A) / (px * px);
}

static void build_kernel(void)
{
    for (int p = 0; p < LANCZOS_PHASES; p++) {
        double frac = (double) p / LANCZOS_PHASES, sum = 0, w[LANCZOS_TAPS];
        for (int k = 0; k < LANCZOS_TAPS; k++) {
            w[k] = lanczos(frac - (k - (LANCZOS_A - 1)));
            sum += w[k];
        }
        for (int k = 0; k < LANCZOS_TAPS; k++) {
            s_kernel[p][k] = (int16_t) lrint(w[k] / sum * KERNEL_ONE);    /* (gain 1) */
        }
    }
    s_kernel_ready = 1;
}

/* the queue: stereo frames at the sectors' rate; the resampler's position
 * is LANCZOS_A - 1 frames after s_read (the frames before it are history) */
#define QUEUE_FRAMES            (RATE_FULL * 2)     /* two seconds */
static int16_t s_queue[QUEUE_FRAMES][2];
static int s_read, s_count;
static int s_rate = RATE_HALF;
static uint32_t s_phase;                /* 16.16 position between two queued frames */

/* the decoder's history per channel */
static int s_old[2], s_older[2];

void xa_reset(void)
{
    memset(s_queue, 0, sizeof(s_queue[0]) * (LANCZOS_A - 1));
    s_read = 0;
    s_count = LANCZOS_A - 1;                /* silent history */
    s_phase = 0;
    s_old[0] = s_old[1] = s_older[0] = s_older[1] = 0;
}

static int16_t clamp16(int v)
{
    return (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* the unit's 28 samples */
static void decode_unit(const uint8_t *group, int unit, int eight_bit, int channel,
                        int16_t out[SAMPLES_PER_UNIT])
{
    uint8_t parameters = group[GROUP_PARAMETERS + unit];
    int filter = (parameters >> FILTER_SHIFT) & FILTER_MASK;
    int shift = (eight_bit ? SHIFT_BASE_8BIT : SHIFT_BASE_4BIT) - (parameters & SHIFT_MASK);
    int f0 = FILTER_POS[filter], f1 = FILTER_NEG[filter];
    int old = s_old[channel], older = s_older[channel];
    for (int i = 0; i < SAMPLES_PER_UNIT; i++) {
        int raw;
        if (eight_bit) {
            raw = (int8_t) group[GROUP_SAMPLES + unit + i * 4];
        } else {
            uint8_t b = group[GROUP_SAMPLES + unit / 2 + i * 4];
            raw = (int8_t) ((unit & 1 ? b & 0xF0 : b << 4)) >> 4;
        }
        int s = (shift >= 0 ? raw << shift : raw >> -shift) + (old * f0 + older * f1 + 32) / 64;
        out[i] = clamp16(s);
        older = old;
        old = out[i];
    }
    s_old[channel] = old;
    s_older[channel] = older;
}

static void queue_frame(int16_t left, int16_t right)
{
    if (s_count >= QUEUE_FRAMES) {
        return;                         /* nobody listens: drop it */
    }
    int at = (s_read + s_count) % QUEUE_FRAMES;
    s_queue[at][0] = left;
    s_queue[at][1] = right;
    s_count++;
}

void xa_decode_sector(const uint8_t *sector)
{
    uint8_t coding = sector[SUBHEADER_OFFSET + SUBHEADER_CODING];
    int stereo = (coding & CODING_STEREO) != 0;
    int eight_bit = (coding & CODING_8BIT) != 0;
    int units = eight_bit ? UNITS_8BIT : UNITS_4BIT;
    int16_t samples[SAMPLES_PER_UNIT], right[SAMPLES_PER_UNIT];

    s_rate = coding & CODING_HALF_RATE ? RATE_HALF : RATE_FULL;
    for (int g = 0; g < GROUPS; g++) {
        const uint8_t *group = sector + DATA_OFFSET + g * GROUP_SIZE;
        if (stereo) {
            for (int u = 0; u < units; u += 2) {
                decode_unit(group, u, eight_bit, 0, samples);
                decode_unit(group, u + 1, eight_bit, 1, right);
                for (int i = 0; i < SAMPLES_PER_UNIT; i++) {
                    queue_frame(samples[i], right[i]);
                }
            }
        } else {
            for (int u = 0; u < units; u++) {
                decode_unit(group, u, eight_bit, 0, samples);
                for (int i = 0; i < SAMPLES_PER_UNIT; i++) {
                    queue_frame(samples[i], samples[i]);
                }
            }
        }
    }
}

void xa_mix(int16_t *out, int frames, int volume_left, int volume_right)
{
    if (!s_kernel_ready) {
        build_kernel();
    }
    uint32_t step = (uint32_t) (((uint64_t) s_rate << 16) / OUTPUT_RATE);
    for (int i = 0; i < frames && s_count >= LANCZOS_TAPS; i++) {
        const int16_t *kernel = s_kernel[(s_phase & 0xFFFF) * LANCZOS_PHASES >> 16];
        for (int c = 0; c < 2; c++) {
            int32_t sum = 0;
            for (int k = 0; k < LANCZOS_TAPS; k++) {
                sum += s_queue[(s_read + k) % QUEUE_FRAMES][c] * kernel[k];
            }
            int s = sum / KERNEL_ONE;
            int v = (s * (c ? volume_right : volume_left)) >> 8;
            out[i * 2 + c] = clamp16(out[i * 2 + c] + v);
        }
        s_phase += step;
        while (s_phase >= 0x10000 && s_count >= LANCZOS_TAPS) {
            s_phase -= 0x10000;
            s_read = (s_read + 1) % QUEUE_FRAMES;
            s_count--;
        }
    }
}
