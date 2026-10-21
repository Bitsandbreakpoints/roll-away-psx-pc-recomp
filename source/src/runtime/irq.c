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
 * The sound: the SPU is mixed in step with the game, one frame of sound at
 * every vertical blank, as the console plays it while the game runs (the
 * game's sound driver checks its voices' envelopes from frame to frame
 * and must see them move on by a frame each time).  The device's thread
 * plays it from a small queue (s_lock guards both, see system.h).
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
#include "psx/gte.h"
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

/* the queue between the frames' sound and the device's thread: a few
 * frames are kept; each frame gets a sample or two more or less to keep it
 * there (the console's 59.94 Hz against the PC's 44.1 kHz clock) */
#define QUEUE_SAMPLES           (PSX_SOUND_FRAME_SAMPLES * 16)
#define QUEUE_TARGET            (PSX_SOUND_FRAME_SAMPLES * 3)
#define SOUND_CHUNK             1024
#define SOUND_CHUNK_MIN         64      /* (not less at once) */
#define SOUND_CATCH_UP_MAX      (PSX_SOUND_FRAME_SAMPLES * 4)
static uint64_t s_sound_start_us;
static int64_t s_sound_samples;         /* mixed since s_sound_start_us */
static int16_t s_queue[QUEUE_SAMPLES][2];
static int s_queue_read, s_queue_count;
static int s_queue_short;               /* device frames the queue could not fill (the log) */
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
    memset(out, 0, PSX_SOUND_FRAME_SAMPLES * 2 * sizeof(int16_t));
    if (!s_lock) {
        return;
    }
    threads_lock(s_lock);
    int n = s_queue_count < PSX_SOUND_FRAME_SAMPLES ? s_queue_count : PSX_SOUND_FRAME_SAMPLES;
    if (s_sound_running && n < PSX_SOUND_FRAME_SAMPLES) {
        s_queue_short++;
    }
    for (int i = 0; i < n; i++) {
        out[i * 2] = s_queue[s_queue_read][0];
        out[i * 2 + 1] = s_queue[s_queue_read][1];
        s_queue_read = (s_queue_read + 1) % QUEUE_SAMPLES;
    }
    s_queue_count -= n;
    threads_unlock(s_lock);
}

/* Adds n samples of sound to the queue (the game's thread holds s_lock). */
static void queue_sound(const int16_t *samples, int n)
{
    for (int i = 0; i < n; i++) {
        if (s_queue_count == QUEUE_SAMPLES) {
            s_queue_read = (s_queue_read + 1) % QUEUE_SAMPLES;     /* (drop the oldest) */
            s_queue_count--;
        }
        int at = (s_queue_read + s_queue_count) % QUEUE_SAMPLES;
        s_queue[at][0] = samples[i * 2];
        s_queue[at][1] = samples[i * 2 + 1];
        s_queue_count++;
    }
}

/* Mixes the sound due by now, in real time as the SPU plays it while the
 * game runs, with a sample or two more or less to keep the queue at its
 * level (the PC's clock against the sound device's). */
static void sound_update(void)
{
    int16_t chunk[SOUND_CHUNK * 2];
    uint64_t now = irq_time_us();
    int64_t due = (int64_t) ((now - s_sound_start_us) * SPU_SAMPLE_RATE / 1000000) - s_sound_samples;
    if (due > SOUND_CATCH_UP_MAX) {
        /* (the game stood still, e.g. paused: go on from here) */
        s_sound_samples += due - SOUND_CATCH_UP_MAX;
        due = SOUND_CATCH_UP_MAX;
    }
    if (due < SOUND_CHUNK_MIN) {
        return;
    }
    s_sound_samples += due;
    int n = (int) due + (s_queue_count < QUEUE_TARGET ? 2
                         : s_queue_count > QUEUE_TARGET + PSX_SOUND_FRAME_SAMPLES ? -2 : 0);
    while (n > 0) {
        int part = n < SOUND_CHUNK ? n : SOUND_CHUNK;
        render_sound(chunk, part);
        if (!g_psx_skip_frames) {
            queue_sound(chunk, part);   /* (fast forward: the sound goes on, unheard) */
        }
        n -= part;
    }
}

/* the sound at the vertical blank */
static void sound_vblank(void)
{
    if (s_sound_realtime) {
        sound_update();
        return;
    }
    /* (recorded or silent: exactly a frame each time, the same every run) */
    int16_t frame[PSX_SOUND_FRAME_SAMPLES * 2];
    render_sound(frame, PSX_SOUND_FRAME_SAMPLES);
    if (s_audio_sink && !g_psx_skip_frames) {
        s_audio_sink(frame, PSX_SOUND_FRAME_SAMPLES);
    }
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
    s_queue_read = s_queue_count = 0;
    io_reset();
    spu_reset();
    irq_reset();
    s_sound_start_us = irq_time_us();
    s_sound_samples = 0;
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
    s_queue_read = s_queue_count = 0;
    extern int g_pgxp_stats[3], g_pgxp_age_stats[8];
    psx_log("pgxp ages: %d %d %d %d %d %d %d %d+", g_pgxp_age_stats[0], g_pgxp_age_stats[1],
            g_pgxp_age_stats[2], g_pgxp_age_stats[3], g_pgxp_age_stats[4], g_pgxp_age_stats[5],
            g_pgxp_age_stats[6], g_pgxp_age_stats[7]);
    psx_log("pgxp: %d vertices, %d precise, %d projected by the GTE", g_pgxp_stats[0],
            g_pgxp_stats[1], g_pgxp_stats[2]);
    extern int g_spu_unheard, g_xa_underruns;
    psx_log("sound: %d device frames not filled, %d voices stopped unheard, XA ran dry %d times",
            s_queue_short, g_spu_unheard, g_xa_underruns);
    g_spu_unheard = g_xa_underruns = 0;
    s_queue_short = 0;
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
    gte_new_frame();
    pad_update();
    cd_tick();
    sound_vblank();
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
    if (s_sound_realtime) {
        sound_update();
    }
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
