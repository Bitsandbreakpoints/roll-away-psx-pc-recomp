/*
 * platform.h - the Windows side of the PC port: the window, Direct3D 11,
 * frame timing, files dropped on the window.  The PS1 code above it only
 * sees "present a frame and wait for the next vertical blank".
 */
#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_REFRESH_HZ     60      /* NTSC: the game runs one step per frame */
#define PLATFORM_SPLASH_MIN_W   720     /* smallest splash, at 96 DPI */
#define PLATFORM_SPLASH_MIN_H   432

/* PlatformConfig.window_mode */
enum WindowMode {
    WINDOW_MODE_WINDOWED = 0,
    WINDOW_MODE_BORDERLESS = 1,         /* borderless window covering the monitor */
    WINDOW_MODE_SPLASH = 2              /* the launcher: a borderless, centred window of
                                           width x height at 96 DPI (scaled for the
                                           monitor), rounded corners, resizable by its
                                           edges down to PLATFORM_SPLASH_MIN_* */
};

typedef struct {
    int width, height;                  /* client area in windowed mode */
    int window_mode;                    /* enum WindowMode */
    int vsync;
} PlatformConfig;

/* Opens the window and the Direct3D 11 device.  FALSE on failure (the
 * reason has been shown to the user). */
int platform_init(const PlatformConfig *config);
void platform_shutdown(void);

/* Applies a changed window size / mode / vsync. */
void platform_apply_config(const PlatformConfig *config);

/* Handles the window messages.  FALSE once the window has been closed. */
int platform_pump(void);

/* Clears the back buffer (colour as 0xRRGGBB) and makes it the target. */
void platform_clear(uint32_t rgb);

/* Shows the back buffer and waits for the next 1/60 s tick. */
void platform_present(void);

/* Fast forward: platform_present() shows only every 4th frame and does
 * not wait. */
void platform_set_fast_forward(int on);

/* Frames per second over the last second. */
double platform_fps(void);

/* Size of the client area in pixels and the monitor's DPI scale (1.0 = 96 DPI). */
void platform_client_size(int *width, int *height);
float platform_dpi_scale(void);

/* A file dropped on the window since the last call (UTF-8), else FALSE. */
int platform_take_dropped_file(char *path, int size);

/* "Open file" dialog for a disc image.  FALSE if cancelled. */
int platform_open_file_dialog(char *path, int size);

/* The launcher's own window controls (splash mode): moving the window
 * while the left mouse button is held, minimising it. */
void platform_begin_window_drag(void);
void platform_minimize(void);

/* Sets the window title (UTF-8). */
void platform_set_title(const char *title);

/* Shows a message box (UTF-8). */
void platform_message(const char *title, const char *text);

/* Native handles, for the renderer and the UI layer. */
void *platform_hwnd(void);                  /* HWND */
void *platform_d3d_device(void);            /* ID3D11Device * */
void *platform_d3d_context(void);           /* ID3D11DeviceContext * */

/* A hook that sees every window message first (the UI layer); returns
 * nonzero if it handled the message. */
typedef intptr_t (*PlatformMessageHook)(void *hwnd, unsigned msg, uintptr_t wparam, intptr_t lparam);
void platform_set_message_hook(PlatformMessageHook hook);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PLATFORM_H */
