/*
 * hle_gpu.c - LIBGPU's core (SYS.OBJ: ResetGraph, DrawOTag, LoadImage,
 * PutDrawEnv, PutDispEnv ...) on top of the software GPU (psx/gpu.c).
 *
 * The arguments are PS1 addresses of the game's structures, which lie in
 * PS1 memory with the console's layout.  Drawing finishes at once, so
 * DrawSync() never waits; the DrawSyncCallback runs at the next
 * opportunity, as after the GPU's interrupt.
 */
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/irq.h"
#include "runtime/system.h"
#include "psx/gpu.h"
#include "psx/log.h"

#define LIST_END                0x00FFFFFFu
#define LINK_MASK               0x00FFFFFFu

/* GP0 commands */
#define GP0_FILL                0x02000000u
#define GP0_DRAW_MODE           0xE1000000u
#define GP0_TEXTURE_WINDOW      0xE2000000u
#define GP0_AREA_START          0xE3000000u
#define GP0_AREA_END            0xE4000000u
#define GP0_OFFSET              0xE5000000u
#define GP0_MASK                0xE6000000u

/* DRAWENV (0x5C bytes) */
#define DE_CLIP                 0x00            /* RECT */
#define DE_OFS                  0x08            /* short[2] */
#define DE_TW                   0x0C            /* RECT */
#define DE_TPAGE                0x14
#define DE_DTD                  0x16
#define DE_DFE                  0x17
#define DE_ISBG                 0x18
#define DE_R0                   0x19
#define DRAWENV_SIZE            0x5C
/* DISPENV (0x14 bytes) */
#define DI_DISP                 0x00            /* RECT */
#define DI_SCREEN               0x08            /* RECT */
#define DI_ISINTER              0x10
#define DI_ISRGB24              0x11
#define DISPENV_SIZE            0x14

static uint8_t s_drawenv[DRAWENV_SIZE];
static uint8_t s_dispenv[DISPENV_SIZE];

void hle_gpu_reset(void)
{
    memset(s_drawenv, 0, sizeof(s_drawenv));
    memset(s_dispenv, 0, sizeof(s_dispenv));
    gpu_reset();
}

static int16_t s16(uint32_t a)
{
    return (int16_t) RD16(a);
}

typedef struct {
    int x, y, w, h;
} Rect;

static Rect rect_at(uint32_t a)
{
    Rect r = { s16(a), s16(a + 2), s16(a + 4), s16(a + 6) };
    return r;
}

/* ---- primitives built for the drawing environment ---------------------- */

static uint32_t area_start(int x, int y)
{
    return GP0_AREA_START | (uint32_t) (y & 0x3FF) << 10 | (uint32_t) (x & 0x3FF);
}

static uint32_t area_end(int x, int y)
{
    x = x < 0 ? 0 : x > GPU_VRAM_WIDTH - 1 ? GPU_VRAM_WIDTH - 1 : x;
    y = y < 0 ? 0 : y > GPU_VRAM_HEIGHT - 1 ? GPU_VRAM_HEIGHT - 1 : y;
    return GP0_AREA_END | (uint32_t) (y & 0x3FF) << 10 | (uint32_t) (x & 0x3FF);
}

static uint32_t offset_command(int x, int y)
{
    return GP0_OFFSET | (uint32_t) (y & 0x7FF) << 11 | (uint32_t) (x & 0x7FF);
}

static uint32_t mode_command(int dfe, int dtd, int tpage)
{
    return GP0_DRAW_MODE | (dtd ? 0x200u : 0) | (dfe ? 0x400u : 0) | ((uint32_t) tpage & 0x9FF);
}

static uint32_t window_command(Rect tw)
{
    return GP0_TEXTURE_WINDOW | ((~(tw.w - 1) >> 3) & 0x1Fu) | ((~(tw.h - 1) >> 3) & 0x1Fu) << 5
           | ((tw.x >> 3) & 0x1Fu) << 10 | ((tw.y >> 3) & 0x1Fu) << 15;
}

/* the commands of a drawing environment (as DR_ENV); returns the count */
static int env_commands(uint32_t env, uint32_t out[9])
{
    Rect clip = rect_at(env + DE_CLIP), tw = rect_at(env + DE_TW);
    int n = 0;
    out[n++] = area_start(clip.x, clip.y);
    out[n++] = area_end(clip.x + clip.w - 1, clip.y + clip.h - 1);
    out[n++] = offset_command(s16(env + DE_OFS), s16(env + DE_OFS + 2));
    out[n++] = mode_command(RD8(env + DE_DFE), RD8(env + DE_DTD), RD16(env + DE_TPAGE));
    out[n++] = window_command(tw);
    out[n++] = GP0_MASK;
    if (RD8(env + DE_ISBG)) {
        int w = clip.w, h = clip.h;
        w = w < 0 ? 0 : w > GPU_VRAM_WIDTH - 1 ? GPU_VRAM_WIDTH - 1 : w;
        h = h < 0 ? 0 : h > GPU_VRAM_HEIGHT - 1 ? GPU_VRAM_HEIGHT - 1 : h;
        out[n++] = GP0_FILL | RD8(env + DE_R0) | RD8(env + DE_R0 + 1) << 8
                   | (uint32_t) RD8(env + DE_R0 + 2) << 16;
        out[n++] = (uint16_t) clip.x | (uint32_t) (uint16_t) clip.y << 16;
        out[n++] = (uint32_t) w | (uint32_t) h << 16;
    }
    return n;
}

/* ---- ordering tables ---------------------------------------------------- */

static void draw_list(uint32_t p)
{
    if (g_psx_skip_frames) {
        return;                         /* a frame that will not be shown */
    }
    for (int guard = 0; guard < 0x200000; guard++) {
        uint32_t header = RD32(RT_RAM_BASE | (p & LINK_MASK));
        int words = (int) (header >> 24);
        if (words) {
            const uint32_t *packet = (const uint32_t *) rt_pointer(p + 4);
            if (packet) {
                gpu_gp0_words(packet, words);
            }
        }
        if ((header & LINK_MASK) == LIST_END || (header & 0x800000u)) {
            break;
        }
        p = RT_RAM_BASE | (header & 0x1FFFFFu);
    }
}

/* ---- the library functions ------------------------------------------------ */

void hle_ResetGraph(void)
{
    if (GPR[4] == 0 || GPR[4] == 3) {
        gpu_set_display_enabled(0);
        gpu_set_draw_area(0, 0, GPU_VRAM_WIDTH - 1, GPU_VRAM_HEIGHT - 1);
        gpu_set_draw_offset(0, 0);
        gpu_set_draw_mode(0);
        gpu_set_texture_window(0);
    }
    RETURN(0);
}

void hle_SetGraphDebug(void)
{
    RETURN(0);
}

void hle_SetGraphQueue(void)
{
    RETURN(0);
}

void hle_GetGraphDebug(void)
{
    RETURN(0);
}

void hle_DrawSyncCallback(void)
{
    RETURN(irq_set_draw_callback(GPR[4]));
}

void hle_SetDispMask(void)
{
    gpu_set_display_enabled(GPR[4] != 0);
}

void hle_DrawSync(void)
{
    RETURN(0);                          /* nothing left to draw */
}

static void clear_image(void)
{
    Rect r = rect_at(GPR[4]);
    uint32_t words[3];
    words[0] = GP0_FILL | (GPR[5] & 0xFF) | (GPR[6] & 0xFF) << 8 | (GPR[7] & 0xFF) << 16;
    words[1] = (uint16_t) r.x | (uint32_t) (uint16_t) r.y << 16;
    words[2] = (uint16_t) r.w | (uint32_t) (uint16_t) r.h << 16;
    gpu_gp0_words(words, 3);
    irq_draw_done();
    RETURN(0);
}

void hle_ClearImage(void)
{
    clear_image();
}

void hle_ClearImage2(void)
{
    clear_image();
}

static void load_image(void)
{
    Rect r = rect_at(GPR[4]);
    const void *data = rt_pointer(GPR[5]);
    if (data && r.w > 0 && r.h > 0) {
        gpu_load_image(r.x, r.y, r.w, r.h, (const uint16_t *) data);
    }
    irq_draw_done();
    RETURN(0);
}

void hle_LoadImage(void)
{
    load_image();
}

void hle_LoadImage2(void)
{
    load_image();
}

static void store_image(void)
{
    Rect r = rect_at(GPR[4]);
    void *data = rt_pointer(GPR[5]);
    if (data && r.w > 0 && r.h > 0) {
        gpu_store_image(r.x, r.y, r.w, r.h, (uint16_t *) data);
    }
    RETURN(0);
}

void hle_StoreImage(void)
{
    store_image();
}

void hle_StoreImage2(void)
{
    store_image();
}

static void move_image(void)
{
    Rect r = rect_at(GPR[4]);
    if (r.w > 0 && r.h > 0) {
        gpu_move_image(r.x, r.y, (int16_t) GPR[5], (int16_t) GPR[6], r.w, r.h);
    }
    irq_draw_done();
    RETURN(0);
}

void hle_MoveImage(void)
{
    move_image();
}

void hle_MoveImage2(void)
{
    move_image();
}

/* ot[0] -> ot[1] -> ... -> ot[n-1] -> end */
void hle_ClearOTag(void)
{
    uint32_t ot = GPR[4], n = GPR[5];
    for (uint32_t i = 0; i + 1 < n; i++) {
        WR32(ot + 4 * i, (ot + 4 * (i + 1)) & LINK_MASK);
    }
    if (n) {
        WR32(ot + 4 * (n - 1), LIST_END);
    }
    RETURN(ot);
}

/* ot[n-1] -> ... -> ot[0] -> end (drawn from ot[n-1]) */
void hle_ClearOTagR(void)
{
    uint32_t ot = GPR[4], n = GPR[5];
    for (uint32_t i = n; i-- > 1;) {
        WR32(ot + 4 * i, (ot + 4 * (i - 1)) & LINK_MASK);
    }
    if (n) {
        WR32(ot, LIST_END);
    }
    RETURN(ot);
}

void hle_DrawPrim(void)
{
    uint32_t p = GPR[4];
    int words = (int) (RD32(p) >> 24);
    const uint32_t *packet = (const uint32_t *) rt_pointer(p + 4);
    if (packet && words && !g_psx_skip_frames) {
        gpu_gp0_words(packet, words);
    }
    irq_draw_done();
}

void hle_DrawOTag(void)
{
    draw_list(GPR[4]);
    irq_draw_done();
}

void hle_DrawOTag2(void)
{
    hle_DrawOTag();
}

static void put_draw_env(uint32_t env)
{
    uint32_t words[9];
    int n = env_commands(env, words);
    gpu_gp0_words(words, n);
    for (int i = 0; i < DRAWENV_SIZE; i++) {
        s_drawenv[i] = (uint8_t) RD8(env + i);
    }
}

void hle_PutDrawEnv(void)
{
    put_draw_env(GPR[4]);
    irq_draw_done();
    RETURN(GPR[4]);
}

void hle_DrawOTagEnv(void)
{
    uint32_t ot = GPR[4];
    put_draw_env(GPR[5]);
    draw_list(ot);
    irq_draw_done();
}

void hle_GetDrawEnv(void)
{
    for (int i = 0; i < DRAWENV_SIZE; i++) {
        WR8(GPR[4] + i, s_drawenv[i]);
    }
    RETURN(GPR[4]);
}

void hle_PutDispEnv(void)
{
    uint32_t env = GPR[4];
    Rect disp = rect_at(env + DI_DISP);
    gpu_set_display(disp.x, disp.y, disp.w, disp.h, RD8(env + DI_ISRGB24));
    for (int i = 0; i < DISPENV_SIZE; i++) {
        s_dispenv[i] = (uint8_t) RD8(env + i);
    }
    RETURN(env);
}

void hle_GetDispEnv(void)
{
    for (int i = 0; i < DISPENV_SIZE; i++) {
        WR8(GPR[4] + i, s_dispenv[i]);
    }
    RETURN(GPR[4]);
}

void hle_GetODE(void)
{
    RETURN(irq_vblank_count() & 1);
}

/* DR_AREA: tag + 2 commands */
void hle_SetDrawArea(void)
{
    uint32_t p = GPR[4];
    Rect r = rect_at(GPR[5]);
    WR32(p, (RD32(p) & LINK_MASK) | 2u << 24);
    WR32(p + 4, area_start(r.x, r.y));
    WR32(p + 8, area_end(r.x + r.w - 1, r.y + r.h - 1));
}

/* DR_OFFSET: tag + 2 commands */
void hle_SetDrawOffset(void)
{
    uint32_t p = GPR[4], ofs = GPR[5];
    WR32(p, (RD32(p) & LINK_MASK) | 2u << 24);
    WR32(p + 4, offset_command(s16(ofs), s16(ofs + 2)));
    WR32(p + 8, 0);
}

/* DR_ENV: tag + up to 15 commands */
void hle_SetDrawEnv(void)
{
    uint32_t p = GPR[4], words[9];
    int n = env_commands(GPR[5], words);
    WR32(p, (RD32(p) & LINK_MASK) | (uint32_t) n << 24);
    for (int i = 0; i < n; i++) {
        WR32(p + 4 + 4 * i, words[i]);
    }
}

void hle__GPU_ResetCallback(void)
{
    RETURN(0);
}
