/*
 * irq.c - interrupts and LIBETC: the vertical blank, VSync(), the
 * callbacks, the root counters, and the sound's share of the time.
 *
 * Nothing interrupts the recompiled code from outside.  Instead it looks
 * for interrupts itself: on every so many backward jumps (RT_POLL, i.e.
 * in loops) and when it waits in VSync().  Then, as the hardware would,
 * a due vertical blank is taken (the frame is shown), and the game's
 * callbacks for the vertical blank, the timers and finished DMA run, with
 * the registers saved around them (rt_invoke()).
 *
 * The sound: the device's thread mixes the SPU while the game's code is
 * not running (it holds s_lock and lets go only while it waits for the
 * vertical blank), see system.h.
 */
#include <windows.h>
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/system.h"
#include "runtime/irq.h"
#include "runtime/memory.h"
#include "platform/threads.h"
#include "psx/spu.h"
#include "psx/log.h"

#define FRAME_US                16683           /* NTSC: 59.94 Hz */
#define HBLANKS_PER_FRAME       263
#define POLL_INTERVAL           256             /* backward jumps between checks */
#define VSYNC_CALLBACKS         8
#define DMA_CHANNELS            8
#define TIMERS                  3
#define MAX_TIMER_EVENTS        8               /* at most this many callbacks at once */

#define SYSCLK                  33868800.0
#define HBLANK_HZ               15734.0

/* SetRCnt() */
#define RCNT_SPEC_BASE          0xF2000000u
#define RCNT_MODE_INTR          0x1000u
#define RCNT_MODE_SC            0x0001u

int g_rt_poll_count = POLL_INTERVAL;
int g_psx_skip_frames;

static LARGE_INTEGER s_freq, s_start;
static int s_vblank_count, s_last_vsync_count;
static uint64_t s_last_vblank_us, s_last_vsync_us;
static int s_in_callback;
static int s_callbacks_on;
static int s_vblank_pending;            /* a vertical blank whose callbacks wait */
static uint32_t s_dma_pending;          /* finished channels whose callbacks wait */
static int s_draw_pending;
static uint32_t s_vsync_callback, s_vsync_callbacks[VSYNC_CALLBACKS];
static uint32_t s_irq_callbacks[IRQ_COUNT];
static uint32_t s_dma_callbacks[DMA_CHANNELS];
static uint32_t s_draw_callback;
static uint32_t s_intr_mask;

typedef struct {
    uint32_t mode, target;
    int interrupts;
    uint64_t base_us;                   /* when the count was 0 */
    uint64_t fired;                     /* targets reached and reported */
} Timer;

static Timer s_timers[TIMERS];

/* ---- sound -------------------------------------------------------------- */

static ThreadsLock *s_lock;
static int s_lock_depth;                /* (the game's thread) */
static int s_sound_running, s_sound_realtime;
static PsxAudioSink s_audio_sink;
static int s_master_volume = 100, s_music_volume = 100, s_effects_volume = 100;

/* the SPU's CD audio input (registers from 1F801C00h) */
#define SPU_REG_CONTROL         0x1AA
#define SPU_CNT_CD_AUDIO        0x0001
#define SPU_REG_CD_VOLUME_LEFT  0x1B0
#define SPU_REG_CD_VOLUME_RIGHT 0x1B2
#define SPU_VOLUME_MAX          0x8000
#define CD_VOLUME_ONE           256     /* cd_mix_audio(): as decoded */

void cd_tick(void);
void cd_mix_audio(int16_t *out, int frames, int volume_left, int volume_right);

static void game_lock(void)
{
    if (!s_lock) {
        s_lock = threads_lock_create();
    }
    threads_lock(s_lock);
    s_lock_depth++;
}

static void game_unlock(void)
{
    if (s_lock_depth > 0) {
        s_lock_depth--;
        threads_unlock(s_lock);
    }
}

void psx_set_sound_realtime(int on)
{
    s_sound_realtime = on;
}

void psx_set_audio_sink(PsxAudioSink sink)
{
    s_audio_sink = sink;
}

void psx_set_sound_volumes(int master, int music, int effects)
{
    s_master_volume = master;
    s_music_volume = music;             /* the XA music from the CD */
    s_effects_volume = effects;         /* everything the SPU plays */
}

static void scale_samples(int16_t *samples, int count, int volume)
{
    if (volume >= 100) {
        return;
    }
    for (int i = 0; i < count; i++) {
        samples[i] = (int16_t) (samples[i] * volume / 100);
    }
}

/* the SPU's voices and the CD's XA sound (at the SPU's CD volume, if the
 * SPU takes CD audio in), then the master volume */
static void render_sound(int16_t *out, int frames)
{
    spu_render(out, frames);
    scale_samples(out, frames * 2, s_effects_volume);
    if (spu_read16(SPU_REG_CONTROL) & SPU_CNT_CD_AUDIO) {
        int left = (int16_t) spu_read16(SPU_REG_CD_VOLUME_LEFT);
        int right = (int16_t) spu_read16(SPU_REG_CD_VOLUME_RIGHT);
        cd_mix_audio(out, frames, left * CD_VOLUME_ONE / SPU_VOLUME_MAX * s_music_volume / 100,
                     right * CD_VOLUME_ONE / SPU_VOLUME_MAX * s_music_volume / 100);
    }
    scale_samples(out, frames * 2, s_master_volume);
}

void psx_sound_frame(int16_t out[PSX_SOUND_FRAME_SAMPLES * 2])
{
    if (!s_lock) {
        memset(out, 0, PSX_SOUND_FRAME_SAMPLES * 2 * sizeof(int16_t));
        return;
    }
    threads_lock(s_lock);
    if (s_sound_running) {
        render_sound(out, PSX_SOUND_FRAME_SAMPLES);
    } else {
        memset(out, 0, PSX_SOUND_FRAME_SAMPLES * 2 * sizeof(int16_t));
    }
    threads_unlock(s_lock);
}

/* ---- time --------------------------------------------------------------- */

uint64_t irq_time_us(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t) ((now.QuadPart - s_start.QuadPart) * 1000000 / s_freq.QuadPart);
}

int irq_vblank_count(void)
{
    return s_vblank_count;
}

static int hblanks_since(uint64_t us)
{
    return (int) ((irq_time_us() - us) * HBLANKS_PER_FRAME / FRAME_US);
}

/* ---- the devices ---------------------------------------------------------- */

void irq_reset(void)
{
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_start);
    s_vblank_count = s_last_vsync_count = 0;
    s_last_vblank_us = s_last_vsync_us = 0;
    s_in_callback = 0;
    s_callbacks_on = 1;
    s_vblank_pending = 0;
    s_dma_pending = 0;
    s_draw_pending = 0;
    s_vsync_callback = 0;
    memset(s_vsync_callbacks, 0, sizeof(s_vsync_callbacks));
    memset(s_irq_callbacks, 0, sizeof(s_irq_callbacks));
    memset(s_dma_callbacks, 0, sizeof(s_dma_callbacks));
    s_draw_callback = 0;
    s_intr_mask = 0;
    memset(s_timers, 0, sizeof(s_timers));
    g_rt_poll_count = POLL_INTERVAL;
}

void hle_reset_all(void);

void psx_devices_reset(void)
{
    extern int g_spu_trace;
    g_spu_trace = GetEnvironmentVariableA("RA_SPU_TRACE", NULL, 0) != 0;
    game_lock();                        /* (held while the game's code runs) */
    s_sound_running = 1;
    io_reset();
    spu_reset();
    irq_reset();
    pad_reset();
    hle_reset_all();
}

void psx_devices_stop(void)
{
    while (s_lock_depth > 0) {
        game_unlock();
    }
    threads_lock(s_lock);
    s_sound_running = 0;
    threads_unlock(s_lock);
}

/* ---- timers (root counters) ------------------------------------------- */

static double timer_hz(int t)
{
    const Timer *tm = &s_timers[t];
    if (t == 1 && (tm->mode & RCNT_MODE_SC)) {
        return HBLANK_HZ;
    }
    if (t == 2 && (tm->mode & RCNT_MODE_SC)) {
        return SYSCLK / 8;
    }
    return SYSCLK;
}

static uint64_t timer_ticks(int t)
{
    return (uint64_t) ((irq_time_us() - s_timers[t].base_us) * timer_hz(t) / 1e6);
}

uint32_t irq_timer_value(int timer)
{
    if (timer < 0 || timer >= TIMERS) {
        return 0;
    }
    uint64_t ticks = timer_ticks(timer);
    uint32_t wrap = s_timers[timer].target ? s_timers[timer].target : 0x10000;
    return (uint32_t) (ticks % wrap);
}

void irq_timer_write(int timer, int reg, uint32_t value)
{
    if (timer < 0 || timer >= TIMERS) {
        return;
    }
    Timer *t = &s_timers[timer];
    if (reg == 0 || reg == 1) {
        t->base_us = irq_time_us();
        t->fired = 0;
    }
    if (reg == 2) {
        t->target = value & 0xFFFF;
    }
}

/* ---- callbacks ---------------------------------------------------------- */

static int can_interrupt(void)
{
    return !s_in_callback && s_callbacks_on && rt_interrupts_enabled();
}

static void callback(uint32_t func, uint32_t arg)
{
    if (!func) {
        return;
    }
    s_in_callback++;
    rt_invoke(func, arg);
    s_in_callback--;
}

/* whatever is due and can run now */
static void service(void)
{
    if (!can_interrupt()) {
        return;
    }
    if (s_vblank_pending) {
        s_vblank_pending = 0;
        callback(s_irq_callbacks[IRQ_VBLANK], 0);
        for (int i = 0; i < VSYNC_CALLBACKS; i++) {
            callback(s_vsync_callbacks[i], 0);
        }
        callback(s_vsync_callback, 0);
    }
    while (s_dma_pending) {
        int ch = 0;
        while (!(s_dma_pending & (1u << ch))) {
            ch++;
        }
        s_dma_pending &= ~(1u << ch);
        callback(s_dma_callbacks[ch], 0);
    }
    if (s_draw_pending) {
        s_draw_pending = 0;
        callback(s_draw_callback, 0);
    }
    for (int t = 0; t < TIMERS; t++) {
        Timer *tm = &s_timers[t];
        uint32_t func = s_irq_callbacks[IRQ_TIMER0 + t];
        if (!tm->interrupts || !func || !tm->target) {
            continue;
        }
        uint64_t reached = timer_ticks(t) / tm->target;
        if (reached > tm->fired + MAX_TIMER_EVENTS) {
            tm->fired = reached - MAX_TIMER_EVENTS;     /* (too far behind: drop some) */
        }
        while (tm->fired < reached) {
            tm->fired++;
            callback(func, 0);
        }
    }
}

void irq_dma_done(int channel)
{
    if (channel >= 0 && channel < DMA_CHANNELS && s_dma_callbacks[channel]) {
        s_dma_pending |= 1u << channel;
        g_rt_poll_count = 0;
    }
}

void irq_draw_done(void)
{
    if (s_draw_callback) {
        s_draw_pending = 1;
        g_rt_poll_count = 0;
    }
}

uint32_t irq_set_draw_callback(uint32_t func)
{
    uint32_t old = s_draw_callback;
    s_draw_callback = func;
    return old;
}

/* ---- the vertical blank ---------------------------------------------------- */

void irq_vblank(void)
{
    /* the PC shows the frame and waits for the 60 Hz tick: the sound may
     * run meanwhile (the hook may also leave the game, see psx_vblank()) */
    game_unlock();
    psx_vblank();
    game_lock();
    s_last_vblank_us = irq_time_us();
    s_vblank_count++;
    pad_update();
    cd_tick();
    if (!s_sound_realtime && s_audio_sink && !g_psx_skip_frames) {
        int16_t frame[PSX_SOUND_FRAME_SAMPLES * 2];
        render_sound(frame, PSX_SOUND_FRAME_SAMPLES);
        s_audio_sink(frame, PSX_SOUND_FRAME_SAMPLES);
    }
    s_vblank_pending = 1;
    service();
}

void irq_idle(void)
{
    game_unlock();
    Sleep(1);
    game_lock();
    g_rt_poll_count = 0;
}

void rt_poll(void)
{
    g_rt_poll_count = POLL_INTERVAL;
    if (!can_interrupt()) {
        return;
    }
    /* the game is busy (or waits in a loop): a vertical blank when it is due */
    if (irq_time_us() - s_last_vblank_us >= FRAME_US) {
        irq_vblank();
    } else {
        service();
    }
}

/* ---- LIBETC: VSync and the callbacks ---------------------------------------- */

void hle_VSync(void)
{
    int32_t mode = (int32_t) GPR[4];
    if (mode < 0) {
        RETURN(s_vblank_count);
        return;
    }
    if (mode == 1) {
        RETURN(hblanks_since(s_last_vblank_us));
        return;
    }
    int target = mode == 0 ? s_vblank_count + 1 : s_last_vsync_count + mode;
    while (s_vblank_count < target) {
        irq_vblank();
    }
    service();
    RETURN(hblanks_since(s_last_vsync_us));
    s_last_vsync_count = s_vblank_count;
    s_last_vsync_us = irq_time_us();
}

static uint32_t swap(uint32_t *slot, uint32_t value)
{
    uint32_t old = *slot;
    *slot = value;
    return old;
}

void hle_VSyncCallback(void)
{
    RETURN(swap(&s_vsync_callback, GPR[4]));
}

void hle_VSyncCallbacks(void)
{
    RETURN(GPR[4] < VSYNC_CALLBACKS ? swap(&s_vsync_callbacks[GPR[4]], GPR[5]) : 0);
}

void hle_InterruptCallback(void)
{
    RETURN(GPR[4] < IRQ_COUNT ? swap(&s_irq_callbacks[GPR[4]], GPR[5]) : 0);
}

void hle_DMACallback(void)
{
    RETURN(GPR[4] < DMA_CHANNELS ? swap(&s_dma_callbacks[GPR[4]], GPR[5]) : 0);
}

void hle_ResetCallback(void)
{
    s_vsync_callback = 0;
    memset(s_vsync_callbacks, 0, sizeof(s_vsync_callbacks));
    memset(s_irq_callbacks, 0, sizeof(s_irq_callbacks));
    memset(s_dma_callbacks, 0, sizeof(s_dma_callbacks));
    s_callbacks_on = 1;
    RETURN(0);
}

void hle_StopCallback(void)
{
    s_callbacks_on = 0;
    RETURN(0);
}

void hle_RestartCallback(void)
{
    s_callbacks_on = 1;
    RETURN(0);
}

void hle_CheckCallback(void)
{
    RETURN(s_in_callback > 0);
}

void hle_GetIntrMask(void)
{
    RETURN(s_intr_mask);
}

void hle_SetIntrMask(void)
{
    RETURN(swap(&s_intr_mask, GPR[4]));
}

/* ---- root counters (LIBAPI COUNTER.OBJ) -------------------------------------- */

static int rcnt_index(uint32_t spec)
{
    uint32_t n = spec - RCNT_SPEC_BASE;
    return n < TIMERS ? (int) n : -1;
}

void hle_SetRCnt(void)
{
    int t = rcnt_index(GPR[4]);
    if (t < 0) {
        RETURN(0);
        return;
    }
    s_timers[t].target = GPR[5] & 0xFFFF;
    s_timers[t].mode = GPR[6];
    s_timers[t].base_us = irq_time_us();
    s_timers[t].fired = 0;
    s_timers[t].interrupts = (GPR[6] & RCNT_MODE_INTR) != 0;
    psx_log("SetRCnt(%d, %u, %04X)", t, GPR[5] & 0xFFFF, GPR[6]);
    RETURN(1);
}

void hle_GetRCnt(void)
{
    int t = rcnt_index(GPR[4]);
    RETURN(t < 0 ? 0 : irq_timer_value(t));
}

void hle_StartRCnt(void)
{
    int t = rcnt_index(GPR[4]);
    if (t >= 0) {
        s_timers[t].interrupts = 1;
    }
    RETURN(t >= 0);
}

void hle_StopRCnt(void)
{
    int t = rcnt_index(GPR[4]);
    if (t >= 0) {
        s_timers[t].interrupts = 0;
    }
    RETURN(t >= 0);
}

void hle_ResetRCnt(void)
{
    int t = rcnt_index(GPR[4]);
    if (t >= 0) {
        s_timers[t].base_us = irq_time_us();
        s_timers[t].fired = 0;
    }
    RETURN(t >= 0);
}
