/*
 * main_pc.cpp - entry point of the Roll Away PC port.
 *
 * The program starts in the launcher (choose the disc, settings).  "Play"
 * runs the game: the recompiled PS1 program (src/runtime/) boots from the
 * player's disc and hands control back once per frame, at the vertical
 * blank, to game_frame() below, which shows the PS1 display area in the
 * window.
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgui.h"
#include "platform/platform.h"
#include "platform/input.h"
#include "launcher/settings.h"
#include "launcher/launcher.h"
#include "launcher/ui.h"
#include "launcher/controls.h"
#include "platform/audio.h"
#include "disc/disc.h"
#include "runtime/system.h"
#include "runtime/kanji.h"
#include "runtime/custom_level.h"
#include "runtime/runtime.h"
#include "platform/font.h"
#include "psx/gte.h"
#include "psx/gpu.h"
#include "psx/log.h"

#define BACKGROUND              0x0A1020
#define GAME_BACKGROUND         0x000000
#define PSX_ASPECT              (4.0f / 3.0f)   /* the TV picture */
#define WIDE_ASPECT             (16.0f / 9.0f)  /* the widescreen mode */
#define HINT_FRAMES             (60 * 4)        /* "Esc" hint at the start */
#define PAD_SELECT_START        0x0009          /* PS1 buttons that open the settings */
#define SETTINGS_WIDTH          760.0f          /* in-game settings, before DPI scaling */
#define SETTINGS_HEIGHT         560.0f
#define SETTINGS_DIM            IM_COL32(0, 0, 0, 150)
#define GAME_TITLE              "Roll Away"

static int s_quit;                      /* the window was closed */
static int s_frame;                     /* frames of the running game */

/* A crash: the address and the return addresses on the stack go to the
 * log (llvm-symbolizer turns them into source lines). */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *info)
{
    const CONTEXT *c = info->ContextRecord;
    psx_log("CRASH: exception %08lx at %08lx (access %08lx)",
            (unsigned long) info->ExceptionRecord->ExceptionCode, (unsigned long) c->Eip,
            info->ExceptionRecord->NumberParameters > 1
                ? (unsigned long) info->ExceptionRecord->ExceptionInformation[1] : 0ul);
    psx_log("CRASH: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx esp %08lx ebp %08lx",
            c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp);
    const DWORD *frame = (const DWORD *) c->Ebp;
    for (int i = 0; i < 24 && frame && !IsBadReadPtr(frame, 8); i++) {
        psx_log("CRASH: called from %08lx", frame[1]);
        frame = (const DWORD *) frame[0];
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * Debugging and tests: RA_INPUT=<frame>:<button>[,...] presses a button of
 * controller 1 for a few frames at the given game frame, or holds it with
 * <first>-<last>:<button>, e.g. "300:start,420:cross,900-1500:left" (names:
 * up down left right cross circle square triangle l1 r1 l2 r2 start
 * select).  See also RA_VRAM_DUMP, RA_WAV_DUMP, RA_MUTE, RA_AUTOSTART,
 * RA_NATIVE and RA_LEAVE below, and tests/.
 */
#define SCRIPT_PRESS_FRAMES     6

typedef struct {
    int first, last, bits;
} ScriptedPress;

static int scripted_buttons(void)
{
    static const struct { const char *name; int bit; } BUTTONS[] = {
        { "select", 0x0001 }, { "start", 0x0008 }, { "up", 0x0010 }, { "right", 0x0020 },
        { "down", 0x0040 }, { "left", 0x0080 }, { "l2", 0x0100 }, { "r2", 0x0200 },
        { "l1", 0x0400 }, { "r1", 0x0800 }, { "triangle", 0x1000 }, { "circle", 0x2000 },
        { "cross", 0x4000 }, { "square", 0x8000 },
    };
    static ScriptedPress *presses;
    static int count = -1;
    if (count < 0) {
        /* (parsed once) */
        static char script[262144];
        count = 0;
        DWORD n = GetEnvironmentVariableA("RA_INPUT", script, sizeof(script));
        if (n == 0 || n >= sizeof(script)) {
            return 0;
        }
        presses = (ScriptedPress *) calloc(n / 4 + 1, sizeof(ScriptedPress));
        for (char *p = script; *p && presses;) {
            int frame = atoi(p), last = frame + SCRIPT_PRESS_FRAMES - 1;
            char *dash = strchr(p, '-');
            char *name = strchr(p, ':');
            if (dash && name && dash < name) {
                last = atoi(dash + 1);
            }
            char *end = strchr(p, ',');
            if (!name || (end && name > end)) {
                break;
            }
            name++;
            int length = end ? (int) (end - name) : (int) strlen(name);
            for (const auto &b : BUTTONS) {
                if ((int) strlen(b.name) == length && _strnicmp(b.name, name, length) == 0) {
                    presses[count].first = frame;
                    presses[count].last = last;
                    presses[count].bits = b.bit;
                    count++;
                }
            }
            p = end ? end + 1 : name + length;
        }
    }
    int buttons = 0;
    for (int i = 0; i < count; i++) {
        if (s_frame >= presses[i].first && s_frame <= presses[i].last) {
            buttons |= presses[i].bits;
        }
    }
    return buttons;
}

/* (tests: RA_POKE=<frame>:<address>=<value>[,...] writes a word of PS1
 * memory at that game frame, address and value in hex) */
extern "C" void *rt_pointer(uint32_t address);

static void scripted_pokes(void)
{
    static char script[16384];
    DWORD n = GetEnvironmentVariableA("RA_POKE", script, sizeof(script));
    if (n == 0 || n >= sizeof(script)) {
        return;
    }
    for (const char *p = script; p && *p;) {
        unsigned frame, address, value;
        if (sscanf(p, "%u:%x=%x", &frame, &address, &value) == 3 && (int) frame == s_frame) {
            uint32_t *word = (uint32_t *) rt_pointer(address);
            if (word) {
                *word = value;
                psx_log("poke %08X = %08X (frame %d)", address, value, s_frame);
            }
        }
        p = strchr(p, ',');
        p = p ? p + 1 : NULL;
    }
}

/* A level of the player's: the confirm button (X, KulaQuest O) is pressed
 * for 3 frames of every 20 until the game loads the level - through the
 * intro, the title and the menus straight into an Arcade game. */
static int custom_level_buttons(void)
{
    static int scripted = -1;
    if (scripted < 0) {
        scripted = GetEnvironmentVariableA("RA_INPUT", NULL, 0) != 0;
    }
    if (scripted || !custom_level_active() || custom_level_started() || s_frame % 20 >= 3) {
        return 0;
    }
    bool japan = g_rt_module && !strcmp(g_rt_module->disc_name, "SCPS_100.64");
    return japan ? 0x2000 : 0x4000;
}

/* A controller's buttons: the settings, plus RA_INPUT for player 1. */
static int read_pad(int port)
{
    int buttons = controls_psx_buttons(port);
    if (port == 0) {
        int scripted = scripted_buttons() | custom_level_buttons();
        if (scripted) {
            buttons = (buttons < 0 ? 0 : buttons) | scripted;
        }
    }
    return buttons;
}

/* Debugging: RA_WAV_DUMP=<file> records the game's sound (16-bit stereo WAV). */
static FILE *s_wav;
static uint32_t s_wav_bytes;

static void wav_header(FILE *f, uint32_t data_bytes)
{
    const uint32_t rate = 44100, byte_rate = rate * 4;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    uint32_t riff = 36 + data_bytes, fmt_size = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&pcm, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

static int s_fast_forward;              /* Tab held: as fast as it goes, silent */

/* real time (the sound device's thread) */
static void sound_stream(int16_t *samples, int frames)
{
    psx_sound_frame(samples);
    if (s_fast_forward) {
        memset(samples, 0, (size_t) frames * 2 * sizeof(int16_t));
    }
}

static void sound_sink(const int16_t *samples, int frames)
{
    if (!s_fast_forward) {
        audio_submit(samples, frames);
    }
    if (s_wav) {
        fwrite(samples, 4, (size_t) frames, s_wav);
        s_wav_bytes += (uint32_t) frames * 4;
    }
}

/* the displayed picture (internal resolution) as a 24-bit BMP */
static void dump_picture(const char *path)
{
    int w, h;
    const uint32_t *picture = gpu_display_rgba(&w, &h);
    FILE *f = picture ? fopen(path, "wb") : NULL;
    if (!f) {
        return;
    }
    uint8_t header[54] = { 'B', 'M' };
    int stride = (w * 3 + 3) & ~3;
    uint32_t size = 54 + (uint32_t) (stride * h);
    int32_t bw = w, bh = -h;            /* top-down */
    memcpy(header + 2, &size, 4);
    header[10] = 54;
    header[14] = 40;
    memcpy(header + 18, &bw, 4);
    memcpy(header + 22, &bh, 4);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (int y = 0; y < h; y++) {
        uint8_t row[4096 * 3 + 4] = { 0 };
        for (int x = 0; x < w && x < 4096; x++) {
            uint32_t p = picture[y * w + x];
            row[x * 3] = (uint8_t) (p >> 16);
            row[x * 3 + 1] = (uint8_t) (p >> 8);
            row[x * 3 + 2] = (uint8_t) p;
        }
        fwrite(row, 1, (size_t) stride, f);
    }
    fclose(f);
}

/* Debugging: RA_VRAM_DUMP=<frame>[,<frame>...] writes vram_<frame>.bmp
 * and the displayed picture as shot_<frame>.bmp. */
static void debug_dump_vram(void)
{
    static char frames[4096];
    char name[64];
    if (!GetEnvironmentVariableA("RA_VRAM_DUMP", frames, sizeof(frames))) {
        return;
    }
    for (char *p = frames; *p;) {
        if (atoi(p) == s_frame) {
            snprintf(name, sizeof(name), "vram_%d.bmp", s_frame);
            gpu_debug_dump(name);
            snprintf(name, sizeof(name), "shot_%d.bmp", s_frame);
            dump_picture(name);
            /* (RA_RAM_DUMP=1: the 2 MB of PS1 RAM too, ram_<frame>.bin) */
            if (GetEnvironmentVariableA("RA_RAM_DUMP", NULL, 0)) {
                snprintf(name, sizeof(name), "ram_%d.bin", s_frame);
                FILE *f = fopen(name, "wb");
                if (f) {
                    fwrite(rt_pointer(0x80000000u), 1, 0x200000, f);
                    fclose(f);
                }
            }
        }
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
}

/* The PS1 picture in the window: 4:3 (or 16:9 in widescreen mode; the
 * movies stay 4:3) with black bars, or stretched. */
static void draw_picture(int w, int h)
{
    int pw, ph;
    const uint32_t *picture = gpu_display_rgba(&pw, &ph);
    if (!picture) {
        return;                         /* display off: black */
    }
    ui_video_update(picture, pw, ph);
    float x0 = 0, y0 = 0, x1 = (float) w, y1 = (float) h;
    if (g_settings_pc.aspect != ASPECT_STRETCH) {
        float aspect = g_settings_pc.aspect == ASPECT_WIDESCREEN && !gpu_display_is_24bit()
                       && !GetEnvironmentVariableA("RA_NATIVE", NULL, 0) ? WIDE_ASPECT : PSX_ASPECT;
        if (w > h * aspect) {
            float width = h * aspect;
            x0 = (w - width) / 2;
            x1 = x0 + width;
        } else {
            float height = w / aspect;
            y0 = (h - height) / 2;
            y1 = y0 + height;
        }
    }
    ui_draw_video(ImGui::GetBackgroundDrawList(), ImVec2(x0, y0), ImVec2(x1, y1),
                  g_settings_pc.video_filter, g_settings_pc.filter_strength, gpu_display_width(),
                  gpu_display_lines());
    if (!gpu_display_wide()) {
        /* (widescreen, a picture of only 2D: squeezed into the middle 3/4) */
        float bar = (x1 - x0) / 8;
        ImDrawList *draw = ImGui::GetBackgroundDrawList();
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bar, y1), IM_COL32(0, 0, 0, 255));
        draw->AddRectFilled(ImVec2(x1 - bar, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 255));
    }
}

extern "C" int g_pgxp_debug;           /* (psx/gpu.c: RA_PGXP_DEBUG) */
extern "C" int g_save_every_level;     /* (runtime/hooks.c) */
extern "C" int g_timeless_arcade;      /* (runtime/timeless.c) */
extern "C" void psx_set_pal_60hz(int on);   /* (runtime/runtime.c) */
extern "C" int g_prim_trace[6];
extern "C" int g_gte_trace[6];         /* (psx/gte.c: RA_GTE_TRACE) */        /* (psx/gpu.c: RA_PRIM_TRACE) */

/* The settings that take effect at once while the game runs. */
static void apply_game_settings(void)
{
    /* (tests: RA_NATIVE=1 draws like the console, for comparisons) */
    if (GetEnvironmentVariableA("RA_NATIVE", NULL, 0)) {
        gpu_set_true_color(0);
        gpu_set_texture_lod(0);
        gpu_set_enhancements(1, 0, 0);
        gte_set_widescreen(0);
        gpu_set_widescreen(0);
    } else {
        gte_set_widescreen(g_settings_pc.aspect == ASPECT_WIDESCREEN);
        gpu_set_true_color(g_settings_pc.true_color);
        gpu_set_texture_lod(g_settings_pc.detailed_textures);
        g_pgxp_debug = GetEnvironmentVariableA("RA_PGXP_DEBUG", NULL, 0) != 0;
        char gtrace[64];
        if (GetEnvironmentVariableA("RA_GTE_TRACE", gtrace, sizeof(gtrace))) {
            sscanf(gtrace, "%d,%d,%d,%d,%d,%d", &g_gte_trace[0], &g_gte_trace[1], &g_gte_trace[2],
                   &g_gte_trace[3], &g_gte_trace[4], &g_gte_trace[5]);
        }
        char trace[64];
        if (GetEnvironmentVariableA("RA_PRIM_TRACE", trace, sizeof(trace))) {
            sscanf(trace, "%d,%d,%d,%d,%d,%d", &g_prim_trace[0], &g_prim_trace[1], &g_prim_trace[2],
                   &g_prim_trace[3], &g_prim_trace[4], &g_prim_trace[5]);
        }
        gpu_set_widescreen(g_settings_pc.aspect == ASPECT_WIDESCREEN);
        gpu_set_enhancements(g_settings_pc.internal_scale, g_settings_pc.precise_vertices,
                             g_settings_pc.texture_filtering);
    }
    psx_set_sound_volumes(g_settings_pc.volume_master, g_settings_pc.volume_music,
                          g_settings_pc.volume_effects);
    g_save_every_level = g_settings_pc.save_every_level;
    g_timeless_arcade = g_settings_pc.timeless_arcade;
    psx_set_pal_60hz(g_settings_pc.pal_60hz);
}

/* F1 or Select+Start on controller 1 was pressed (not held) */
static int settings_key_pressed(void)
{
    static int was_down;
    int buttons = read_pad(0);
    int down = input_key_down(VK_F1)
               || (buttons > 0 && (buttons & PAD_SELECT_START) == PAD_SELECT_START);
    int pressed = down && !was_down;
    was_down = down;
    return pressed;
}

/*
 * The settings over the paused game (the game waits inside its vertical
 * blank until they are closed with Esc, F1, Select+Start or "Close").
 * FALSE if the window was closed meanwhile.
 */
enum { PAUSE_RESUME, PAUSE_LAUNCHER, PAUSE_QUIT };

/* the buttons at the top of the pause window; returns a PAUSE_* choice */
static int pause_buttons(int first)
{
    int choice = PAUSE_RESUME, chosen = 0;
    float scale = platform_dpi_scale();
    ImVec2 size(170 * scale, 0);
    if (ImGui::Button("Resume", size)) {
        chosen = 1;
    }
    if (first) {
        ImGui::SetItemDefaultFocus();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Back to launcher", size)) {
        choice = PAUSE_LAUNCHER;
        chosen = 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit game", size)) {
        choice = PAUSE_QUIT;
        chosen = 1;
    }
    ImGui::Separator();
    return chosen ? choice : -1;
}

static int game_settings(void)
{
    int open = 1, first = 1;
    int esc_was_down = 1;
    int choice = PAUSE_RESUME;

    while (open) {
        int w, h;
        if (!platform_pump()) {
            s_quit = 1;
            return 0;
        }
        input_poll();
        int esc = input_key_down(VK_ESCAPE);
        if ((esc && !esc_was_down && !settings_capturing()) || settings_key_pressed()) {
            open = 0;
        }
        esc_was_down = esc;
        platform_clear(GAME_BACKGROUND);
        ui_begin_frame();
        platform_client_size(&w, &h);
        draw_picture(w, h);
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float) w, (float) h),
                                                      SETTINGS_DIM);
        float scale = platform_dpi_scale();
        ImVec2 size(SETTINGS_WIDTH * scale, SETTINGS_HEIGHT * scale);
        size.x = size.x > w - 32.0f ? w - 32.0f : size.x;
        size.y = size.y > h - 32.0f ? h - 32.0f : size.y;
        ImGui::SetNextWindowPos(ImVec2((w - size.x) / 2, (h - size.y) / 2));
        ImGui::SetNextWindowSize(size);
        int first_frame = first;
        if (first) {
            ImGui::SetNextWindowFocus();
            first = 0;
        }
        ImGui::Begin("Paused", NULL,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
                         | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        int pressed = pause_buttons(first_frame);
        if (pressed >= 0) {
            choice = pressed;
            open = 0;
        }
        if (!settings_screen()) {
            open = 0;
        }
        ImGui::End();
        ui_end_frame();
        platform_present();
        apply_game_settings();
    }
    settings_save();
    if (choice == PAUSE_QUIT) {
        s_quit = 1;                     /* closes the program */
        return 0;
    }
    if (choice == PAUSE_LAUNCHER) {
        return 0;                       /* stops the game */
    }
    /* Esc must not reach the game (it would leave for the launcher) */
    while (input_key_down(VK_ESCAPE)) {
        if (!platform_pump()) {
            s_quit = 1;
            return 0;
        }
        input_poll();
        platform_present();
    }
    return 1;
}

/* One frame of the game (at the vertical blank); FALSE stops the game. */
static int game_frame(void)
{
    int w, h;

    platform_set_refresh_hz(psx_video_hz());    /* (PAL: 50 Hz) */

    if (!platform_pump()) {
        s_quit = 1;
        return 0;
    }
    input_poll();
    /* fast forward while Tab is held: three of four frames are not shown
     * (nor drawn in high resolution) and nothing waits */
    s_fast_forward = input_key_down(VK_TAB) && GetForegroundWindow() == (HWND) platform_hwnd();
    platform_set_fast_forward(s_fast_forward);
    if (s_fast_forward && (s_frame & 3) != 3) {
        gpu_skip_frame();
        ++s_frame;
        return 1;
    }
    platform_clear(GAME_BACKGROUND);
    ui_begin_frame();
    platform_client_size(&w, &h);
    draw_picture(w, h);
    debug_dump_vram();
    if (s_fast_forward) {
        ImGui::GetForegroundDrawList()->AddText(ImVec2(12, 10), IM_COL32(255, 255, 255, 210),
                                                ">>  Fast forward (Tab)");
    } else if (s_frame < HINT_FRAMES) {
        ImGui::GetForegroundDrawList()->AddText(
            ImVec2(12, 10), IM_COL32(255, 255, 255, 150),
            "Esc: back to the launcher    F1 / Select+Start: settings    Tab: fast forward");
    }
    ++s_frame;
    scripted_pokes();
    ui_end_frame();
    platform_present();
    if (settings_key_pressed() && !game_settings()) {
        return 0;
    }
    /* (tests: RA_LEAVE=<frame> goes back to the launcher there, as Esc) */
    char leave[16];
    if (GetEnvironmentVariableA("RA_LEAVE", leave, sizeof(leave)) && s_frame >= atoi(leave)) {
        return 0;
    }
    return !input_key_down(VK_ESCAPE);
}

/* the launcher's window: a splash of this size at 96 DPI */
#define SPLASH_WIDTH            1000
#define SPLASH_HEIGHT           600

static void show_splash(void)
{
    PlatformConfig config = {};
    config.width = g_settings_pc.launcher_width;
    config.height = g_settings_pc.launcher_height;
    config.window_mode = WINDOW_MODE_SPLASH;
    config.vsync = 1;
    platform_apply_config(&config);
}

/* keeps the launcher window's size (at 96 DPI) for next time */
static void remember_splash_size(void)
{
    int w, h;
    float scale = platform_dpi_scale();
    platform_client_size(&w, &h);
    if (w > 0 && h > 0) {
        g_settings_pc.launcher_width = (int) (w / scale + 0.5f);
        g_settings_pc.launcher_height = (int) (h / scale + 0.5f);
    }
}

/* the game's window: the display settings */
static void show_game_window(void)
{
    PlatformConfig config = {};
    config.width = g_settings_pc.window_width;
    config.height = g_settings_pc.window_height;
    config.window_mode = g_settings_pc.window_mode;
    config.vsync = g_settings_pc.vsync;
    platform_apply_config(&config);
}

static void run_game(void)
{
    /* (tests: RA_DISC=<image> plays that disc, the settings left as they are) */
    static char test_disc[MAX_PATH];
    const char *disc = GetEnvironmentVariableA("RA_DISC", test_disc, sizeof(test_disc))
                       ? test_disc : g_settings_pc.disc_path;
    if (!disc_open(disc)) {
        platform_message(GAME_TITLE, "The disc cannot be read.");
        return;
    }
    custom_level_prepare();
    settings_save();
    launcher_set_in_game(1);
    show_game_window();
    s_frame = 0;
    char wav_path[MAX_PATH];
    if (GetEnvironmentVariableA("RA_WAV_DUMP", wav_path, sizeof(wav_path))) {
        s_wav = fopen(wav_path, "wb");
        s_wav_bytes = 0;
        if (s_wav) {
            wav_header(s_wav, 0);
        }
    }
    /* (RA_MUTE=1: no sound device, e.g. for recording tests) */
    int mute = GetEnvironmentVariableA("RA_MUTE", NULL, 0) != 0;
    int sound = !mute && audio_init();
    if (!sound && !mute) {
        psx_log("no sound device (XAudio2)");
    }
    psx_set_audio_sink(sound || s_wav ? sound_sink : NULL);
    /* the sound in real time on its own thread; in step with the frames
     * when it is recorded (RA_WAV_DUMP) */
    psx_set_sound_realtime(sound && !s_wav
                           && audio_stream_start(sound_stream, PSX_SOUND_FRAME_SAMPLES));
    apply_game_settings();
    DWORD start = GetTickCount();
    psx_run_game(game_frame);
    double seconds = (GetTickCount() - start) / 1000.0;
    psx_log("%d frames in %.1f s (%.1f fps)", s_frame, seconds,
            seconds > 0 ? s_frame / seconds : 0.0);
    audio_stream_stop();
    audio_stop();
    if (s_wav) {
        wav_header(s_wav, s_wav_bytes);
        fclose(s_wav);
        s_wav = NULL;
    }
    const char *fatal = psx_fatal_message();
    if (fatal && !s_quit && !GetEnvironmentVariableA("RA_AUTOSTART", NULL, 0)) {
        char text[700];
        snprintf(text, sizeof(text), "The game stopped:\n\n%s\n\n(See roll_away.log.)", fatal);
        platform_message(GAME_TITLE, text);
    }
    /* wait for Esc to be released, or the launcher sees it too */
    while (!s_quit && input_key_down(VK_ESCAPE) && platform_pump()) {
        input_poll();
        platform_present();
    }
    launcher_set_in_game(0);
    if (!s_quit) {
        show_splash();
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    PlatformConfig config = {};

    SetUnhandledExceptionFilter(crash_handler);
    /* first, before DLLs could take its addresses */
    if (!psx_system_init()) {
        MessageBoxW(NULL, L"The PS1 memory (0x80000000) could not be reserved.", L"Roll Away",
                    MB_ICONERROR);
        return 1;
    }
    settings_load();
    if (g_settings_pc.launcher_width < PLATFORM_SPLASH_MIN_W
        || g_settings_pc.launcher_height < PLATFORM_SPLASH_MIN_H) {
        g_settings_pc.launcher_width = SPLASH_WIDTH;
        g_settings_pc.launcher_height = SPLASH_HEIGHT;
    }
    config.width = g_settings_pc.launcher_width;
    config.height = g_settings_pc.launcher_height;
    config.window_mode = WINDOW_MODE_SPLASH;
    config.vsync = 1;
    if (!platform_init(&config)) {
        return 1;
    }
    ui_init();
    launcher_init();
    psx_set_pad_reader(read_pad);
    kanji_set_renderer(font_render_sjis);   /* (the dialogue's font) */
    /* `roll_away.exe --level <file.rlv>` (the level editor's "Play"): that
     * level at once, and the program closes when the game is left;
     * (tests: RA_LEVEL=<file.rlv> plays it too) */
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char level_path[MAX_PATH * 3] = "";
    int level_only = 0;
    for (int i = 1; argv && i + 1 < argc; i++) {
        if (!wcscmp(argv[i], L"--level")) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, level_path, sizeof(level_path),
                                NULL, NULL);
            level_only = 1;
        }
    }
    if (argv) {
        LocalFree(argv);
    }
    int from_test = !level_path[0]
                    && GetEnvironmentVariableA("RA_LEVEL", level_path, sizeof(level_path));
    if (level_path[0]) {
        char error[256];
        if (!custom_level_set(level_path, error, sizeof(error))) {
            /* (tests: only logged, no box waits for a click) */
            psx_log("%s: %s", level_path, error);
            if (!from_test) {
                platform_message(GAME_TITLE, error);
            }
            level_only = 0;
        }
    }
    if (level_only) {
        if (g_settings_pc.disc_path[0]) {
            run_game();
        } else {
            platform_message(GAME_TITLE, "Choose your Roll Away disc in the launcher first.");
        }
        s_quit = 1;
    }
    /* debugging: RA_AUTOSTART=1 starts the game right away */
    if (GetEnvironmentVariableA("RA_AUTOSTART", NULL, 0) && g_settings_pc.disc_path[0]) {
        run_game();
        /* (tests: RA_QUIT=1 closes the program when the game is left) */
        if (GetEnvironmentVariableA("RA_QUIT", NULL, 0)) {
            s_quit = 1;
        }
    }

    while (!s_quit && platform_pump()) {
        input_poll();
        platform_clear(BACKGROUND);
        ui_begin_frame();
        int action = launcher_frame();
        ui_end_frame();
        platform_present();
        if (!IsIconic((HWND) platform_hwnd())) {
            remember_splash_size();     /* (saved with the settings) */
        }
        if (action == LAUNCHER_PLAY) {
            settings_save();
            run_game();
        } else if (action == LAUNCHER_PLAY_LEVEL) {
            settings_save();
            run_game();
            custom_level_set(NULL, NULL, 0);    /* (PLAY: the game's own levels again) */
        } else if (action == LAUNCHER_QUIT) {
            break;
        }
    }
    settings_save();
    audio_shutdown();
    launcher_shutdown();
    ui_shutdown();
    platform_shutdown();
    return 0;
}
