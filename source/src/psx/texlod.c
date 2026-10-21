/*
 * texlod.c - detailed distant textures (see texlod.h).
 *
 * Roll Away draws the track blocks near the camera with 64 x 64 texel,
 * 8-bit textures and the blocks further away with 32 x 32, 4-bit copies of
 * them (each with its own 16 colours), which look blurred and washed out
 * at a high resolution.  A copy is told apart from the full-size texture by
 * content: its 32 x 32 texels against every 64 x 64 cell of the 8-bit pages
 * and palettes the game has drawn 3D polygons with, scaled down to half
 * (each texel the mean of four); the closest, if close enough, is taken.
 * Results are kept per cell until new textures are loaded.  Only the
 * high-resolution picture changes; the game's own VRAM and drawing stay
 * as they are.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "psx/texlod.h"
#include "psx/gpu.h"
#include "psx/log.h"

int g_texlod_log;                       /* (the log: the first matches) */

#define SMALL                   32      /* texels per side of a copy */
#define FULL                    64      /* of the full-size texture */
#define PAGE_TEXELS             256
#define MAX_SOURCES             32      /* 8-bit pages seen (each with its first palette) */
#define CELLS_PER_PAGE          ((PAGE_TEXELS / FULL) * (PAGE_TEXELS / FULL))
#define MAX_MATCHES             512
#define MIN_COMPARED            (SMALL * SMALL / 2)

typedef struct {
    int page_x, page_y, clut_x, clut_y;
    int have_small;                     /* `small` is made */
    uint8_t small[CELLS_PER_PAGE][SMALL * SMALL][4];   /* RGB + opaque, scaled down */
} Source;

typedef struct {
    int used;
    int page_x, page_y, clut_x, clut_y, cell_u, cell_v;
    int sources;                        /* how many sources it was compared with */
    int found;                          /* a full-size texture was found: */
    int source, cell;
    int gain[3];                        /* its colours to the copy's (x 256): distance shading */
} Match;

static int s_enabled = 1;
static Source *s_sources;
static int s_source_count;
static Match s_matches[MAX_MATCHES];

void texlod_set_enabled(int on)
{
    s_enabled = on;
}

int texlod_enabled(void)
{
    return s_enabled;
}

void texlod_forget(void)
{
    s_source_count = 0;
    memset(s_matches, 0, sizeof(s_matches));
}

static int overlaps(int x, int y, int w, int h, int ax, int ay, int aw, int ah)
{
    return x < ax + aw && ax < x + w && y < ay + ah && ay < y + h;
}

/* (VRAM pixels: a page is 256 texel rows; 8-bit texels two to a pixel, 4-bit four) */
void texlod_vram_changed(int x, int y, int w, int h)
{
    for (int i = 0; i < s_source_count; i++) {
        const Source *s = &s_sources[i];
        if (overlaps(x, y, w, h, s->page_x, s->page_y, PAGE_TEXELS / 2, PAGE_TEXELS)
            || overlaps(x, y, w, h, s->clut_x, s->clut_y, 256, 1)) {
            texlod_forget();
            return;
        }
    }
    for (int i = 0; i < MAX_MATCHES; i++) {
        Match *m = &s_matches[i];
        if (m->used && (overlaps(x, y, w, h, m->page_x + m->cell_u / 4, m->page_y + m->cell_v,
                                 SMALL / 4, SMALL)
                        || overlaps(x, y, w, h, m->clut_x, m->clut_y, 16, 1))) {
            m->used = 0;
        }
    }
}

static uint16_t vram(int x, int y)
{
    return g_vram[y & (GPU_VRAM_HEIGHT - 1)][x & (GPU_VRAM_WIDTH - 1)];
}

static uint16_t texel(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v)
{
    int index = depth == 0 ? (vram(page_x + u / 4, page_y + v) >> ((u & 3) * 4)) & 0xF
                           : (vram(page_x + u / 2, page_y + v) >> ((u & 1) * 8)) & 0xFF;
    return vram(clut_x + index, clut_y);
}

static void to_rgb(uint16_t c, int rgb[3])
{
    rgb[0] = (c & 31) << 3;
    rgb[1] = ((c >> 5) & 31) << 3;
    rgb[2] = ((c >> 10) & 31) << 3;
}

void texlod_note(const GpuPrim *p)
{
    if (!s_enabled || p->depth != 1) {
        return;
    }
    for (int i = 0; i < s_source_count; i++) {
        const Source *s = &s_sources[i];
        if (s->page_x == p->page_x && s->page_y == p->page_y) {
            return;                     /* (one palette per page: the pattern is the same) */
        }
    }
    if (!s_sources) {
        s_sources = (Source *) malloc(sizeof(Source) * MAX_SOURCES);
        if (!s_sources) {
            return;
        }
    }
    if (s_source_count == MAX_SOURCES) {
        return;
    }
    Source *s = &s_sources[s_source_count++];
    s->page_x = p->page_x;
    s->page_y = p->page_y;
    s->clut_x = p->clut_x;
    s->clut_y = p->clut_y;
    s->have_small = 0;
}

/* the source's 16 cells, each scaled down to half */
static void make_small(Source *s)
{
    for (int cell = 0; cell < CELLS_PER_PAGE; cell++) {
        int cu = (cell % (PAGE_TEXELS / FULL)) * FULL, cv = (cell / (PAGE_TEXELS / FULL)) * FULL;
        for (int v = 0; v < SMALL; v++) {
            for (int u = 0; u < SMALL; u++) {
                int sum[3] = { 0, 0, 0 }, n = 0;
                for (int k = 0; k < 4; k++) {
                    uint16_t t = texel(s->page_x, s->page_y, 1, s->clut_x, s->clut_y,
                                       cu + 2 * u + (k & 1), cv + 2 * v + (k >> 1));
                    if (t) {
                        int c[3];
                        to_rgb(t, c);
                        sum[0] += c[0];
                        sum[1] += c[1];
                        sum[2] += c[2];
                        n++;
                    }
                }
                uint8_t *out = s->small[cell][v * SMALL + u];
                for (int c = 0; c < 3; c++) {
                    out[c] = (uint8_t) (n ? sum[c] / n : 0);
                }
                out[3] = n > 0;
            }
        }
    }
    s->have_small = 1;
}

/* Compares the copy's cell with every source cell; the best if close
 * enough.  The game shades distant blocks darker and duller (fog) through
 * the copies' palettes, so the patterns are compared by the correlation of
 * their brightness (blind to brightness and contrast), and the colours'
 * ratio is kept to shade the full-size texture alike. */
#define MATCH_MIN_PERMILLE      800     /* correlation x 1000 */

static int luma(const int *c)
{
    return (c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8;
}

static void find_match(Match *m)
{
    int copy[SMALL * SMALL][4];
    for (int v = 0; v < SMALL; v++) {
        for (int u = 0; u < SMALL; u++) {
            uint16_t t = texel(m->page_x, m->page_y, 0, m->clut_x, m->clut_y, m->cell_u + u,
                               m->cell_v + v);
            int *c = copy[v * SMALL + u];
            to_rgb(t, c);
            c[3] = t != 0;
        }
    }
    int best = -1, best_cell = 0;
    double best_corr = -1;
    long best_sum_a[3] = { 1, 1, 1 }, best_sum_b[3] = { 1, 1, 1 };
    for (int i = 0; i < s_source_count; i++) {
        Source *s = &s_sources[i];
        if (!s->have_small) {
            make_small(s);
        }
        for (int cell = 0; cell < CELLS_PER_PAGE; cell++) {
            long sum_a[3] = { 0, 0, 0 }, sum_b[3] = { 0, 0, 0 };
            double la = 0, lb = 0, laa = 0, lbb = 0, lab = 0;
            int compared = 0;
            for (int k = 0; k < SMALL * SMALL; k++) {
                const uint8_t *a8 = s->small[cell][k];
                const int *b = copy[k];
                if (!a8[3] || !b[3]) {
                    continue;
                }
                int a[3] = { a8[0], a8[1], a8[2] };
                for (int c = 0; c < 3; c++) {
                    sum_a[c] += a[c];
                    sum_b[c] += b[c];
                }
                double x = luma(a), y = luma(b);
                la += x;
                lb += y;
                laa += x * x;
                lbb += y * y;
                lab += x * y;
                compared++;
            }
            if (compared < MIN_COMPARED) {
                continue;
            }
            double n = compared;
            double va = laa - la * la / n, vb = lbb - lb * lb / n, cov = lab - la * lb / n;
            if (va < n || vb < n) {
                continue;                   /* (flat: nothing to compare) */
            }
            double corr = cov / sqrt(va * vb);
            if (corr > best_corr) {
                best = i;
                best_cell = cell;
                best_corr = corr;
                memcpy(best_sum_a, sum_a, sizeof(sum_a));
                memcpy(best_sum_b, sum_b, sizeof(sum_b));
            }
        }
    }
    m->sources = s_source_count;
    m->found = best >= 0 && best_corr * 1000 >= MATCH_MIN_PERMILLE;
    m->source = best;
    m->cell = best_cell;
    for (int c = 0; c < 3; c++) {
        long g = best_sum_a[c] ? best_sum_b[c] * 256 / best_sum_a[c] : 256;
        m->gain[c] = (int) (g < 64 ? 64 : g > 512 ? 512 : g);
    }
    if (g_texlod_log < 20 && m->found) {
        g_texlod_log++;
        psx_log("texlod: page %d,%d clut %d,%d cell %d,%d: %s %.3f (%d sources)", m->page_x,
                m->page_y, m->clut_x, m->clut_y, m->cell_u, m->cell_v,
                m->found ? "found" : "none", best_corr, s_source_count);
    }
}

int texlod_upgrade(GpuPrim *p, int *u, int *v, int count, int gain[3])
{
    if (!s_enabled || p->depth != 0 || !p->textured || s_source_count == 0) {
        return 0;
    }
    int u0 = u[0], u1 = u[0], v0 = v[0], v1 = v[0];
    for (int k = 1; k < count; k++) {
        u0 = u[k] < u0 ? u[k] : u0;
        u1 = u[k] > u1 ? u[k] : u1;
        v0 = v[k] < v0 ? v[k] : v0;
        v1 = v[k] > v1 ? v[k] : v1;
    }
    int cell_u = u0 & ~(SMALL - 1), cell_v = v0 & ~(SMALL - 1);
    if (u1 >= cell_u + SMALL || v1 >= cell_v + SMALL) {
        return 0;                       /* (not within one cell) */
    }
    unsigned hash = (unsigned) (p->page_x * 7 + p->page_y * 131 + p->clut_x * 31 + p->clut_y * 1009
                                + cell_u * 17 + cell_v * 257);
    Match *m = NULL, *free_slot = NULL;
    for (int i = 0; i < 16; i++) {
        Match *e = &s_matches[(hash + (unsigned) i) % MAX_MATCHES];
        if (e->used && e->page_x == p->page_x && e->page_y == p->page_y
            && e->clut_x == p->clut_x && e->clut_y == p->clut_y && e->cell_u == cell_u
            && e->cell_v == cell_v) {
            m = e;
            break;
        }
        if (!e->used && !free_slot) {
            free_slot = e;
        }
    }
    if (!m) {
        m = free_slot ? free_slot : &s_matches[hash % MAX_MATCHES];
        memset(m, 0, sizeof(*m));
        m->used = 1;
        m->page_x = p->page_x;
        m->page_y = p->page_y;
        m->clut_x = p->clut_x;
        m->clut_y = p->clut_y;
        m->cell_u = cell_u;
        m->cell_v = cell_v;
        m->sources = -1;
    }
    if (m->sources != s_source_count) {
        find_match(m);                  /* (new candidates since the last try) */
    }
    if (!m->found) {
        return 0;
    }
    const Source *s = &s_sources[m->source];
    int full_u = (m->cell % (PAGE_TEXELS / FULL)) * FULL, full_v = (m->cell / (PAGE_TEXELS / FULL)) * FULL;
    p->page_x = s->page_x;
    p->page_y = s->page_y;
    p->clut_x = s->clut_x;
    p->clut_y = s->clut_y;
    p->depth = 1;
    memcpy(gain, m->gain, sizeof(m->gain));
    for (int k = 0; k < count; k++) {
        u[k] = full_u + (u[k] - cell_u) * (FULL / SMALL);
        v[k] = full_v + (v[k] - cell_v) * (FULL / SMALL);
    }
    return 1;
}
