/*
 * spu.c - the PS1 sound processor in software (see spu.h).
 *
 * After the "SPU" chapter of psx-spx (nocash): the registers, 16-byte
 * ADPCM blocks of 28 samples with the five prediction filters and loop
 * flags, the ADSR envelope with its linear/exponential steps, the pitch
 * counter in 4.12 (1000h = 44.1 kHz) with pitch modulation, the noise
 * generator and the reverb at 22.05 kHz with its 32 registers.
 * Differences: samples are interpolated with a cubic B-spline (the
 * hardware's Gaussian table is not reproduced), the reverb's input and
 * output are not FIR-resampled, volume sweeps keep their current level,
 * and there is no SPU interrupt.  (The mixing core comes from the
 * American Pool PC port.)
 */
#include <string.h>
#include "psx/spu.h"

uint8_t g_spu_ram[SPU_RAM_SIZE];

#define BLOCK_SIZE              16
#define BLOCK_SAMPLES           28
#define PITCH_MAX               0x4000
#define LEVEL_MAX               0x7FFF

/* ADPCM block flags */
#define FLAG_LOOP_END           1
#define FLAG_LOOP_REPEAT        2
#define FLAG_LOOP_START         4

/* register offsets (from 1F801C00h) */
#define REG_VOICE_END           0x180
#define REG_MAIN_VOL_L          0x180
#define REG_MAIN_VOL_R          0x182
#define REG_REVERB_VOL_L        0x184
#define REG_REVERB_VOL_R        0x186
#define REG_KEY_ON              0x188
#define REG_KEY_OFF             0x18C
#define REG_PITCH_MOD           0x190
#define REG_NOISE_ON            0x194
#define REG_REVERB_ON           0x198
#define REG_ENDX                0x19C
#define REG_REVERB_BASE         0x1A2
#define REG_TRANSFER_ADDR       0x1A6
#define REG_TRANSFER_FIFO       0x1A8
#define REG_CONTROL             0x1AA
#define REG_STATUS              0x1AE
#define REG_REVERB_FIRST        0x1C0
#define REG_VOICE_VOLUME        0x200   /* 1E00h: current volumes (read) */

/* SPUCNT */
#define CNT_ENABLE              0x8000
#define CNT_UNMUTE              0x4000
#define CNT_REVERB              0x0080
#define CNT_MODE_MASK           0x003F

/* voice registers */
#define V_VOL_L                 0x0
#define V_VOL_R                 0x2
#define V_PITCH                 0x4
#define V_START                 0x6
#define V_ADSR1                 0x8
#define V_ADSR2                 0xA
#define V_LEVEL                 0xC
#define V_REPEAT                0xE
#define VOLUME_SWEEP            0x8000

enum Phase { PHASE_OFF, PHASE_ATTACK, PHASE_DECAY, PHASE_SUSTAIN, PHASE_RELEASE };

typedef struct {
    uint32_t start, repeat, current;    /* byte addresses */
    int repeat_set;                     /* repeat written: loop-start flags ignored */
    int16_t block[BLOCK_SAMPLES];
    int block_pos;
    int block_flags;
    int32_t hist1, hist2;               /* ADPCM prediction */
    int16_t last[4];                    /* the last samples, for interpolation */
    uint32_t counter;                   /* pitch counter, 12 fraction bits */
    int pitch;
    int volume[2];                      /* -8000h..7FFEh */
    uint16_t adsr1, adsr2;
    int phase;
    int32_t level;                      /* envelope 0..7FFFh */
    int32_t wait;                       /* samples until the next envelope step */
    int32_t output;                     /* last sample after the envelope (pitch modulation) */
} Voice;

static Voice s_voices[SPU_VOICES];
static uint16_t s_regs[0x200];          /* what was written (1F801C00h..1F801FFFh) */
static uint32_t s_endx;
static uint32_t s_transfer;             /* transfer address (bytes) */
static int s_main_volume[2];
static int s_reverb_volume[2];
static int16_t s_reverb[32];            /* dAPF1 .. vRIN as signed values */
static uint32_t s_reverb_base, s_reverb_current;
static int32_t s_reverb_in[2], s_reverb_out[2];
static int s_reverb_phase;
static int32_t s_noise_timer;
static uint16_t s_noise_level;

/* reverb register indices */
enum {
    R_DAPF1, R_DAPF2, R_VIIR, R_VCOMB1, R_VCOMB2, R_VCOMB3, R_VCOMB4, R_VWALL,
    R_VAPF1, R_VAPF2, R_MLSAME, R_MRSAME, R_MLCOMB1, R_MRCOMB1, R_MLCOMB2, R_MRCOMB2,
    R_DLSAME, R_DRSAME, R_MLDIFF, R_MRDIFF, R_MLCOMB3, R_MRCOMB3, R_MLCOMB4, R_MRCOMB4,
    R_DLDIFF, R_DRDIFF, R_MLAPF1, R_MRAPF1, R_MLAPF2, R_MRAPF2, R_VLIN, R_VRIN
};

static const int FILTER_POS[5] = { 0, 60, 115, 98, 122 };
static const int FILTER_NEG[5] = { 0, 0, -52, -55, -60 };

void spu_reset(void)
{
    memset(g_spu_ram, 0, sizeof(g_spu_ram));
    memset(s_voices, 0, sizeof(s_voices));
    memset(s_regs, 0, sizeof(s_regs));
    memset(s_reverb, 0, sizeof(s_reverb));
    s_endx = 0;
    s_transfer = 0;
    s_main_volume[0] = s_main_volume[1] = 0;
    s_reverb_volume[0] = s_reverb_volume[1] = 0;
    s_reverb_base = s_reverb_current = SPU_RAM_SIZE - 2;
    s_reverb_in[0] = s_reverb_in[1] = s_reverb_out[0] = s_reverb_out[1] = 0;
    s_reverb_phase = 0;
    s_noise_timer = 0;
    s_noise_level = 1;
}

static int16_t clamp16(int32_t v)
{
    return (int16_t) (v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}

/* a volume register: fixed (bits 0..14 = volume / 2) or a sweep (kept as it is) */
static int volume_value(uint16_t reg, int current)
{
    if (reg & VOLUME_SWEEP) {
        return current;
    }
    return (int16_t) (reg << 1);
}

/* ---- voices ----------------------------------------------------------- */

static void decode_block(Voice *v, int index)
{
    const uint8_t *b = &g_spu_ram[v->current & (SPU_RAM_SIZE - BLOCK_SIZE)];
    int shift = b[0] & 0xF, filter = (b[0] >> 4) & 7;
    if (shift > 12) {
        shift = 9;
    }
    if (filter > 4) {
        filter = 4;
    }
    v->block_flags = b[1];
    if ((v->block_flags & FLAG_LOOP_START) && !v->repeat_set) {
        v->repeat = v->current;
    }
    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        int nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t s = (int32_t) (int16_t) (nibble << 12) >> shift;
        s += (v->hist1 * FILTER_POS[filter] + v->hist2 * FILTER_NEG[filter] + 32) >> 6;
        s = clamp16(s);
        v->hist2 = v->hist1;
        v->hist1 = s;
        v->block[i] = (int16_t) s;
    }
    v->block_pos = 0;
    (void) index;
}

static void key_on(uint32_t voices)
{
    for (int i = 0; i < SPU_VOICES; i++) {
        if (voices & (1u << i)) {
            Voice *v = &s_voices[i];
            v->current = v->start;
            v->repeat_set = 0;
            v->hist1 = v->hist2 = 0;
            memset(v->last, 0, sizeof(v->last));
            v->counter = 0;
            v->phase = PHASE_ATTACK;
            v->level = 0;
            v->wait = 0;
            s_endx &= ~(1u << i);
            decode_block(v, i);
        }
    }
}

static void key_off(uint32_t voices)
{
    for (int i = 0; i < SPU_VOICES; i++) {
        if ((voices & (1u << i)) && s_voices[i].phase != PHASE_OFF) {
            s_voices[i].phase = PHASE_RELEASE;
            s_voices[i].wait = 0;
        }
    }
}

/* ---- registers -------------------------------------------------------- */

static void set_low_high(uint32_t *bits, uint32_t offset, uint16_t value)
{
    if (offset & 2) {
        *bits = (*bits & 0xFFFFu) | (uint32_t) (value & 0xFF) << 16;
    } else {
        *bits = (*bits & 0xFF0000u) | value;
    }
}

int g_spu_trace;                        /* (debugging: log the global registers) */
void psx_log(const char *format, ...);

void spu_write16(uint32_t offset, uint16_t value)
{
    offset &= 0x3FE;
    if (g_spu_trace && offset >= REG_VOICE_END && offset != REG_TRANSFER_FIFO
        && offset != REG_KEY_ON && offset != REG_KEY_ON + 2 && offset != REG_KEY_OFF
        && offset != REG_KEY_OFF + 2 && s_regs[offset / 2] != value) {
        psx_log("SPU %03X = %04X", 0xC00 + offset, value);
    }
    s_regs[offset / 2] = value;
    if (offset < REG_VOICE_END) {
        Voice *v = &s_voices[offset >> 4];
        switch (offset & 0xF) {
        case V_VOL_L:
            v->volume[0] = volume_value(value, v->volume[0]);
            break;
        case V_VOL_R:
            v->volume[1] = volume_value(value, v->volume[1]);
            break;
        case V_PITCH:
            v->pitch = value;
            break;
        case V_START:
            v->start = (uint32_t) value * 8 & (SPU_RAM_SIZE - 1);
            break;
        case V_ADSR1:
            v->adsr1 = value;
            break;
        case V_ADSR2:
            v->adsr2 = value;
            break;
        case V_LEVEL:
            v->level = (int16_t) value < 0 ? 0 : (int16_t) value;
            break;
        case V_REPEAT:
            v->repeat = (uint32_t) value * 8 & (SPU_RAM_SIZE - 1);
            v->repeat_set = 1;
            break;
        }
        return;
    }
    static uint32_t pending;            /* (key on/off: both halves are written) */
    switch (offset) {
    case REG_MAIN_VOL_L:
        s_main_volume[0] = volume_value(value, s_main_volume[0]);
        break;
    case REG_MAIN_VOL_R:
        s_main_volume[1] = volume_value(value, s_main_volume[1]);
        break;
    case REG_REVERB_VOL_L:
        s_reverb_volume[0] = (int16_t) value;
        break;
    case REG_REVERB_VOL_R:
        s_reverb_volume[1] = (int16_t) value;
        break;
    case REG_KEY_ON:
    case REG_KEY_ON + 2:
        pending = 0;
        set_low_high(&pending, offset, value);
        key_on(pending);
        break;
    case REG_KEY_OFF:
    case REG_KEY_OFF + 2:
        pending = 0;
        set_low_high(&pending, offset, value);
        key_off(pending);
        break;
    case REG_REVERB_BASE:
        s_reverb_base = (uint32_t) value * 8 & (SPU_RAM_SIZE - 2);
        s_reverb_current = s_reverb_base;
        break;
    case REG_TRANSFER_ADDR:
        s_transfer = (uint32_t) value * 8 & (SPU_RAM_SIZE - 1);
        break;
    case REG_TRANSFER_FIFO:
        g_spu_ram[s_transfer] = (uint8_t) value;
        g_spu_ram[s_transfer + 1] = (uint8_t) (value >> 8);
        s_transfer = (s_transfer + 2) & (SPU_RAM_SIZE - 1);
        break;
    default:
        if (offset >= REG_REVERB_FIRST && offset < REG_REVERB_FIRST + 64) {
            s_reverb[(offset - REG_REVERB_FIRST) / 2] = (int16_t) value;
        }
        break;
    }
}

uint16_t spu_read16(uint32_t offset)
{
    offset &= 0x3FE;
    if (offset < REG_VOICE_END && (offset & 0xF) == V_LEVEL) {
        return (uint16_t) s_voices[offset >> 4].level;
    }
    switch (offset) {
    case REG_ENDX:
        return (uint16_t) s_endx;
    case REG_ENDX + 2:
        return (uint16_t) (s_endx >> 16);
    case REG_STATUS:
        /* the mode bits follow SPUCNT at once; never busy */
        return s_regs[REG_CONTROL / 2] & CNT_MODE_MASK;
    case REG_TRANSFER_ADDR:
        return s_regs[offset / 2];
    }
    if (offset >= REG_VOICE_VOLUME && offset < REG_VOICE_VOLUME + SPU_VOICES * 4) {
        const Voice *v = &s_voices[(offset - REG_VOICE_VOLUME) / 4];
        return (uint16_t) (v->volume[(offset / 2) & 1] / 2);
    }
    return s_regs[offset / 2];
}

void spu_dma_write(const void *data, uint32_t bytes)
{
    const uint8_t *p = (const uint8_t *) data;
    for (uint32_t i = 0; i < bytes; i++) {
        g_spu_ram[s_transfer] = p[i];
        s_transfer = (s_transfer + 1) & (SPU_RAM_SIZE - 1);
    }
}

void spu_dma_read(void *data, uint32_t bytes)
{
    uint8_t *p = (uint8_t *) data;
    for (uint32_t i = 0; i < bytes; i++) {
        p[i] = g_spu_ram[s_transfer];
        s_transfer = (s_transfer + 1) & (SPU_RAM_SIZE - 1);
    }
}

/* ---- envelope --------------------------------------------------------- */

static void envelope_step(Voice *v)
{
    int exponential, decreasing, shift, step;
    switch (v->phase) {
    case PHASE_ATTACK:
        exponential = v->adsr1 >> 15;
        decreasing = 0;
        shift = (v->adsr1 >> 10) & 0x1F;
        step = 7 - ((v->adsr1 >> 8) & 3);
        break;
    case PHASE_DECAY:
        exponential = 1;
        decreasing = 1;
        shift = ((v->adsr1 >> 4) & 0xF) << 2;
        step = -8;
        break;
    case PHASE_SUSTAIN:
        exponential = v->adsr2 >> 15;
        decreasing = (v->adsr2 >> 14) & 1;
        shift = (v->adsr2 >> 8) & 0x1F;
        step = decreasing ? -8 + ((v->adsr2 >> 6) & 3) : 7 - ((v->adsr2 >> 6) & 3);
        break;
    case PHASE_RELEASE:
        exponential = (v->adsr2 >> 5) & 1;
        decreasing = 1;
        shift = v->adsr2 & 0x1F;
        step = -8;
        break;
    default:
        return;
    }
    if (--v->wait > 0) {
        return;
    }
    int32_t cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int32_t delta = step << (shift < 11 ? 11 - shift : 0);
    if (exponential && !decreasing && v->level > 0x6000) {
        cycles *= 4;
    }
    if (exponential && decreasing) {
        delta = (delta * v->level) >> 15;
    }
    v->wait = cycles;
    v->level += delta;
    if (v->level > LEVEL_MAX) {
        v->level = LEVEL_MAX;
    }
    if (v->level < 0) {
        v->level = 0;
    }

    int32_t sustain = ((v->adsr1 & 0xF) + 1) * 0x800;
    switch (v->phase) {
    case PHASE_ATTACK:
        if (v->level >= LEVEL_MAX) {
            v->phase = PHASE_DECAY;
            v->wait = 0;
        }
        break;
    case PHASE_DECAY:
        if (v->level <= (sustain > LEVEL_MAX ? LEVEL_MAX : sustain)) {
            v->phase = PHASE_SUSTAIN;
            v->wait = 0;
        }
        break;
    case PHASE_RELEASE:
        if (v->level <= 0) {
            v->phase = PHASE_OFF;
        }
        break;
    }
}

/* ---- samples ---------------------------------------------------------- */

/* the next sample of a voice's stream (blocks, loops) */
static int16_t next_sample(Voice *v, int index)
{
    if (v->block_pos >= BLOCK_SAMPLES) {
        if (v->block_flags & FLAG_LOOP_END) {
            s_endx |= 1u << index;
            v->current = v->repeat;
            if (!(v->block_flags & FLAG_LOOP_REPEAT)) {
                v->phase = PHASE_OFF;           /* (release with level 0) */
                v->level = 0;
            }
        } else {
            v->current = (v->current + BLOCK_SIZE) & (SPU_RAM_SIZE - 1);
        }
        decode_block(v, index);
    }
    return v->block[v->block_pos++];
}

/* cubic B-spline through the last four samples, t in 0..FFFh */
static int32_t interpolate(const int16_t s[4], uint32_t t)
{
    float x = t / 4096.0f, x2 = x * x, x3 = x2 * x;
    float w0 = (1 - x) * (1 - x) * (1 - x) / 6;
    float w1 = (3 * x3 - 6 * x2 + 4) / 6;
    float w2 = (-3 * x3 + 3 * x2 + 3 * x + 1) / 6;
    float w3 = x3 / 6;
    return (int32_t) (s[0] * w0 + s[1] * w1 + s[2] * w2 + s[3] * w3);
}

/* the noise generator, once per sample */
static void noise_step(void)
{
    uint16_t cnt = s_regs[REG_CONTROL / 2];
    int step = ((cnt >> 8) & 3) + 4, shift = (cnt >> 10) & 0xF;
    s_noise_timer -= step;
    int parity = ((s_noise_level >> 15) ^ (s_noise_level >> 12) ^ (s_noise_level >> 11)
                  ^ (s_noise_level >> 10) ^ 1) & 1;
    if (s_noise_timer < 0) {
        s_noise_level = (uint16_t) (s_noise_level * 2 + parity);
        s_noise_timer += 0x20000 >> shift;
        if (s_noise_timer < 0) {
            s_noise_timer += 0x20000 >> shift;
        }
    }
}

/* ---- reverb ----------------------------------------------------------- */

static uint32_t reverb_address(int32_t offset_bytes)
{
    uint32_t size = SPU_RAM_SIZE - s_reverb_base;
    int32_t a = (int32_t) (s_reverb_current - s_reverb_base) + offset_bytes;
    a %= (int32_t) size;
    if (a < 0) {
        a += (int32_t) size;
    }
    return s_reverb_base + (uint32_t) a;
}

static int32_t reverb_read(int reg, int extra_bytes)
{
    uint32_t a = reverb_address((uint16_t) s_reverb[reg] * 8 + extra_bytes) & ~1u;
    return (int16_t) (g_spu_ram[a] | g_spu_ram[a + 1] << 8);
}

static void reverb_write(int reg, int32_t value)
{
    uint32_t a = reverb_address((uint16_t) s_reverb[reg] * 8) & ~1u;
    int16_t v = clamp16(value);
    g_spu_ram[a] = (uint8_t) v;
    g_spu_ram[a + 1] = (uint8_t) (v >> 8);
}

static int32_t mul(int32_t a, int reg)
{
    return (a * s_reverb[reg]) >> 15;
}

/* one 22.05 kHz step of the reverb */
static void reverb_step(int32_t in_left, int32_t in_right)
{
    int32_t lin = mul(in_left, R_VLIN), rin = mul(in_right, R_VRIN);
    int32_t prev;

    prev = reverb_read(R_MLSAME, -2);
    reverb_write(R_MLSAME, mul(lin + mul(reverb_read(R_DLSAME, 0), R_VWALL) - prev, R_VIIR) + prev);
    prev = reverb_read(R_MRSAME, -2);
    reverb_write(R_MRSAME, mul(rin + mul(reverb_read(R_DRSAME, 0), R_VWALL) - prev, R_VIIR) + prev);
    prev = reverb_read(R_MLDIFF, -2);
    reverb_write(R_MLDIFF, mul(lin + mul(reverb_read(R_DRDIFF, 0), R_VWALL) - prev, R_VIIR) + prev);
    prev = reverb_read(R_MRDIFF, -2);
    reverb_write(R_MRDIFF, mul(rin + mul(reverb_read(R_DLDIFF, 0), R_VWALL) - prev, R_VIIR) + prev);

    int32_t l = mul(reverb_read(R_MLCOMB1, 0), R_VCOMB1) + mul(reverb_read(R_MLCOMB2, 0), R_VCOMB2)
                + mul(reverb_read(R_MLCOMB3, 0), R_VCOMB3) + mul(reverb_read(R_MLCOMB4, 0), R_VCOMB4);
    int32_t r = mul(reverb_read(R_MRCOMB1, 0), R_VCOMB1) + mul(reverb_read(R_MRCOMB2, 0), R_VCOMB2)
                + mul(reverb_read(R_MRCOMB3, 0), R_VCOMB3) + mul(reverb_read(R_MRCOMB4, 0), R_VCOMB4);

    int32_t d1 = -(uint16_t) s_reverb[R_DAPF1] * 8, d2 = -(uint16_t) s_reverb[R_DAPF2] * 8;
    int32_t t;
    t = reverb_read(R_MLAPF1, d1);
    l -= mul(t, R_VAPF1);
    reverb_write(R_MLAPF1, l);
    l = mul(l, R_VAPF1) + t;
    t = reverb_read(R_MRAPF1, d1);
    r -= mul(t, R_VAPF1);
    reverb_write(R_MRAPF1, r);
    r = mul(r, R_VAPF1) + t;
    t = reverb_read(R_MLAPF2, d2);
    l -= mul(t, R_VAPF2);
    reverb_write(R_MLAPF2, l);
    l = mul(l, R_VAPF2) + t;
    t = reverb_read(R_MRAPF2, d2);
    r -= mul(t, R_VAPF2);
    reverb_write(R_MRAPF2, r);
    r = mul(r, R_VAPF2) + t;

    s_reverb_out[0] = clamp16((l * s_reverb_volume[0]) >> 15);
    s_reverb_out[1] = clamp16((r * s_reverb_volume[1]) >> 15);
    s_reverb_current = reverb_address(2);
}

/* ---- mixing ----------------------------------------------------------- */

void spu_render(int16_t *out, int frames)
{
    uint16_t cnt = s_regs[REG_CONTROL / 2];
    uint32_t pmon = s_regs[REG_PITCH_MOD / 2] | (uint32_t) s_regs[REG_PITCH_MOD / 2 + 1] << 16;
    uint32_t non = s_regs[REG_NOISE_ON / 2] | (uint32_t) s_regs[REG_NOISE_ON / 2 + 1] << 16;
    uint32_t eon = s_regs[REG_REVERB_ON / 2] | (uint32_t) s_regs[REG_REVERB_ON / 2 + 1] << 16;
    int on = (cnt & CNT_ENABLE) && (cnt & CNT_UNMUTE);

    for (int f = 0; f < frames; f++) {
        int32_t mix[2] = { 0, 0 }, rev[2] = { 0, 0 };
        noise_step();
        for (int i = 0; i < SPU_VOICES; i++) {
            Voice *v = &s_voices[i];
            if (v->phase == PHASE_OFF) {
                v->output = 0;
                continue;
            }
            int32_t s = (non >> i) & 1 ? (int16_t) s_noise_level
                                       : interpolate(v->last, v->counter & 0xFFF);
            envelope_step(v);
            s = (s * v->level) >> 15;
            v->output = s;
            for (int c = 0; c < 2; c++) {
                int32_t o = (s * v->volume[c]) >> 15;
                mix[c] += o;
                if ((eon >> i) & 1) {
                    rev[c] += o;
                }
            }
            int32_t step = v->pitch;
            if (i > 0 && ((pmon >> i) & 1)) {
                int32_t factor = s_voices[i - 1].output + 0x8000;
                step = (((int32_t) (int16_t) step * factor) >> 15) & 0xFFFF;
            }
            v->counter += (uint32_t) (step > PITCH_MAX ? PITCH_MAX : step);
            while (v->counter >= 0x1000 && v->phase != PHASE_OFF) {
                v->counter -= 0x1000;
                v->last[0] = v->last[1];
                v->last[1] = v->last[2];
                v->last[2] = v->last[3];
                v->last[3] = next_sample(v, i);
            }
        }
        /* the reverb runs at half the rate */
        s_reverb_in[0] += rev[0];
        s_reverb_in[1] += rev[1];
        if (++s_reverb_phase == 2) {
            s_reverb_phase = 0;
            if (cnt & CNT_REVERB) {
                reverb_step(clamp16(s_reverb_in[0] / 2), clamp16(s_reverb_in[1] / 2));
            } else {
                s_reverb_out[0] = s_reverb_out[1] = 0;
            }
            s_reverb_in[0] = s_reverb_in[1] = 0;
        }
        for (int c = 0; c < 2; c++) {
            int32_t total = clamp16(mix[c]) + s_reverb_out[c];
            out[f * 2 + c] = on ? clamp16((total * s_main_volume[c]) >> 15) : 0;
        }
    }
}
