/*
 * gpu.c - the PS1 GPU in software (see gpu.h).
 *
 * Behaviour after the "GPU" chapter of psx-spx (nocash): 11-bit signed
 * vertices plus the drawing offset, the drawing area as an inclusive
 * clip rectangle, polygons without their right and bottom edges, texture
 * pages with 4/8-bit CLUT or 15-bit texels (texel 0 is transparent),
 * texture windows, the four semi-transparency modes (only for texels with
 * bit 15 set), the 4x4 dither matrix for shaded and texture-blended
 * polygons, and the mask bit.
 *
 * With a higher internal resolution or precise vertices, every primitive
 * is also handed to gpu_hires.c (with the GTE's float positions where
 * pgxp.c knows them), and the window shows that copy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx/gpu.h"
#include "psx/gpu_hires.h"
#include "psx/pgxp.h"
#include "psx/balls.h"
#include "psx/log.h"

uint16_t g_vram[GPU_VRAM_HEIGHT][GPU_VRAM_WIDTH];

/* longest command (polyline) we buffer */
#define MAX_COMMAND_WORDS       256
/* a polyline ends with this pattern in a vertex position */
#define POLYLINE_END_MASK       0xF000F000u
#define POLYLINE_END            0x50005000u
/* the longest edge a polygon or line may have */
#define MAX_EDGE_DX             1023
#define MAX_EDGE_DY             511

typedef struct {
    /* draw mode, GP0(E1h) */
    int page_x, page_y;             /* texture page origin in VRAM */
    int depth;                      /* 0: 4-bit CLUT, 1: 8-bit CLUT, 2: 15-bit */
    int blend;                      /* semi-transparency mode 0..3 */
    int dither;
    int flip_x, flip_y;             /* textured rectangles */
    /* texture window, GP0(E2h), in 8-pixel units */
    int win_mask_x, win_mask_y, win_off_x, win_off_y;
    /* drawing area (inclusive) and offset, GP0(E3h..E5h) */
    int clip_x1, clip_y1, clip_x2, clip_y2;
    int off_x, off_y;
    /* mask bit, GP0(E6h) */
    int set_mask, check_mask;
    /* display */
    int disp_x, disp_y, disp_w, disp_h, disp_rgb24, disp_on;
} GpuState;

static GpuState s;

/* command being received, and where each word came from in memory (NULL:
 * written to GP0 directly) */
static uint32_t s_cmd[MAX_COMMAND_WORDS];
static const uint32_t *s_cmd_src[MAX_COMMAND_WORDS];
static int s_cmd_len;

/* the displayed picture (gpu_display_rgba) */
static uint32_t *s_picture;
static int s_picture_size;

/* a CPU to VRAM transfer through GP0(A0h) */
static int s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, s_xfer_pos, s_xfer_left;

typedef GpuPrim Prim;

typedef struct {
    int x, y;
    int r, g, b;
    int u, v;
} Vertex;

static const int8_t DITHER[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};

/* ---- state ------------------------------------------------------------ */

void gpu_reset(void)
{
    memset(g_vram, 0, sizeof(g_vram));
    memset(&s, 0, sizeof(s));
    s.clip_x2 = GPU_VRAM_WIDTH - 1;
    s.clip_y2 = GPU_VRAM_HEIGHT - 1;
    s.disp_w = 320;
    s.disp_h = 240;
    s_cmd_len = 0;
    s_xfer_left = 0;
    hires_copy_from_original(0, 0, GPU_VRAM_WIDTH, GPU_VRAM_HEIGHT);
}

void gpu_set_enhancements(int scale, int precise_vertices, int texture_filter)
{
    g_pgxp_enabled = precise_vertices;
    hires_configure(scale > 1 || precise_vertices ? scale : 0);
    hires_set_texture_filter(precise_vertices && texture_filter);
}

void gpu_skip_frame(void)
{
    hires_discard();
}

/* the state a primitive queued for the high-resolution copy needs */
static void current_env(GpuEnv *e)
{
    e->clip_x1 = s.clip_x1;
    e->clip_y1 = s.clip_y1;
    e->clip_x2 = s.clip_x2;
    e->clip_y2 = s.clip_y2;
    e->win_mask_x = s.win_mask_x;
    e->win_mask_y = s.win_mask_y;
    e->win_off_x = s.win_off_x;
    e->win_off_y = s.win_off_y;
    e->set_mask = s.set_mask;
    e->check_mask = s.check_mask;
}

void gpu_set_draw_area(int x1, int y1, int x2, int y2)
{
    s.clip_x1 = x1 & 0x3FF;
    s.clip_y1 = y1 & 0x1FF;
    s.clip_x2 = x2 & 0x3FF;
    s.clip_y2 = y2 & 0x1FF;
}

static int sign_extend_11(uint32_t value)
{
    return (int) (value << 21) >> 21;
}

void gpu_set_draw_offset(int x, int y)
{
    s.off_x = sign_extend_11((uint32_t) x);
    s.off_y = sign_extend_11((uint32_t) y);
}

/* the texture page bits of GP0(E1h) / a textured polygon's tpage */
static void set_texture_page(uint32_t tpage)
{
    s.page_x = (tpage & 0xF) * 64;
    s.page_y = ((tpage >> 4) & 1) * 256;
    s.blend = (tpage >> 5) & 3;
    s.depth = (tpage >> 7) & 3;
}

void gpu_set_draw_mode(uint32_t e1)
{
    set_texture_page(e1);
    s.dither = (e1 >> 9) & 1;
    s.flip_x = (e1 >> 12) & 1;
    s.flip_y = (e1 >> 13) & 1;
}

void gpu_set_texture_window(uint32_t e2)
{
    s.win_mask_x = e2 & 0x1F;
    s.win_mask_y = (e2 >> 5) & 0x1F;
    s.win_off_x = (e2 >> 10) & 0x1F;
    s.win_off_y = (e2 >> 15) & 0x1F;
}

void gpu_set_display(int x, int y, int w, int h, int rgb24)
{
    s.disp_x = x & 0x3FF;
    s.disp_y = y & 0x1FF;
    s.disp_w = w;
    s.disp_h = h;
    s.disp_rgb24 = rgb24;
}

void gpu_set_display_enabled(int enabled)
{
    s.disp_on = enabled;
}

/* ---- pixels ----------------------------------------------------------- */

static uint16_t *vram_at(int x, int y)
{
    return &g_vram[y & (GPU_VRAM_HEIGHT - 1)][x & (GPU_VRAM_WIDTH - 1)];
}

static uint16_t texel(const Prim *p, int u, int v)
{
    u &= 0xFF;
    v &= 0xFF;
    u = (u & ~(s.win_mask_x * 8)) | ((s.win_off_x & s.win_mask_x) * 8);
    v = (v & ~(s.win_mask_y * 8)) | ((s.win_off_y & s.win_mask_y) * 8);
    switch (p->depth) {
    case 0: {
        uint16_t word = *vram_at(p->page_x + u / 4, p->page_y + v);
        return *vram_at(p->clut_x + ((word >> ((u & 3) * 4)) & 0xF), p->clut_y);
    }
    case 1: {
        uint16_t word = *vram_at(p->page_x + u / 2, p->page_y + v);
        return *vram_at(p->clut_x + ((word >> ((u & 1) * 8)) & 0xFF), p->clut_y);
    }
    default:
        return *vram_at(p->page_x + u, p->page_y + v);
    }
}

static int clamp255(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

/* 8-bit colour to 15 bits, dithered or with the low bits cut */
static uint16_t to_15bit(int r, int g, int b, int x, int y, int dither)
{
    if (dither) {
        int d = DITHER[y & 3][x & 3];
        r = clamp255(r + d);
        g = clamp255(g + d);
        b = clamp255(b + d);
    } else {
        r = clamp255(r);
        g = clamp255(g);
        b = clamp255(b);
    }
    return (uint16_t) ((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
}

static uint16_t blend(uint16_t back, uint16_t front, int mode)
{
    int result = 0;
    for (int shift = 0; shift < 15; shift += 5) {
        int b = (back >> shift) & 31, f = (front >> shift) & 31, c;
        switch (mode) {
        case 0: c = (b + f) >> 1; break;
        case 1: c = b + f; break;
        case 2: c = b - f; break;
        default: c = b + (f >> 2); break;
        }
        c = c < 0 ? 0 : c > 31 ? 31 : c;
        result |= c << shift;
    }
    return (uint16_t) result;
}

static int inside_clip(int x, int y)
{
    return x >= s.clip_x1 && x <= s.clip_x2 && y >= s.clip_y1 && y <= s.clip_y2;
}

/* Draws one pixel of a primitive (already clipped). */
static void plot(const Prim *p, int x, int y, int r, int g, int b, int u, int v)
{
    uint16_t *dst = vram_at(x, y);
    uint16_t color;
    uint16_t mask = (uint16_t) (s.set_mask << 15);
    int semi = p->semi;

    if (s.check_mask && (*dst & 0x8000)) {
        return;
    }
    if (p->textured) {
        uint16_t t = texel(p, u, v);
        if (t == 0) {
            return;                         /* fully transparent texel */
        }
        semi = p->semi && (t & 0x8000);
        mask |= t & 0x8000;
        if (p->raw) {
            color = t & 0x7FFF;
        } else {
            /* texel x colour / 128, in 8 bits */
            color = to_15bit(((t & 31) * r) >> 4, (((t >> 5) & 31) * g) >> 4,
                             (((t >> 10) & 31) * b) >> 4, x, y, p->dither);
        }
    } else {
        color = to_15bit(r, g, b, x, y, p->dither);
    }
    if (semi) {
        color = blend(*dst, color, p->blend);
    }
    *dst = color | mask;
}

/* ---- polygons --------------------------------------------------------- */

static int64_t edge(const Vertex *a, const Vertex *b, int x, int y)
{
    return (int64_t) (b->x - a->x) * (y - a->y) - (int64_t) (b->y - a->y) * (x - a->x);
}

/* the edge a->b belongs to the triangle (top or left edge) */
static int top_left(const Vertex *a, const Vertex *b)
{
    int dx = b->x - a->x, dy = b->y - a->y;
    return dy < 0 || (dy == 0 && dx > 0);
}

static int edge_too_long(const Vertex *a, const Vertex *b)
{
    return abs(b->x - a->x) > MAX_EDGE_DX || abs(b->y - a->y) > MAX_EDGE_DY;
}

static int interpolate(int64_t w0, int64_t w1, int64_t w2, int a0, int a1, int a2, int64_t area)
{
    return (int) ((w0 * a0 + w1 * a1 + w2 * a2) / area);
}

/* polygons with no area or too long an edge are not drawn */
static int triangle_drawn(const Vertex *v0, const Vertex *v1, const Vertex *v2)
{
    return edge(v0, v1, v2->x, v2->y) != 0 && !edge_too_long(v0, v1) && !edge_too_long(v1, v2)
           && !edge_too_long(v2, v0);
}

static void draw_triangle(const Prim *p, const Vertex *v0, const Vertex *v1, const Vertex *v2)
{
    int64_t area = edge(v0, v1, v2->x, v2->y);

    if (!triangle_drawn(v0, v1, v2)) {
        return;
    }
    if (area < 0) {
        const Vertex *t = v1;
        v1 = v2;
        v2 = t;
        area = -area;
    }
    int min_x = v0->x, max_x = v0->x, min_y = v0->y, max_y = v0->y;
    const Vertex *vs[3] = { v0, v1, v2 };
    for (int i = 1; i < 3; i++) {
        min_x = vs[i]->x < min_x ? vs[i]->x : min_x;
        max_x = vs[i]->x > max_x ? vs[i]->x : max_x;
        min_y = vs[i]->y < min_y ? vs[i]->y : min_y;
        max_y = vs[i]->y > max_y ? vs[i]->y : max_y;
    }
    min_x = min_x < s.clip_x1 ? s.clip_x1 : min_x;
    min_y = min_y < s.clip_y1 ? s.clip_y1 : min_y;
    max_x = max_x > s.clip_x2 ? s.clip_x2 : max_x;
    max_y = max_y > s.clip_y2 ? s.clip_y2 : max_y;

    int tl0 = top_left(v1, v2), tl1 = top_left(v2, v0), tl2 = top_left(v0, v1);
    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            int64_t w0 = edge(v1, v2, x, y), w1 = edge(v2, v0, x, y), w2 = edge(v0, v1, x, y);
            if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !tl0) || (w1 == 0 && !tl1)
                || (w2 == 0 && !tl2)) {
                continue;
            }
            int r = v0->r, g = v0->g, b = v0->b, u = 0, v = 0;
            if (p->gouraud) {
                r = interpolate(w0, w1, w2, v0->r, v1->r, v2->r, area);
                g = interpolate(w0, w1, w2, v0->g, v1->g, v2->g, area);
                b = interpolate(w0, w1, w2, v0->b, v1->b, v2->b, area);
            }
            if (p->textured) {
                u = interpolate(w0, w1, w2, v0->u, v1->u, v2->u, area);
                v = interpolate(w0, w1, w2, v0->v, v1->v, v2->v, area);
            }
            plot(p, x, y, r, g, b, u, v);
        }
    }
}

static void set_color(Vertex *v, uint32_t word)
{
    v->r = word & 0xFF;
    v->g = (word >> 8) & 0xFF;
    v->b = (word >> 16) & 0xFF;
}

static void set_position(Vertex *v, uint32_t word)
{
    v->x = sign_extend_11(word) + s.off_x;
    v->y = sign_extend_11(word >> 16) + s.off_y;
}

static void set_clut(Prim *p, uint32_t clut)
{
    p->clut_x = (clut & 0x3F) * 16;
    p->clut_y = (clut >> 6) & 0x1FF;
}

/* the current texture page for rectangles and untextured primitives */
static void prim_from_state(Prim *p)
{
    p->page_x = s.page_x;
    p->page_y = s.page_y;
    p->depth = s.depth;
    p->blend = s.blend;
}

/* a vertex for the high-resolution copy: the GTE's float position and
 * depth if this packet word came from it */
static void hires_vertex(HiresVertex *h, const Vertex *v, int word_index)
{
    PgxpVertex pv;
    h->x = (float) v->x;
    h->y = (float) v->y;
    h->z = 0;
    h->r = v->r;
    h->g = v->g;
    h->b = v->b;
    h->u = v->u;
    h->v = v->v;
    if (g_pgxp_enabled && s_cmd_src[word_index]
        && pgxp_lookup(s_cmd_src[word_index], s_cmd[word_index], &pv)) {
        h->x = pv.x + (float) s.off_x;
        h->y = pv.y + (float) s.off_y;
        h->z = pv.z;
    }
}

/*
 * Whether the high-resolution copy draws a triangle.  The console drops
 * triangles with no area; with precise corners that is decided on the
 * float positions instead (hires_triangle()): a thin sliver whose whole-
 * pixel corners fall on one line still covers some of the finer pixels
 * (see also NCLIP in gte.c).  Too long an edge drops it, as on the console.
 */
static int hires_triangle_drawn(const Vertex *v0, const Vertex *v1, const Vertex *v2,
                                const HiresVertex *h0, const HiresVertex *h1,
                                const HiresVertex *h2)
{
    if (h0->z > 0 && h1->z > 0 && h2->z > 0) {
        return !edge_too_long(v0, v1) && !edge_too_long(v1, v2) && !edge_too_long(v2, v0);
    }
    return triangle_drawn(v0, v1, v2);
}

static void command_polygon(const uint32_t *c)
{
    uint32_t op = c[0] >> 24;
    int quad = (op & 0x08) != 0, gouraud = (op & 0x10) != 0;
    int count = quad ? 4 : 3;
    Prim p = { 0 };
    Vertex v[4];
    int position_word[4];
    int i = 1;

    p.textured = (op & 0x04) != 0;
    p.raw = p.textured && (op & 0x01);
    p.semi = (op & 0x02) != 0;
    p.gouraud = gouraud;
    for (int k = 0; k < count; k++) {
        set_color(&v[k], k > 0 && gouraud ? c[i++] : c[0]);
        position_word[k] = i;
        set_position(&v[k], c[i++]);
        if (p.textured) {
            uint32_t uv = c[i++];
            v[k].u = uv & 0xFF;
            v[k].v = (uv >> 8) & 0xFF;
            if (k == 0) {
                set_clut(&p, uv >> 16);
            } else if (k == 1) {
                set_texture_page(uv >> 16);     /* also changes the draw mode */
            }
        } else {
            v[k].u = v[k].v = 0;
        }
    }
    prim_from_state(&p);
    p.dither = s.dither && (gouraud || (p.textured && !p.raw));
    draw_triangle(&p, &v[0], &v[1], &v[2]);
    if (quad) {
        draw_triangle(&p, &v[1], &v[2], &v[3]);
    }
    if (hires_scale()) {
        HiresVertex h[4];
        GpuEnv e;
        current_env(&e);
        BallRecord *ball = g_smooth_balls && s_cmd_src[0] ? balls_find(s_cmd_src[0]) : NULL;
        if (ball && ball->visible) {
            hires_ball(ball, &e, s.off_x, s.off_y);     /* instead of the model and picture */
            return;
        }
        for (int k = 0; k < count; k++) {
            hires_vertex(&h[k], &v[k], position_word[k]);
        }
        if (hires_triangle_drawn(&v[0], &v[1], &v[2], &h[0], &h[1], &h[2])) {
            hires_triangle(&p, &e, &h[0], &h[1], &h[2]);
        }
        if (quad && hires_triangle_drawn(&v[1], &v[2], &v[3], &h[1], &h[2], &h[3])) {
            hires_triangle(&p, &e, &h[1], &h[2], &h[3]);
        }
    }
}

/* ---- lines ------------------------------------------------------------ */

static int lerp(int a, int b, int i, int n)
{
    return n ? a + ((b - a) * i * 2 + (b >= a ? n : -n)) / (2 * n) : a;
}

static void draw_line(const Prim *p, const Vertex *a, const Vertex *b)
{
    if (edge_too_long(a, b)) {
        return;
    }
    if (hires_scale()) {
        HiresVertex ha = { (float) a->x, (float) a->y, 0, a->r, a->g, a->b, 0, 0 };
        HiresVertex hb = { (float) b->x, (float) b->y, 0, b->r, b->g, b->b, 0, 0 };
        GpuEnv e;
        current_env(&e);
        hires_line(p, &e, &ha, &hb);
    }
    int dx = b->x - a->x, dy = b->y - a->y;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    for (int i = 0; i <= n; i++) {
        int x = lerp(a->x, b->x, i, n), y = lerp(a->y, b->y, i, n);
        if (!inside_clip(x, y)) {
            continue;
        }
        if (p->gouraud) {
            plot(p, x, y, lerp(a->r, b->r, i, n), lerp(a->g, b->g, i, n), lerp(a->b, b->b, i, n),
                 0, 0);
        } else {
            plot(p, x, y, a->r, a->g, a->b, 0, 0);
        }
    }
}

/* `count` words of a line or polyline (without the end marker) */
static void command_line(const uint32_t *c, int count)
{
    uint32_t op = c[0] >> 24;
    Prim p = { 0 };
    Vertex a, b;
    int i = 1;

    p.semi = (op & 0x02) != 0;
    p.gouraud = (op & 0x10) != 0;
    prim_from_state(&p);
    p.dither = s.dither && p.gouraud;
    set_color(&a, c[0]);
    set_position(&a, c[i++]);
    while (i < count) {
        set_color(&b, p.gouraud ? c[i++] : c[0]);
        if (i >= count) {
            break;
        }
        set_position(&b, c[i++]);
        draw_line(&p, &a, &b);
        a = b;
    }
}

/* ---- rectangles ------------------------------------------------------- */

static void command_rectangle(const uint32_t *c)
{
    static const int SIZES[4] = { 0, 1, 8, 16 };
    uint32_t op = c[0] >> 24;
    int size = SIZES[(op >> 3) & 3];
    Prim p = { 0 };
    Vertex v;
    int i = 2, w, h, u0 = 0, v0 = 0;

    p.textured = (op & 0x04) != 0;
    p.raw = p.textured && (op & 0x01);
    p.semi = (op & 0x02) != 0;
    prim_from_state(&p);
    set_color(&v, c[0]);
    set_position(&v, c[1]);
    if (p.textured) {
        u0 = c[2] & 0xFF;
        v0 = (c[2] >> 8) & 0xFF;
        set_clut(&p, c[2] >> 16);
        i = 3;
    }
    if (size) {
        w = h = size;
    } else {
        w = c[i] & 0x3FF;
        h = (c[i] >> 16) & 0x1FF;
    }
    if (hires_scale()) {
        GpuEnv e;
        current_env(&e);
        hires_rectangle(&p, &e, v.x, v.y, w, h, v.r, v.g, v.b, u0, v0, s.flip_x, s.flip_y);
    }
    for (int row = 0; row < h; row++) {
        int y = v.y + row;
        if (y < s.clip_y1 || y > s.clip_y2) {
            continue;
        }
        int tv = s.flip_y ? v0 - row : v0 + row;
        for (int col = 0; col < w; col++) {
            int x = v.x + col;
            if (x < s.clip_x1 || x > s.clip_x2) {
                continue;
            }
            plot(&p, x, y, v.r, v.g, v.b, s.flip_x ? u0 - col : u0 + col, tv);
        }
    }
}

/* ---- VRAM ------------------------------------------------------------- */

void gpu_fill(int x, int y, int w, int h, uint32_t rgb24)
{
    uint16_t color = (uint16_t) (((rgb24 >> 3) & 31) | ((rgb24 >> 11) & 31) << 5
                                 | ((rgb24 >> 19) & 31) << 10);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            *vram_at(x + col, y + row) = color;
        }
    }
    hires_fill(x & (GPU_VRAM_WIDTH - 1), y & (GPU_VRAM_HEIGHT - 1), w, h, color);
}

void gpu_load_image(int x, int y, int w, int h, const uint16_t *data)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            *vram_at(x + col, y + row) = *data++;
        }
    }
    hires_copy_from_original(x, y, w, h);
}

void gpu_store_image(int x, int y, int w, int h, uint16_t *data)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            *data++ = *vram_at(x + col, y + row);
        }
    }
}

void gpu_move_image(int sx, int sy, int dx, int dy, int w, int h)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint16_t *dst = vram_at(dx + col, dy + row);
            if (s.check_mask && (*dst & 0x8000)) {
                continue;
            }
            *dst = *vram_at(sx + col, sy + row) | (uint16_t) (s.set_mask << 15);
        }
    }
    hires_move(sx & (GPU_VRAM_WIDTH - 1), sy & (GPU_VRAM_HEIGHT - 1), dx & (GPU_VRAM_WIDTH - 1),
               dy & (GPU_VRAM_HEIGHT - 1), w, h, s.set_mask, s.check_mask);
}

/* ---- command stream --------------------------------------------------- */

/* words of a command (0: variable, a polyline) */
static int command_length(uint32_t op)
{
    switch (op >> 5) {
    case 1: {                                   /* polygon */
        int verts = (op & 0x08) ? 4 : 3;
        int textured = (op & 0x04) != 0;
        return 1 + verts * (1 + textured) + ((op & 0x10) ? verts - 1 : 0);
    }
    case 2:                                     /* line */
        return (op & 0x08) ? 0 : ((op & 0x10) ? 4 : 3);
    case 3:                                     /* rectangle */
        return 2 + ((op & 0x04) != 0) + (((op >> 3) & 3) == 0);
    case 4:                                     /* VRAM to VRAM */
        return 4;
    case 5:                                     /* CPU to VRAM */
    case 6:                                     /* VRAM to CPU */
        return 3;
    default:
        return op == 0x02 ? 3 : 1;              /* fill, misc, environment */
    }
}

static void execute(const uint32_t *c, int count)
{
    uint32_t op = c[0] >> 24;
    switch (op >> 5) {
    case 1:
        command_polygon(c);
        return;
    case 2:
        command_line(c, count);
        return;
    case 3:
        command_rectangle(c);
        return;
    case 4:
        gpu_move_image(c[1] & 0x3FF, (c[1] >> 16) & 0x1FF, c[2] & 0x3FF, (c[2] >> 16) & 0x1FF,
                       (((c[3] & 0xFFFF) - 1) & 0x3FF) + 1, (((c[3] >> 16) - 1) & 0x1FF) + 1);
        return;
    case 5:
        s_xfer_x = c[1] & 0x3FF;
        s_xfer_y = (c[1] >> 16) & 0x1FF;
        s_xfer_w = (((c[2] & 0xFFFF) - 1) & 0x3FF) + 1;
        s_xfer_h = (((c[2] >> 16) - 1) & 0x1FF) + 1;
        s_xfer_pos = 0;
        s_xfer_left = (s_xfer_w * s_xfer_h + 1) / 2;
        return;
    case 6:
        return;                                 /* no read port */
    }
    switch (op) {
    case 0x02:
        gpu_fill(c[1] & 0x3F0, (c[1] >> 16) & 0x1FF, ((c[2] & 0x3FF) + 0xF) & ~0xF,
                 (c[2] >> 16) & 0x1FF, c[0] & 0xFFFFFF);
        break;
    case 0xE1:
        gpu_set_draw_mode(c[0]);
        break;
    case 0xE2:
        gpu_set_texture_window(c[0]);
        break;
    case 0xE3:
        s.clip_x1 = c[0] & 0x3FF;
        s.clip_y1 = (c[0] >> 10) & 0x1FF;
        break;
    case 0xE4:
        s.clip_x2 = c[0] & 0x3FF;
        s.clip_y2 = (c[0] >> 10) & 0x1FF;
        break;
    case 0xE5:
        s.off_x = sign_extend_11(c[0]);
        s.off_y = sign_extend_11(c[0] >> 11);
        break;
    case 0xE6:
        s.set_mask = c[0] & 1;
        s.check_mask = (c[0] >> 1) & 1;
        break;
    default:
        break;                                  /* NOP, cache flush, IRQ */
    }
}

static void gp0(uint32_t word, const uint32_t *source)
{
    if (s_xfer_left > 0) {
        for (int half = 0; half < 2 && s_xfer_pos < s_xfer_w * s_xfer_h; half++) {
            *vram_at(s_xfer_x + s_xfer_pos % s_xfer_w, s_xfer_y + s_xfer_pos / s_xfer_w) =
                (uint16_t) (word >> (half * 16));
            s_xfer_pos++;
        }
        if (--s_xfer_left == 0) {
            hires_copy_from_original(s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h);
        }
        return;
    }
    s_cmd_src[s_cmd_len] = source;
    s_cmd[s_cmd_len++] = word;
    int length = command_length(s_cmd[0] >> 24);
    if (length == 0) {
        /* polyline: ends with the marker in a vertex (or colour) position */
        int first = ((s_cmd[0] >> 24) & 0x10) ? 4 : 3;
        if ((s_cmd_len > first && (word & POLYLINE_END_MASK) == POLYLINE_END)
            || s_cmd_len == MAX_COMMAND_WORDS) {
            execute(s_cmd, s_cmd_len - 1);
            s_cmd_len = 0;
        }
    } else if (s_cmd_len >= length) {
        execute(s_cmd, s_cmd_len);
        s_cmd_len = 0;
    }
}

void gpu_gp0(uint32_t word)
{
    gp0(word, NULL);
}

void gpu_gp0_words(const uint32_t *words, int count)
{
    for (int i = 0; i < count; i++) {
        gp0(words[i], &words[i]);
    }
}

/* ---- display ---------------------------------------------------------- */

static uint32_t expand_5bit(int c)
{
    return (uint32_t) ((c << 3) | (c >> 2));
}

int gpu_display_width(void)
{
    return s.disp_w;
}

int gpu_display_lines(void)
{
    return s.disp_h;
}

const uint32_t *gpu_display_rgba(int *w, int *h)
{
    if (!s.disp_on || s.disp_w <= 0 || s.disp_h <= 0) {
        return NULL;
    }
    int scale = s.disp_rgb24 ? 1 : hires_scale();
    scale = scale < 1 ? 1 : scale;
    int size = s.disp_w * s.disp_h * scale * scale;
    if (size > s_picture_size) {
        free(s_picture);
        s_picture = (uint32_t *) malloc((size_t) size * sizeof(uint32_t));
        s_picture_size = s_picture ? size : 0;
        if (!s_picture) {
            return NULL;
        }
    }
    *w = s.disp_w * scale;
    *h = s.disp_h * scale;
    if (hires_scale() && !s.disp_rgb24) {
        hires_read(s_picture, s.disp_x, s.disp_y, s.disp_w, s.disp_h);
        return s_picture;
    }
    uint32_t *out = s_picture;
    for (int y = 0; y < s.disp_h; y++) {
        if (s.disp_rgb24) {
            const uint8_t *row = (const uint8_t *) g_vram[(s.disp_y + y) & (GPU_VRAM_HEIGHT - 1)];
            for (int x = 0; x < s.disp_w; x++) {
                int offset = (s.disp_x * 2 + x * 3) % (GPU_VRAM_WIDTH * 2);
                *out++ = 0xFF000000u | row[offset] | row[offset + 1] << 8
                         | (uint32_t) row[offset + 2] << 16;
            }
        } else {
            for (int x = 0; x < s.disp_w; x++) {
                uint16_t p = *vram_at(s.disp_x + x, s.disp_y + y);
                *out++ = 0xFF000000u | expand_5bit(p & 31) | expand_5bit((p >> 5) & 31) << 8
                         | expand_5bit((p >> 10) & 31) << 16;
            }
        }
    }
    return s_picture;
}

/* ---- debugging -------------------------------------------------------- */

void gpu_debug_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint8_t header[54] = { 'B', 'M' };
    uint32_t size = 54 + GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT * 3;

    psx_log("GPU: display %s at %d,%d size %dx%d%s; draw area %d,%d-%d,%d offset %d,%d",
            s.disp_on ? "on" : "off", s.disp_x, s.disp_y, s.disp_w, s.disp_h,
            s.disp_rgb24 ? " (24-bit)" : "", s.clip_x1, s.clip_y1, s.clip_x2, s.clip_y2,
            s.off_x, s.off_y);
    if (!f) {
        return;
    }
    memcpy(header + 2, &size, 4);
    header[10] = 54;
    header[14] = 40;
    int32_t w = GPU_VRAM_WIDTH, h = -GPU_VRAM_HEIGHT;   /* top-down */
    memcpy(header + 18, &w, 4);
    memcpy(header + 22, &h, 4);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (int y = 0; y < GPU_VRAM_HEIGHT; y++) {
        for (int x = 0; x < GPU_VRAM_WIDTH; x++) {
            uint16_t p = g_vram[y][x];
            uint8_t bgr[3] = { (uint8_t) expand_5bit((p >> 10) & 31),
                               (uint8_t) expand_5bit((p >> 5) & 31), (uint8_t) expand_5bit(p & 31) };
            fwrite(bgr, 1, 3, f);
        }
    }
    fclose(f);
}
