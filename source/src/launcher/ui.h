/*
 * ui.h - Dear ImGui on top of the platform layer (launcher and settings).
 */
#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#include <stdint.h>
#include "imgui.h"

/* An image in a Direct3D 11 texture (with mipmaps). */
struct UiImage {
    void *view;                     /* ID3D11ShaderResourceView */
    int width, height;
};

void ui_init(void);
void ui_shutdown(void);
void ui_begin_frame(void);
void ui_end_frame(void);            /* draws into the back buffer */

/* Segoe UI Bold (titles, menu buttons); the normal font if missing. */
ImFont *ui_font_bold(void);

/* Bahnschrift (Windows 10+: titles, the launcher's menu); else the bold font. */
ImFont *ui_font_display(void);

/* Decodes a JPEG built into the exe (resources.rc). */
int ui_image_from_resource(int resource_id, UiImage *image);
void ui_image_free(UiImage *image);

/* Draws `image` so that it covers p0..p1 (cropped, not stretched), keeping
 * the point focus (0..1 of the image) as central as possible.  Uses
 * trilinear filtering, so a large photo stays smooth when shrunk. */
void ui_draw_image_cover(ImDrawList *draw, const UiImage *image, ImVec2 p0, ImVec2 p1,
                         ImVec2 focus);

/* ui_draw_video() filters (the setting's values) */
enum UiVideoFilter {
    UI_VIDEO_SHARP = 0,                 /* the pixels as they are */
    UI_VIDEO_BILINEAR = 1,              /* smooth scaling */
    UI_VIDEO_CRT = 2,                   /* scanlines, mask, curvature */
    UI_VIDEO_TV = 3                     /* soft TV signal (GTU-like) */
};

/* The game's picture: a texture updated every frame from 0xAABBGGRR
 * pixels, drawn into p0..p1 with a filter (enum UiVideoFilter); `width`
 * and `lines` are its size in the console's pixels (what the CRT and TV
 * filters work in). */
void ui_video_update(const uint32_t *rgba, int w, int h);
void ui_draw_video(ImDrawList *draw, ImVec2 p0, ImVec2 p1, int filter, int width, int lines);

#endif /* LAUNCHER_UI_H */
