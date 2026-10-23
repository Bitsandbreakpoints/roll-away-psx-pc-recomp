/*
 * editor.h - the level editor: its state, shared by the panels
 * (editor.cpp) and the 3D view (editor_view.cpp).
 */
#ifndef EDITOR_EDITOR_H
#define EDITOR_EDITOR_H

#include <string>
#include <vector>

#include "imgui.h"
#include "editor/level_model.h"
#include "editor/renderer.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

enum Tool {
    TOOL_BUILD = 0,                     /* new blocks against the side clicked */
    TOOL_ERASE,
    TOOL_PAINT,                         /* changes a block's kind */
    TOOL_THING,                         /* puts a thing on the side clicked */
    TOOL_SELECT,
    TOOL_COUNT
};

/* picking a block or side for a reference */
enum LinkPick {
    PICK_NONE = 0,
    PICK_POWER,                         /* a thing's power (w[5]) */
    PICK_TARGET,                        /* a thing's target (w[6]) */
    PICK_LASER_POWER
};

struct Editor {
    Level level;
    std::string path;                   /* the .rlv, "" not saved yet */
    bool dirty = false;

    std::vector<Level> undo, redo;

    int tool = TOOL_BUILD;
    int block_kind = BLOCK_NORMAL;
    int thing_id = THING_COIN;
    int thing_variant = 0;

    int sel_cell = -1, sel_side = SIDE_TOP;
    int link_pick = PICK_NONE;

    /* the build plane (for blocks not against another) */
    bool plane = false;
    int plane_y = 16;

    Camera camera;
    bool show_links = true;
    bool show_icons = true;
    bool show_grid = true;

    std::string message;                /* the status bar's */
    double message_time = 0;

    bool quit = false;
};

extern Editor g_ed;

bool editor_init(ID3D11Device *device, ID3D11DeviceContext *context, void *hwnd);
void editor_shutdown();
/* the UI and the 3D view of one frame (queued for renderer_flush) */
void editor_frame(int width, int height);
/* the window is to close: FALSE if the player keeps it open (unsaved) */
bool editor_close_request();
void editor_open_path(const char *path);

/* editor.cpp: changes */
void editor_snapshot();                 /* before a change: for undo */
void editor_changed();
void editor_say(const char *format, ...);
void editor_world_changed();

/* editor_view.cpp */
void view_frame(int width, int height);
void view_focus();
/* the direction a thing faces: rotation 1..4 on `side`, as a vector */
Vec3 thing_direction(int side, int rotation);
/* the rotation (1..4) on `side` nearest the camera's forward direction */
int rotation_facing_camera(int side);
/* a thing's icon, radius r, alpha 0..255 */
void draw_thing_icon(ImDrawList *dl, ImVec2 centre, float r, int id, int variant, int alpha);
/* the block (or the new block's cell) under the mouse */
bool view_hover(int *cell, int *side);

#endif /* EDITOR_EDITOR_H */
