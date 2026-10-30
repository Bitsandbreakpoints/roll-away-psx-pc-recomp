/*
 * settings.h - the player's settings, kept in settings.ini next to the exe.
 */
#ifndef LAUNCHER_SETTINGS_H
#define LAUNCHER_SETTINGS_H

#include "platform/input.h"
#include "psx/gpu_hires.h"          /* HIRES_MAX_SCALE */

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_PATH_MAX       1024
#define SETTINGS_PLAYERS        2

/* ControllerConfig.device */
enum InputDevice {
    DEVICE_NONE = -1,
    DEVICE_KEYBOARD = 0,
    DEVICE_PAD_1 = 1,               /* XInput controllers 1..4 */
    DEVICE_PAD_4 = 4,
    DEVICE_AUTO = 5                 /* the first XInput controller free, else the keyboard */
};

/* Settings.video_filter: 0..VIDEO_FILTER_COUNT - 1 (enum UiVideoFilter, ui.h) */
#define VIDEO_FILTER_COUNT      7

/* Settings.aspect */
enum AspectMode {
    ASPECT_4_3 = 0,
    ASPECT_STRETCH = 1,
    ASPECT_WIDESCREEN = 2           /* 16:9, the 3D squeezed by the GTE (gte_set_widescreen) */
};

/* Settings.renderer: who draws the high-resolution copy (psx/gpu_hires.h) */
enum Renderer {
    RENDERER_SOFTWARE = 0,          /* the processor, on all cores */
    RENDERER_GPU = 1,               /* the graphics card (Direct3D 11) */
    RENDERER_COUNT
};

typedef struct {
    int device;                         /* enum InputDevice */
    int keys[PSX_BUTTON_COUNT];         /* virtual key codes */
    int pad[PSX_BUTTON_COUNT];          /* enum PadInput */
    int stick_as_dpad;                  /* the left stick also moves */
} ControllerConfig;

typedef struct {
    char disc_path[SETTINGS_PATH_MAX];

    /* the launcher's window, at 96 DPI (as it was left) */
    int launcher_width, launcher_height;

    /* graphics */
    int window_width, window_height;
    int window_mode;                    /* enum WindowMode */
    int vsync;
    int renderer;                       /* enum Renderer: who draws the internal resolution */
    int internal_scale;                 /* 1..HIRES_MAX_SCALE times 320x240 */
    int aspect;                         /* enum AspectMode */
    int true_color;                     /* 8 bits per colour, no dithering */
    int detailed_textures;              /* distant surfaces with their full-size textures */
    int video_filter;                   /* enum UiVideoFilter (launcher/ui.h) */
    int filter_strength;                /* 0..100: how much of the TV/CRT filter shows */
    int precise_vertices;               /* no PS1 "wobble" */
    int texture_filtering;              /* bilinear, 3D surfaces only */
    int anti_aliasing;                  /* FXAA on the picture (launcher/ui.cpp) */

    /* sound, 0..100 */
    int volume_master;
    int volume_music;                   /* the XA music from the CD */
    int volume_effects;                 /* everything the SPU plays */

    int save_every_level;               /* the save screen after every level (else every fifth) */
    int timeless_arcade;                /* Arcade without the time limit or the time power-ups */
    int pal_60hz;                       /* Kula World (Europe, PAL) at 60 Hz, as the others */

    ControllerConfig players[SETTINGS_PLAYERS];
} Settings;

extern Settings g_settings_pc;

void settings_defaults(Settings *s);
void settings_default_keys(ControllerConfig *c, int player);
void settings_default_pad(ControllerConfig *c);

/* Loads settings.ini (defaults for anything missing). */
void settings_load(void);
/* Writes settings.ini; FALSE on failure. */
int settings_save(void);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_SETTINGS_H */
