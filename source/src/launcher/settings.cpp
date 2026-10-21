/*
 * settings.cpp - settings.ini: a plain "key = value" file with [sections],
 * written in full on every save.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher/settings.h"
#include "platform/platform.h"

Settings g_settings_pc;

/* names of the PS1 buttons and pad inputs in the file */
static const char *BUTTON_KEYS[PSX_BUTTON_COUNT] = {
    "up", "down", "left", "right", "cross", "circle", "square", "triangle",
    "l1", "r1", "l2", "r2", "start", "select",
};
static const char *PAD_TOKENS[PAD_INPUT_COUNT] = {
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "A", "B", "X", "Y",
    "LB", "RB", "LT", "RT", "START", "BACK", "L3", "R3",
    "LSTICK_UP", "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT",
};

void settings_default_keys(ControllerConfig *c, int player)
{
    /* player 1: arrows, X C Z S, Enter, Backspace, Q E 1 3;
     * player 2: I K J L, numeric keypad */
    static const int P1[PSX_BUTTON_COUNT] = {
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'X', 'C', 'Z', 'S',
        'Q', 'E', '1', '3', VK_RETURN, VK_BACK,
    };
    static const int P2[PSX_BUTTON_COUNT] = {
        'I', 'K', 'J', 'L', VK_NUMPAD2, VK_NUMPAD6, VK_NUMPAD4, VK_NUMPAD8,
        VK_NUMPAD7, VK_NUMPAD9, VK_NUMPAD1, VK_NUMPAD3, VK_NUMPAD5, VK_NUMPAD0,
    };
    memcpy(c->keys, player == 0 ? P1 : P2, sizeof(c->keys));
}

void settings_default_pad(ControllerConfig *c)
{
    static const int PAD[PSX_BUTTON_COUNT] = {
        PAD_DPAD_UP, PAD_DPAD_DOWN, PAD_DPAD_LEFT, PAD_DPAD_RIGHT,
        PAD_A, PAD_B, PAD_X, PAD_Y,
        PAD_LB, PAD_RB, PAD_LT, PAD_RT,
        PAD_START, PAD_BACK,
    };
    memcpy(c->pad, PAD, sizeof(c->pad));
    c->stick_as_dpad = 1;
}

void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    s->launcher_width = 1000;
    s->launcher_height = 600;
    s->window_width = 1280;
    s->window_height = 960;
    s->window_mode = WINDOW_MODE_WINDOWED;
    s->vsync = 1;
    s->internal_scale = 4;
    s->aspect = ASPECT_4_3;
    s->video_filter = 0;
    s->precise_vertices = 1;
    s->texture_filtering = 1;
    s->volume_master = 100;
    s->volume_music = 100;
    s->volume_effects = 100;
    for (int p = 0; p < SETTINGS_PLAYERS; p++) {
        s->players[p].device = p == 0 ? DEVICE_AUTO : DEVICE_NONE;
        settings_default_keys(&s->players[p], p);
        settings_default_pad(&s->players[p]);
    }
}

/* settings.ini next to the exe */
static void ini_path(wchar_t *out)
{
    GetModuleFileNameW(NULL, out, MAX_PATH);
    wchar_t *slash = wcsrchr(out, L'\\');
    wcscpy(slash ? slash + 1 : out, L"settings.ini");
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int pad_token(const char *v)
{
    for (int i = 0; i < PAD_INPUT_COUNT; i++) {
        if (_stricmp(v, PAD_TOKENS[i]) == 0) {
            return i;
        }
    }
    return PAD_NONE;
}

static void set_value(Settings *s, const char *section, const char *key, const char *v)
{
    int n = atoi(v);
    if (strcmp(section, "disc") == 0) {
        if (strcmp(key, "path") == 0) snprintf(s->disc_path, sizeof(s->disc_path), "%s", v);
    } else if (strcmp(section, "graphics") == 0) {
        if (strcmp(key, "window_width") == 0) s->window_width = clampi(n, 320, 16384);
        else if (strcmp(key, "window_height") == 0) s->window_height = clampi(n, 240, 16384);
        else if (strcmp(key, "window_mode") == 0) s->window_mode = clampi(n, 0, 1);
        else if (strcmp(key, "vsync") == 0) s->vsync = n != 0;
        else if (strcmp(key, "internal_scale") == 0) s->internal_scale = clampi(n, 1, HIRES_MAX_SCALE);
        else if (strcmp(key, "aspect") == 0) s->aspect = clampi(n, 0, 2);
        else if (strcmp(key, "smooth_filtering") == 0) s->video_filter = n != 0;   /* (older) */
        else if (strcmp(key, "video_filter") == 0) s->video_filter = clampi(n, 0, VIDEO_FILTER_COUNT - 1);
        else if (strcmp(key, "precise_vertices") == 0) s->precise_vertices = n != 0;
        else if (strcmp(key, "true_color") == 0) s->true_color = n != 0;
        else if (strcmp(key, "texture_filtering") == 0) s->texture_filtering = n != 0;
    } else if (strcmp(section, "sound") == 0) {
        if (strcmp(key, "master") == 0) s->volume_master = clampi(n, 0, 100);
        else if (strcmp(key, "music") == 0) s->volume_music = clampi(n, 0, 100);
        else if (strcmp(key, "effects") == 0) s->volume_effects = clampi(n, 0, 100);
    } else if (strncmp(section, "player", 6) == 0) {
        int p = atoi(section + 6) - 1;
        if (p < 0 || p >= SETTINGS_PLAYERS) {
            return;
        }
        ControllerConfig *c = &s->players[p];
        if (strcmp(key, "device") == 0) {
            c->device = clampi(n, DEVICE_NONE, DEVICE_AUTO);
        } else if (strcmp(key, "stick_as_dpad") == 0) {
            c->stick_as_dpad = n != 0;
        } else {
            for (int b = 0; b < PSX_BUTTON_COUNT; b++) {
                char name[32];
                snprintf(name, sizeof(name), "key_%s", BUTTON_KEYS[b]);
                if (strcmp(key, name) == 0) {
                    c->keys[b] = clampi(n, 0, 255);
                }
                snprintf(name, sizeof(name), "pad_%s", BUTTON_KEYS[b]);
                if (strcmp(key, name) == 0) {
                    c->pad[b] = pad_token(v);
                }
            }
        }
    } else if (strcmp(section, "launcher") == 0) {
        if (strcmp(key, "width") == 0) s->launcher_width = n;
        else if (strcmp(key, "height") == 0) s->launcher_height = n;
    }
}

static char *trim(char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return p;
}

void settings_load(void)
{
    wchar_t path[MAX_PATH];
    char line[2048], section[64] = "";
    FILE *f;

    settings_defaults(&g_settings_pc);
    ini_path(path);
    f = _wfopen(path, L"rb");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if ((unsigned char) p[0] == 0xEF && (unsigned char) p[1] == 0xBB && (unsigned char) p[2] == 0xBF) {
            p += 3;                     /* UTF-8 byte order mark (e.g. saved by Notepad) */
        }
        p = trim(p);
        if (*p == ';' || *p == '#' || *p == '\0') {
            continue;
        }
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", p + 1);
            }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        set_value(&g_settings_pc, section, trim(p), trim(eq + 1));
    }
    fclose(f);
}

int settings_save(void)
{
    wchar_t path[MAX_PATH];
    const Settings *s = &g_settings_pc;
    FILE *f;

    ini_path(path);
    f = _wfopen(path, L"wb");
    if (!f) {
        return 0;
    }
    fprintf(f, "; Roll Away PC - settings (written by the launcher)\n\n");
    fprintf(f, "[disc]\npath = %s\n\n", s->disc_path);
    fprintf(f, "[graphics]\nwindow_width = %d\nwindow_height = %d\n", s->window_width, s->window_height);
    fprintf(f, "; 0 = window, 1 = borderless full screen\nwindow_mode = %d\n", s->window_mode);
    fprintf(f, "vsync = %d\n; 1..%d times 320x240\ninternal_scale = %d\n", s->vsync, HIRES_MAX_SCALE,
            s->internal_scale);
    fprintf(f, "; 0 = 4:3, 1 = stretch, 2 = 16:9 widescreen\naspect = %d\n", s->aspect);
    fprintf(f, "; 0 = sharp, 1 = bilinear, 2 = CRT, 3 = soft TV, 4 = soft CRT, 5 = composite CRT, 6 = PVM\nvideo_filter = %d\nprecise_vertices = %d\ntexture_filtering = %d\n; 1 = 8 bits per colour, no dithering\ntrue_color = %d\n\n",
            s->video_filter, s->precise_vertices, s->texture_filtering, s->true_color);
    fprintf(f, "[sound]\nmaster = %d\nmusic = %d\neffects = %d\n\n", s->volume_master,
            s->volume_music, s->volume_effects);
    for (int p = 0; p < SETTINGS_PLAYERS; p++) {
        const ControllerConfig *c = &s->players[p];
        fprintf(f, "[player%d]\n; -1 none, 0 keyboard, 1-4 XInput controller, 5 automatic\n", p + 1);
        fprintf(f, "device = %d\nstick_as_dpad = %d\n; keys: Windows virtual key codes\n",
                c->device, c->stick_as_dpad);
        for (int b = 0; b < PSX_BUTTON_COUNT; b++) {
            fprintf(f, "key_%s = %d\n", BUTTON_KEYS[b], c->keys[b]);
        }
        for (int b = 0; b < PSX_BUTTON_COUNT; b++) {
            fprintf(f, "pad_%s = %s\n", BUTTON_KEYS[b],
                    c->pad[b] >= 0 && c->pad[b] < PAD_INPUT_COUNT ? PAD_TOKENS[c->pad[b]] : "NONE");
        }
        fprintf(f, "\n");
    }
    fprintf(f, "[launcher]\n; its window as it was left (at 96 DPI)\nwidth = %d\nheight = %d\n",
            s->launcher_width, s->launcher_height);
    fclose(f);
    return 1;
}
