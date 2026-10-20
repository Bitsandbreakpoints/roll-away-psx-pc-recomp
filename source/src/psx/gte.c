/*
 * gte.c - the PS1 Geometry Transformation Engine in C (see gte.h).
 *
 * After the "GTE" chapter of psx-spx: MAC1..3 are 44-bit sums checked for
 * overflow at every step, MAC0 32 bits; IR1..3 are saturated to 16 bits
 * (0..7FFFh with lm=1), the colour FIFO to 8 bits, SZ/OTZ to 16 bits,
 * SX/SY to 11 bits, IR0 to 0..1000h; the perspective division uses the
 * unsigned Newton-Raphson table.
 *
 * For precise vertices (pgxp.h) the screen position FIFO is also kept as
 * floats, computed from the unrounded view-space position.
 */
#include <string.h>
#include "psx/gte.h"
#include "psx/pgxp.h"

/* FLAG bits */
#define FLAG_MAC1_POS           (1u << 30)      /* +3 per MAC: MAC2 29, MAC3 28 */
#define FLAG_MAC1_NEG           (1u << 27)      /* MAC2 26, MAC3 25 */
#define FLAG_IR1                (1u << 24)      /* IR2 23, IR3 22 */
#define FLAG_COLOR_R            (1u << 21)      /* G 20, B 19 */
#define FLAG_SZ3_OTZ            (1u << 18)
#define FLAG_DIVIDE             (1u << 17)
#define FLAG_MAC0_POS           (1u << 16)
#define FLAG_MAC0_NEG           (1u << 15)
#define FLAG_SX2                (1u << 14)
#define FLAG_SY2                (1u << 13)
#define FLAG_IR0                (1u << 12)

#define MAC44_MAX               0x7FFFFFFFFFFLL
#define MAC44_MIN               (-0x80000000000LL)

typedef struct {
    /* data registers */
    int16_t v[3][3];                /* V0..V2: x, y, z */
    uint8_t rgbc[4];
    uint16_t otz;
    int16_t ir[4];                  /* IR0..IR3 */
    int16_t sxy[3][2];              /* SXY0..SXY2: x, y */
    uint16_t sz[4];                 /* SZ0..SZ3 */
    uint8_t rgb[3][4];              /* colour FIFO RGB0..RGB2 */
    uint32_t res1;
    int32_t mac[4];                 /* MAC0..MAC3 */
    uint32_t lzcs, lzcr;
    /* control registers */
    int16_t rt[3][3];
    int32_t tr[3];
    int16_t llm[3][3];
    int32_t bk[3];
    int16_t lcm[3][3];
    int32_t fc[3];
    int32_t ofx, ofy;
    uint16_t h;
    int16_t dqa;
    int32_t dqb;
    int16_t zsf3, zsf4;
    uint32_t flag;
} Gte;

static Gte g;

/* SXY0..SXY2 as floats (precise vertices); valid only after RTPS/RTPT */
static PgxpVertex s_precise[3];
static int s_precise_valid[3];

static void push_precise(const PgxpVertex *v, int valid)
{
    memmove(&s_precise[0], &s_precise[1], sizeof(s_precise[0]) * 2);
    memmove(&s_precise_valid[0], &s_precise_valid[1], sizeof(s_precise_valid[0]) * 2);
    if (v) {
        s_precise[2] = *v;
    }
    s_precise_valid[2] = valid;
}
static uint8_t s_unr_table[0x101];

void gte_reset(void)
{
    memset(&g, 0, sizeof(g));
    for (int i = 0; i < 0x101; i++) {
        int u = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        s_unr_table[i] = (uint8_t) (u < 0 ? 0 : u);
    }
}

/* ---- registers -------------------------------------------------------- */

static uint32_t pair(int16_t lo, int16_t hi)
{
    return (uint16_t) lo | (uint32_t) (uint16_t) hi << 16;
}

static int16_t lo16(uint32_t v)
{
    return (int16_t) v;
}

static int16_t hi16(uint32_t v)
{
    return (int16_t) (v >> 16);
}

static uint32_t color_component(int16_t ir)
{
    int c = ir >> 7;
    return (uint32_t) (c < 0 ? 0 : c > 0x1F ? 0x1F : c);
}

uint32_t gte_mfc2(int reg)
{
    switch (reg) {
    case GTE_VXY0: case GTE_VXY1: case GTE_VXY2:
        return pair(g.v[reg / 2][0], g.v[reg / 2][1]);
    case GTE_VZ0: case GTE_VZ1: case GTE_VZ2:
        return (uint32_t) (int32_t) g.v[reg / 2][2];
    case GTE_RGBC:
        return g.rgbc[0] | g.rgbc[1] << 8 | g.rgbc[2] << 16 | (uint32_t) g.rgbc[3] << 24;
    case GTE_OTZ:
        return g.otz;
    case GTE_IR0: case GTE_IR1: case GTE_IR2: case GTE_IR3:
        return (uint32_t) (int32_t) g.ir[reg - GTE_IR0];
    case GTE_SXY0: case GTE_SXY1: case GTE_SXY2:
        return pair(g.sxy[reg - GTE_SXY0][0], g.sxy[reg - GTE_SXY0][1]);
    case GTE_SXYP:
        return pair(g.sxy[2][0], g.sxy[2][1]);
    case GTE_SZ0: case GTE_SZ1: case GTE_SZ2: case GTE_SZ3:
        return g.sz[reg - GTE_SZ0];
    case GTE_RGB0: case GTE_RGB1: case GTE_RGB2: {
        const uint8_t *c = g.rgb[reg - GTE_RGB0];
        return c[0] | c[1] << 8 | c[2] << 16 | (uint32_t) c[3] << 24;
    }
    case GTE_RES1:
        return g.res1;
    case GTE_MAC0: case GTE_MAC1: case GTE_MAC2: case GTE_MAC3:
        return (uint32_t) g.mac[reg - GTE_MAC0];
    case GTE_IRGB: case GTE_ORGB:
        return color_component(g.ir[1]) | color_component(g.ir[2]) << 5
               | color_component(g.ir[3]) << 10;
    case GTE_LZCS:
        return g.lzcs;
    default:
        return g.lzcr;
    }
}

/* leading bits equal to the sign bit */
static uint32_t count_leading(uint32_t v)
{
    uint32_t sign = v >> 31, n = 0;
    while (n < 32 && ((v >> (31 - n)) & 1) == sign) {
        n++;
    }
    return n;
}

void gte_mtc2(int reg, uint32_t value)
{
    switch (reg) {
    case GTE_VXY0: case GTE_VXY1: case GTE_VXY2:
        g.v[reg / 2][0] = lo16(value);
        g.v[reg / 2][1] = hi16(value);
        break;
    case GTE_VZ0: case GTE_VZ1: case GTE_VZ2:
        g.v[reg / 2][2] = lo16(value);
        break;
    case GTE_RGBC:
        for (int i = 0; i < 4; i++) {
            g.rgbc[i] = (uint8_t) (value >> (i * 8));
        }
        break;
    case GTE_OTZ:
        g.otz = (uint16_t) value;
        break;
    case GTE_IR0: case GTE_IR1: case GTE_IR2: case GTE_IR3:
        g.ir[reg - GTE_IR0] = lo16(value);
        break;
    case GTE_SXY0: case GTE_SXY1: case GTE_SXY2:
        g.sxy[reg - GTE_SXY0][0] = lo16(value);
        g.sxy[reg - GTE_SXY0][1] = hi16(value);
        s_precise_valid[reg - GTE_SXY0] = 0;
        break;
    case GTE_SXYP:
        memcpy(g.sxy[0], g.sxy[1], sizeof(g.sxy[0]) * 2);
        g.sxy[2][0] = lo16(value);
        g.sxy[2][1] = hi16(value);
        push_precise(NULL, 0);
        break;
    case GTE_SZ0: case GTE_SZ1: case GTE_SZ2: case GTE_SZ3:
        g.sz[reg - GTE_SZ0] = (uint16_t) value;
        break;
    case GTE_RGB0: case GTE_RGB1: case GTE_RGB2:
        for (int i = 0; i < 4; i++) {
            g.rgb[reg - GTE_RGB0][i] = (uint8_t) (value >> (i * 8));
        }
        break;
    case GTE_RES1:
        g.res1 = value;
        break;
    case GTE_MAC0: case GTE_MAC1: case GTE_MAC2: case GTE_MAC3:
        g.mac[reg - GTE_MAC0] = (int32_t) value;
        break;
    case GTE_IRGB:
        g.ir[1] = (int16_t) ((value & 0x1F) << 7);
        g.ir[2] = (int16_t) (((value >> 5) & 0x1F) << 7);
        g.ir[3] = (int16_t) (((value >> 10) & 0x1F) << 7);
        break;
    case GTE_LZCS:
        g.lzcs = value;
        g.lzcr = count_leading(value);
        break;
    default:
        break;                          /* ORGB, LZCR: read-only */
    }
}

static void matrix_from(int16_t m[3][3], int index, uint32_t value)
{
    int16_t *e = &m[0][0];
    e[index * 2] = lo16(value);
    if (index < 4) {
        e[index * 2 + 1] = hi16(value);
    }
}

static uint32_t matrix_to(int16_t m[3][3], int index)
{
    const int16_t *e = &m[0][0];
    return index < 4 ? pair(e[index * 2], e[index * 2 + 1]) : (uint32_t) (int32_t) e[8];
}

uint32_t gte_cfc2(int reg)
{
    if (reg <= GTE_RT33) {
        return matrix_to(g.rt, reg);
    }
    if (reg <= GTE_TRZ) {
        return (uint32_t) g.tr[reg - GTE_TRX];
    }
    if (reg <= GTE_L33) {
        return matrix_to(g.llm, reg - GTE_L11L12);
    }
    if (reg <= GTE_BBK) {
        return (uint32_t) g.bk[reg - GTE_RBK];
    }
    if (reg <= GTE_LB3) {
        return matrix_to(g.lcm, reg - GTE_LR1LR2);
    }
    if (reg <= GTE_BFC) {
        return (uint32_t) g.fc[reg - GTE_RFC];
    }
    switch (reg) {
    case GTE_OFX: return (uint32_t) g.ofx;
    case GTE_OFY: return (uint32_t) g.ofy;
    case GTE_H: return (uint32_t) (int32_t) (int16_t) g.h;     /* (read sign-extended) */
    case GTE_DQA: return (uint32_t) (int32_t) g.dqa;
    case GTE_DQB: return (uint32_t) g.dqb;
    case GTE_ZSF3: return (uint32_t) (int32_t) g.zsf3;
    case GTE_ZSF4: return (uint32_t) (int32_t) g.zsf4;
    default: return g.flag;
    }
}

void gte_ctc2(int reg, uint32_t value)
{
    if (reg <= GTE_RT33) {
        matrix_from(g.rt, reg, value);
    } else if (reg <= GTE_TRZ) {
        g.tr[reg - GTE_TRX] = (int32_t) value;
    } else if (reg <= GTE_L33) {
        matrix_from(g.llm, reg - GTE_L11L12, value);
    } else if (reg <= GTE_BBK) {
        g.bk[reg - GTE_RBK] = (int32_t) value;
    } else if (reg <= GTE_LB3) {
        matrix_from(g.lcm, reg - GTE_LR1LR2, value);
    } else if (reg <= GTE_BFC) {
        g.fc[reg - GTE_RFC] = (int32_t) value;
    } else {
        switch (reg) {
        case GTE_OFX: g.ofx = (int32_t) value; break;
        case GTE_OFY: g.ofy = (int32_t) value; break;
        case GTE_H: g.h = (uint16_t) value; break;
        case GTE_DQA: g.dqa = lo16(value); break;
        case GTE_DQB: g.dqb = (int32_t) value; break;
        case GTE_ZSF3: g.zsf3 = lo16(value); break;
        case GTE_ZSF4: g.zsf4 = lo16(value); break;
        default:
            g.flag = value & 0x7FFFF000u;
            if (g.flag & GTE_FLAG_ERROR) {
                g.flag |= GTE_FLAG_ERROR_BIT;
            }
            break;
        }
    }
}

void gte_lwc2(int reg, const void *address)
{
    uint32_t value;
    memcpy(&value, address, 4);
    gte_mtc2(reg, value);
}

void gte_swc2(int reg, void *address)
{
    uint32_t value = gte_mfc2(reg);
    memcpy(address, &value, 4);
    if (g_pgxp_enabled && reg >= GTE_SXY0 && reg <= GTE_SXYP) {
        int n = reg == GTE_SXYP ? 2 : reg - GTE_SXY0;
        if (s_precise_valid[n]) {
            pgxp_store(address, value, &s_precise[n]);
        }
    }
}

/* ---- arithmetic ------------------------------------------------------- */

/*
 * NCLIP: twice the signed area of the screen triangle SXY0..2, which the
 * game tests to drop polygons facing away.  With precise vertices the
 * float positions decide the sign (as PGXP culling does in emulators): a
 * thin sliver whose whole-pixel corners fall on one line has no area on
 * the console and is dropped, but drawn at a higher resolution it fills a
 * crack between its neighbours (e.g. a line across the cloth).
 */
static int64_t nclip_area(void)
{
    int64_t area = (int64_t) g.sxy[0][0] * g.sxy[1][1] + (int64_t) g.sxy[1][0] * g.sxy[2][1]
                   + (int64_t) g.sxy[2][0] * g.sxy[0][1] - (int64_t) g.sxy[0][0] * g.sxy[2][1]
                   - (int64_t) g.sxy[1][0] * g.sxy[0][1] - (int64_t) g.sxy[2][0] * g.sxy[1][1];
    if (!g_pgxp_enabled || !s_precise_valid[0] || !s_precise_valid[1] || !s_precise_valid[2]) {
        return area;
    }
    const PgxpVertex *v = s_precise;
    double precise = ((double) v[1].x - v[0].x) * ((double) v[2].y - v[0].y)
                     - ((double) v[2].x - v[0].x) * ((double) v[1].y - v[0].y);
    if (precise > 0 && area <= 0) {
        return 1;                       /* (only the sign changes) */
    }
    if (precise < 0 && area >= 0) {
        return -1;
    }
    return area;
}

/* a 44-bit MAC1..3 step: overflow flags, then wrapped to 44 bits */
static int64_t mac44(int i, int64_t value)
{
    if (value > MAC44_MAX) {
        g.flag |= FLAG_MAC1_POS >> (i - 1);
    } else if (value < MAC44_MIN) {
        g.flag |= FLAG_MAC1_NEG >> (i - 1);
    }
    return (int64_t) ((uint64_t) value << 20) >> 20;
}

static int32_t mac0(int64_t value)
{
    if (value > 0x7FFFFFFFLL) {
        g.flag |= FLAG_MAC0_POS;
    } else if (value < -0x80000000LL) {
        g.flag |= FLAG_MAC0_NEG;
    }
    return (int32_t) value;
}

static int16_t saturate_ir(int i, int64_t value, int lm)
{
    int64_t low = lm ? 0 : -0x8000;
    if (value < low) {
        g.flag |= FLAG_IR1 >> (i - 1);
        return (int16_t) low;
    }
    if (value > 0x7FFF) {
        g.flag |= FLAG_IR1 >> (i - 1);
        return 0x7FFF;
    }
    return (int16_t) value;
}

/* MAC1..3 = value >> shift, IR1..3 from them */
static void set_mac_ir(const int64_t value[3], int shift, int lm)
{
    for (int i = 1; i <= 3; i++) {
        g.mac[i] = (int32_t) (value[i - 1] >> shift);
        g.ir[i] = saturate_ir(i, g.mac[i], lm);
    }
}

static uint8_t saturate_color(int i, int32_t value)
{
    if (value < 0) {
        g.flag |= FLAG_COLOR_R >> i;
        return 0;
    }
    if (value > 0xFF) {
        g.flag |= FLAG_COLOR_R >> i;
        return 0xFF;
    }
    return (uint8_t) value;
}

static void push_color(void)
{
    memcpy(g.rgb[0], g.rgb[1], 8);
    for (int i = 0; i < 3; i++) {
        g.rgb[2][i] = saturate_color(i, g.mac[i + 1] >> 4);
    }
    g.rgb[2][3] = g.rgbc[3];
}

/* m * v + (t << 12), each step checked */
static int64_t matrix_row(int i, const int16_t row[3], const int32_t v[3], int64_t t)
{
    int64_t sum = mac44(i, t + (int64_t) row[0] * v[0]);
    sum = mac44(i, sum + (int64_t) row[1] * v[1]);
    return mac44(i, sum + (int64_t) row[2] * v[2]);
}

static int count_leading_zeros16(uint16_t value)
{
    int n = 0;
    for (uint16_t bit = 0x8000; bit && !(value & bit); bit >>= 1) {
        n++;
    }
    return n;
}

/* H / SZ3 as the hardware computes it (unsigned 1.16, at most 1FFFFh) */
static uint32_t divide(uint16_t h, uint16_t sz3)
{
    if (h >= sz3 * 2) {
        g.flag |= FLAG_DIVIDE;
        return 0x1FFFF;
    }
    int z = count_leading_zeros16(sz3);
    uint32_t n = (uint32_t) h << z;
    uint32_t d = (uint32_t) sz3 << z;
    uint32_t u = s_unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    uint64_t q = ((uint64_t) n * d + 0x8000) >> 16;
    return q > 0x1FFFF ? 0x1FFFF : (uint32_t) q;
}

static int16_t saturate_flag(int64_t value, int64_t low, int64_t high, uint32_t flag)
{
    if (value < low) {
        g.flag |= flag;
        return (int16_t) low;
    }
    if (value > high) {
        g.flag |= flag;
        return (int16_t) high;
    }
    return (int16_t) value;
}

/* RTPS on vertex `n`; depth cueing only for the last vertex */
static void rtp(int n, int sf, int lm, int last)
{
    int shift = sf * 12;
    int32_t v[3] = { g.v[n][0], g.v[n][1], g.v[n][2] };
    int64_t r[3];
    for (int i = 0; i < 3; i++) {
        r[i] = matrix_row(i + 1, g.rt[i], v, (int64_t) g.tr[i] << 12);
    }
    for (int i = 1; i <= 3; i++) {
        g.mac[i] = (int32_t) (r[i - 1] >> shift);
    }
    g.ir[1] = saturate_ir(1, g.mac[1], lm);
    g.ir[2] = saturate_ir(2, g.mac[2], lm);
    /* IR3: saturated from MAC3, but flagged from MAC3 >> 12 */
    {
        int32_t z = (int32_t) (r[2] >> 12);
        int32_t low = lm ? 0 : -0x8000;
        g.ir[3] = (int16_t) (g.mac[3] < low ? low : g.mac[3] > 0x7FFF ? 0x7FFF : g.mac[3]);
        if (z < -0x8000 || z > 0x7FFF) {
            g.flag |= FLAG_IR1 >> 2;
        }
        g.sz[0] = g.sz[1];
        g.sz[1] = g.sz[2];
        g.sz[2] = g.sz[3];
        g.sz[3] = (uint16_t) saturate_flag(z, 0, 0xFFFF, FLAG_SZ3_OTZ);
    }
    uint32_t q = divide(g.h, g.sz[3]);
    memcpy(g.sxy[0], g.sxy[1], sizeof(g.sxy[0]) * 2);
    int64_t x = (int64_t) q * g.ir[1] + g.ofx;
    int64_t y = (int64_t) q * g.ir[2] + g.ofy;
    mac0(x);
    mac0(y);
    g.sxy[2][0] = saturate_flag(x >> 16, -0x400, 0x3FF, FLAG_SX2);
    g.sxy[2][1] = saturate_flag(y >> 16, -0x400, 0x3FF, FLAG_SY2);
    {
        /* the same projection without rounding; trusted only where it
         * agrees with the hardware result (no saturation, no overflow) */
        PgxpVertex pv = { 0, 0, 0 };
        double vz = (double) r[2] / 4096.0;
        int valid = 0;
        if (vz >= g.h / 2.0 && vz > 0) {
            pv.x = (float) (g.ofx / 65536.0 + g.h * ((double) r[0] / 4096.0) / vz);
            pv.y = (float) (g.ofy / 65536.0 + g.h * ((double) r[1] / 4096.0) / vz);
            pv.z = (float) vz;
            valid = pv.x - g.sxy[2][0] > -1.5f && pv.x - g.sxy[2][0] < 2.5f
                    && pv.y - g.sxy[2][1] > -1.5f && pv.y - g.sxy[2][1] < 2.5f;
        }
        push_precise(&pv, valid);
    }
    if (last) {
        int64_t depth = (int64_t) q * g.dqa + g.dqb;
        g.mac[0] = mac0(depth);
        g.ir[0] = saturate_flag(depth >> 12, 0, 0x1000, FLAG_IR0);
    }
}

/* the depth cue interpolation towards the far colour, from u[] (<< 12 scale) */
static void interpolate(const int64_t u[3], int shift, int lm)
{
    int64_t r[3];
    for (int i = 1; i <= 3; i++) {
        int64_t d = mac44(i, ((int64_t) g.fc[i - 1] << 12) - u[i - 1]) >> shift;
        g.ir[i] = saturate_ir(i, d, 0);
        r[i - 1] = mac44(i, (int64_t) g.ir[i] * g.ir[0] + u[i - 1]);
    }
    set_mac_ir(r, shift, lm);
}

/* normal colour: light matrix, then colour matrix + background */
static void normal_light(int n, int shift, int lm)
{
    int32_t v[3] = { g.v[n][0], g.v[n][1], g.v[n][2] };
    int64_t r[3];
    for (int i = 0; i < 3; i++) {
        r[i] = matrix_row(i + 1, g.llm[i], v, 0);
    }
    set_mac_ir(r, shift, lm);
    int32_t ir[3] = { g.ir[1], g.ir[2], g.ir[3] };
    for (int i = 0; i < 3; i++) {
        r[i] = matrix_row(i + 1, g.lcm[i], ir, (int64_t) g.bk[i] << 12);
    }
    set_mac_ir(r, shift, lm);
}

/* stage after the colour matrix: 0 none (NCS), 1 colour (NCC), 2 colour + depth (NCD) */
static void color_stage(int mode, int shift, int lm)
{
    if (mode == 1) {
        int64_t r[3];
        for (int i = 1; i <= 3; i++) {
            r[i - 1] = mac44(i, ((int64_t) g.rgbc[i - 1] * g.ir[i]) << 4);
        }
        set_mac_ir(r, shift, lm);
    } else if (mode == 2) {
        int64_t u[3];
        for (int i = 1; i <= 3; i++) {
            u[i - 1] = mac44(i, ((int64_t) g.rgbc[i - 1] * g.ir[i]) << 4);
        }
        interpolate(u, shift, lm);
    }
    push_color();
}

static void mvmva(uint32_t cmd, int shift, int lm)
{
    int mx = (cmd >> 17) & 3, vn = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
    int16_t garbage[3][3] = { { 0 } };
    int16_t (*m)[3] = mx == 0 ? g.rt : mx == 1 ? g.llm : mx == 2 ? g.lcm : garbage;
    int32_t v[3];
    const int32_t *t = cv == 0 ? g.tr : cv == 1 ? g.bk : cv == 2 ? g.fc : NULL;
    int64_t r[3];

    if (mx == 3) {
        /* (the "garbage" matrix of the hardware) */
        garbage[0][0] = (int16_t) -(g.rgbc[0] << 4);
        garbage[0][1] = (int16_t) (g.rgbc[0] << 4);
        garbage[0][2] = g.ir[0];
        garbage[1][0] = garbage[1][1] = garbage[1][2] = g.rt[0][2];
        garbage[2][0] = garbage[2][1] = garbage[2][2] = g.rt[1][1];
    }
    if (vn == 3) {
        v[0] = g.ir[1];
        v[1] = g.ir[2];
        v[2] = g.ir[3];
    } else {
        v[0] = g.v[vn][0];
        v[1] = g.v[vn][1];
        v[2] = g.v[vn][2];
    }
    for (int i = 0; i < 3; i++) {
        if (cv == 2) {
            /* (FC is broken: its column only sets flags) */
            int64_t first = mac44(i + 1, ((int64_t) t[i] << 12) + (int64_t) m[i][0] * v[0]);
            saturate_ir(i + 1, first >> shift, 0);
            int64_t sum = mac44(i + 1, (int64_t) m[i][1] * v[1]);
            r[i] = mac44(i + 1, sum + (int64_t) m[i][2] * v[2]);
        } else {
            r[i] = matrix_row(i + 1, m[i], v, t ? (int64_t) t[i] << 12 : 0);
        }
    }
    set_mac_ir(r, shift, lm);
}

void gte_cop2(uint32_t command)
{
    int sf = (command >> 19) & 1;
    int shift = sf * 12;
    int lm = (command >> 10) & 1;
    int64_t r[3];

    g.flag = 0;
    switch (command & 0x3F) {
    case 0x01:                                  /* RTPS */
        rtp(0, sf, lm, 1);
        break;
    case 0x30:                                  /* RTPT */
        rtp(0, sf, lm, 0);
        rtp(1, sf, lm, 0);
        rtp(2, sf, lm, 1);
        break;
    case 0x06:                                  /* NCLIP */
        g.mac[0] = mac0(nclip_area());
        break;
    case 0x2D: {                                /* AVSZ3 */
        int64_t sum = (int64_t) g.zsf3 * (g.sz[1] + g.sz[2] + g.sz[3]);
        g.mac[0] = mac0(sum);
        g.otz = (uint16_t) saturate_flag(sum >> 12, 0, 0xFFFF, FLAG_SZ3_OTZ);
        break;
    }
    case 0x2E: {                                /* AVSZ4 */
        int64_t sum = (int64_t) g.zsf4 * (g.sz[0] + g.sz[1] + g.sz[2] + g.sz[3]);
        g.mac[0] = mac0(sum);
        g.otz = (uint16_t) saturate_flag(sum >> 12, 0, 0xFFFF, FLAG_SZ3_OTZ);
        break;
    }
    case 0x12:                                  /* MVMVA */
        mvmva(command, shift, lm);
        break;
    case 0x28:                                  /* SQR */
        for (int i = 1; i <= 3; i++) {
            r[i - 1] = mac44(i, (int64_t) g.ir[i] * g.ir[i]);
        }
        set_mac_ir(r, shift, lm);
        break;
    case 0x0C: {                                /* OP: cross product with the RT diagonal */
        int64_t d1 = g.rt[0][0], d2 = g.rt[1][1], d3 = g.rt[2][2];
        r[0] = mac44(1, g.ir[3] * d2 - g.ir[2] * d3);
        r[1] = mac44(2, g.ir[1] * d3 - g.ir[3] * d1);
        r[2] = mac44(3, g.ir[2] * d1 - g.ir[1] * d2);
        set_mac_ir(r, shift, lm);
        break;
    }
    case 0x3D:                                  /* GPF */
        for (int i = 1; i <= 3; i++) {
            r[i - 1] = mac44(i, (int64_t) g.ir[0] * g.ir[i]);
        }
        set_mac_ir(r, shift, lm);
        push_color();
        break;
    case 0x3E:                                  /* GPL */
        for (int i = 1; i <= 3; i++) {
            r[i - 1] = mac44(i, ((int64_t) g.mac[i] << shift) + (int64_t) g.ir[0] * g.ir[i]);
        }
        set_mac_ir(r, shift, lm);
        push_color();
        break;
    case 0x1E:                                  /* NCS */
        normal_light(0, shift, lm);
        color_stage(0, shift, lm);
        break;
    case 0x20:                                  /* NCT */
        for (int n = 0; n < 3; n++) {
            normal_light(n, shift, lm);
            color_stage(0, shift, lm);
        }
        break;
    case 0x1B:                                  /* NCCS */
        normal_light(0, shift, lm);
        color_stage(1, shift, lm);
        break;
    case 0x3F:                                  /* NCCT */
        for (int n = 0; n < 3; n++) {
            normal_light(n, shift, lm);
            color_stage(1, shift, lm);
        }
        break;
    case 0x13:                                  /* NCDS */
        normal_light(0, shift, lm);
        color_stage(2, shift, lm);
        break;
    case 0x16:                                  /* NCDT */
        for (int n = 0; n < 3; n++) {
            normal_light(n, shift, lm);
            color_stage(2, shift, lm);
        }
        break;
    case 0x1C: {                                /* CC: colour matrix from IR */
        int32_t ir[3] = { g.ir[1], g.ir[2], g.ir[3] };
        for (int i = 0; i < 3; i++) {
            r[i] = matrix_row(i + 1, g.lcm[i], ir, (int64_t) g.bk[i] << 12);
        }
        set_mac_ir(r, shift, lm);
        color_stage(1, shift, lm);
        break;
    }
    case 0x14: {                                /* CDP */
        int32_t ir[3] = { g.ir[1], g.ir[2], g.ir[3] };
        for (int i = 0; i < 3; i++) {
            r[i] = matrix_row(i + 1, g.lcm[i], ir, (int64_t) g.bk[i] << 12);
        }
        set_mac_ir(r, shift, lm);
        color_stage(2, shift, lm);
        break;
    }
    case 0x10:                                  /* DPCS */
    case 0x2A: {                                /* DPCT */
        int count = (command & 0x3F) == 0x10 ? 1 : 3;
        for (int n = 0; n < count; n++) {
            const uint8_t *c = count == 1 ? g.rgbc : g.rgb[0];
            int64_t u[3] = { (int64_t) c[0] << 16, (int64_t) c[1] << 16, (int64_t) c[2] << 16 };
            interpolate(u, shift, lm);
            push_color();
        }
        break;
    }
    case 0x11: {                                /* INTPL */
        int64_t u[3] = { (int64_t) g.ir[1] << 12, (int64_t) g.ir[2] << 12, (int64_t) g.ir[3] << 12 };
        interpolate(u, shift, lm);
        push_color();
        break;
    }
    case 0x29: {                                /* DCPL */
        int64_t u[3];
        for (int i = 1; i <= 3; i++) {
            u[i - 1] = ((int64_t) g.rgbc[i - 1] * g.ir[i]) << 4;
        }
        interpolate(u, shift, lm);
        push_color();
        break;
    }
    default:
        break;
    }
    if (g.flag & GTE_FLAG_ERROR) {
        g.flag |= GTE_FLAG_ERROR_BIT;
    }
}
