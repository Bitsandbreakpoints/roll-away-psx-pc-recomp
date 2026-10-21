/*
 * launcher.cpp - the launcher and the settings (Dear ImGui).
 *
 * The launcher is a small borderless splash window (WINDOW_MODE_SPLASH),
 * whatever the game's display settings: the photo, the title, a menu of
 * text items (mouse, keyboard or controller), the disc's state and the
 * button hints.  "Settings" turns it into a settings page in the same
 * style.  The settings rows are shared with the in-game settings window
 * (settings_screen(), F1).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include "imgui.h"
#include "launcher/launcher.h"
#include "launcher/settings.h"
#include "launcher/controls.h"
#include "platform/platform.h"
#include "platform/input.h"
#include "disc/disc.h"
#include "launcher/ui.h"
#include "resources.h"

/* window size presets (client area) */
static const struct { int w, h; } WINDOW_SIZES[] = {
    { 960, 720 }, { 1280, 960 }, { 1440, 1080 }, { 1920, 1440 },
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
};

/* colours */
#define COL_ACCENT              IM_COL32(255, 168, 64, 255)
#define COL_ACCENT_SOFT         IM_COL32(255, 168, 64, 52)
#define COL_ACCENT_NONE         IM_COL32(255, 168, 64, 0)
#define COL_TEXT                IM_COL32(244, 248, 245, 255)
#define COL_TEXT_DIM            IM_COL32(244, 248, 245, 150)
#define COL_TEXT_FAINT          IM_COL32(244, 248, 245, 95)
#define COL_TEXT_DISABLED       IM_COL32(244, 248, 245, 60)
#define COL_SHADE               IM_COL32(4, 12, 32, 235)
#define COL_SHADE_NONE          IM_COL32(4, 12, 32, 0)
#define COL_PANEL               IM_COL32(8, 18, 44, 200)
#define COL_KEY                 IM_COL32(244, 248, 245, 120)

static const ImVec4 ACCENT_V4(1.0f, 0.77f, 0.36f, 1.0f);

static int s_disc_status = DISC_NONE;
static char s_disc_detail[256] = "No disc chosen.";
static int s_in_game;                   /* the game runs: display settings apply at once */
static int s_in_launcher;               /* inside launcher_frame(): sizes follow the window */
static float s_scale = 1.0f;            /* ... by this (pixels per pixel at 96 DPI) */

/* pixels per layout pixel: the launcher scales with its window, the
 * in-game settings with the monitor's DPI */
static float ui_scale(void)
{
    return s_in_launcher ? s_scale : platform_dpi_scale();
}

/* "press a button" capture */
static int s_capture_player = -1;
static int s_capture_button;
static int s_capture_pad;               /* TRUE: a controller input, else a key */

#define CHECK_SIZE              0.75f   /* a checkbox's side, of a row's height */

/* ImGui::Checkbox for an int setting: a little smaller than the other
 * controls, centred in a row of the usual height */
static bool check(const char *label, int *value)
{
    bool b = *value != 0;
    float frame = ImGui::GetFrameHeight(), pad = ImGui::GetStyle().FramePadding.y;
    float small = (frame * CHECK_SIZE - ImGui::GetFontSize()) / 2;
    small = small < 0 ? 0 : small;
    float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(top + pad - small);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(small, small));
    bool changed = ImGui::Checkbox(label, &b);
    ImGui::PopStyleVar();
    /* (the row keeps its full height; a note after it lines up with the label) */
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosY(top);
    ImGui::Dummy(ImVec2(0, frame));
    *value = b;
    return changed;
}

static int disc_playable(void)
{
    return s_disc_status == DISC_OK;   /* (the code is recompiled from that version) */
}

static void choose_disc(const char *path)
{
    s_disc_status = disc_check(path, s_disc_detail, sizeof(s_disc_detail));
    if (disc_playable()) {
        snprintf(g_settings_pc.disc_path, sizeof(g_settings_pc.disc_path), "%s", path);
        settings_save();
    }
}

void launcher_set_in_game(int in_game)
{
    s_in_game = in_game;
}

/* The game's display settings; the launcher keeps its splash window. */
static void apply_window_settings(void)
{
    if (!s_in_game) {
        return;
    }
    PlatformConfig config = {};
    config.width = g_settings_pc.window_width;
    config.height = g_settings_pc.window_height;
    config.window_mode = g_settings_pc.window_mode;
    config.vsync = g_settings_pc.vsync;
    platform_apply_config(&config);
}

/* ---- settings rows ---------------------------------------------------- */

#define ROW_LABEL_WIDTH         190.0f  /* before DPI scaling */

/* a settings row: the label, then the control filling the rest */
static void row(const char *label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ROW_LABEL_WIDTH * ui_scale());
    ImGui::SetNextItemWidth(-FLT_MIN);
}

/* a group of rows: its name, small and green, and a thin line */
static void section(const char *title)
{
    ImGui::Dummy(ImVec2(1, 4 * ui_scale()));
    ImGui::PushFont(ui_font_display(), ImGui::GetFontSize() * 0.9f);
    ImGui::TextColored(ACCENT_V4, "%s", title);
    ImGui::PopFont();
    ImGui::Separator();
}

/* a note after a row's control */
static void note(const char *text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("%s", text);
}

static void help(const char *text)
{
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

/* ---- settings tabs -------------------------------------------------- */

static void graphics_tab(void)
{
    Settings *s = &g_settings_pc;
    const char *modes[] = { "Window", "Fullscreen (borderless)" };
    const char *aspects[] = { "4:3 (original)", "Stretched", "16:9 widescreen" };
    const char *filters[] = { "Sharp pixels", "Bilinear (smooth scaling)", "CRT (tube TV)",
                              "Soft TV (GTU-like)", "Soft CRT (TV with glow)",
                              "Composite CRT (colour bleed)", "PVM (sharp scanlines)" };
    char current[64], format[64];

    section("DISPLAY");
    row("Display mode");
    if (ImGui::Combo("##mode", &s->window_mode, modes, 2)) {
        apply_window_settings();
    }
    snprintf(current, sizeof(current), "%d × %d", s->window_width, s->window_height);
    ImGui::BeginDisabled(s->window_mode != WINDOW_MODE_WINDOWED);
    row("Window size");
    if (ImGui::BeginCombo("##size", current)) {
        for (const auto &size : WINDOW_SIZES) {
            char label[64];
            snprintf(label, sizeof(label), "%d × %d%s", size.w, size.h,
                     size.w * 3 == size.h * 4 ? "  (4:3)" : "  (16:9)");
            if (ImGui::Selectable(label, s->window_width == size.w && s->window_height == size.h)) {
                s->window_width = size.w;
                s->window_height = size.h;
                apply_window_settings();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    row("Vsync");
    if (check("##vsync", &s->vsync)) {
        apply_window_settings();
    }

    section("PICTURE");
    snprintf(format, sizeof(format), "%%d×    (%d × %d)", 320 * s->internal_scale,
             240 * s->internal_scale);
    row("Internal resolution");
    ImGui::SliderInt("##scale", &s->internal_scale, 1, HIRES_MAX_SCALE, format);
    row("Aspect ratio");
    ImGui::Combo("##aspect", &s->aspect, aspects, 3);
    if (s->aspect == ASPECT_WIDESCREEN) {
        note("3D seen wider, menus and HUD as in 4:3; the movies stay 4:3");
    }
    row("Picture filter");
    ImGui::Combo("##filter", &s->video_filter, filters, UI_VIDEO_COUNT);

    section("3D");
    row("Precise vertices");
    check("##precise", &s->precise_vertices);
    note("no PS1 \"wobble\", perspective-correct textures");
    ImGui::BeginDisabled(!s->precise_vertices);
    row("Smooth 3D textures");
    check("##textures", &s->texture_filtering);
    note("3D surfaces only; 2D stays sharp");
    ImGui::EndDisabled();
    row("True colour");
    check("##truecolor", &s->true_color);
    note("smooth shading without the console's dithering (8 bits per colour)");
    row("Detailed distant textures");
    check("##lodtextures", &s->detailed_textures);
    note("distant blocks with their full-size textures, not the game's small copies");
    help("The internal resolution is drawn in software on all cores; high values need a fast "
         "processor. Everything here can be changed during the game with F1.");
}

static const char *device_name(int device)
{
    static char name[64];
    if (device == DEVICE_NONE) return "None";
    if (device == DEVICE_KEYBOARD) return "Keyboard";
    if (device == DEVICE_AUTO) return "Automatic";
    snprintf(name, sizeof(name), "Controller %d%s", device,
             input_pad_connected(device - DEVICE_PAD_1) ? "  (connected)" : "");
    return name;
}

/* Assigns `value` to button b; a button that had it already gets b's old one. */
static void assign(int *table, int b, int value)
{
    for (int i = 0; i < PSX_BUTTON_COUNT; i++) {
        if (i != b && table[i] == value) {
            table[i] = table[b];
        }
    }
    table[b] = value;
}

static void poll_capture(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (s_capture_player < 0) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        return;
    }
    /* the key pressed must not also move through the menu */
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    ControllerConfig *c = &g_settings_pc.players[s_capture_player];
    if (s_capture_pad) {
        int pad = controls_player_pad(s_capture_player);
        if (input_new_key() == VK_ESCAPE) {
            s_capture_player = -1;
            return;
        }
        int in = pad >= 0 ? input_new_pad_input(pad) : PAD_NONE;
        if (in != PAD_NONE) {
            assign(c->pad, s_capture_button, in);
            s_capture_player = -1;
        }
    } else {
        int vk = input_new_key();
        if (vk == VK_ESCAPE) {
            s_capture_player = -1;
        } else if (vk) {
            assign(c->keys, s_capture_button, vk);
            s_capture_player = -1;
        }
    }
}

static void controls_player(int p)
{
    ControllerConfig *c = &g_settings_pc.players[p];
    int pad = controls_player_pad(p);

    row("Device");
    if (ImGui::BeginCombo("##device", device_name(c->device))) {
        for (int d = DEVICE_NONE; d <= DEVICE_AUTO; d++) {
            if (ImGui::Selectable(device_name(d), c->device == d)) {
                c->device = d;
            }
        }
        ImGui::EndCombo();
    }
    if (pad >= 0) {
        ImGui::TextColored(ACCENT_V4, "Controller %d connected – the mapping below is used.", pad + 1);
    } else if (c->device != DEVICE_NONE && c->device != DEVICE_KEYBOARD) {
        ImGui::TextDisabled("No controller connected%s.", p == 0 ? " – the keyboard is used" : "");
    }

    if (ImGui::BeginTable("map", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("PS1 button", ImGuiTableColumnFlags_WidthFixed, 110 * ui_scale());
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Controller (XInput)");
        ImGui::TableHeadersRow();
        for (int b = 0; b < PSX_BUTTON_COUNT; b++) {
            char label[96];
            ImGui::PushID(b);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input_psx_button_name(b));
            ImGui::TableNextColumn();
            int waiting = s_capture_player == p && s_capture_button == b;
            snprintf(label, sizeof(label), "%s###k",
                     waiting && !s_capture_pad ? "Press a key… (Esc cancels)" : input_key_name(c->keys[b]));
            if (ImGui::Button(label, ImVec2(-1, 0))) {
                s_capture_player = p;
                s_capture_button = b;
                s_capture_pad = 0;
            }
            ImGui::TableNextColumn();
            snprintf(label, sizeof(label), "%s###p",
                     waiting && s_capture_pad ? (pad >= 0 ? "Press a controller button…" : "No controller connected")
                                               : input_pad_input_name(c->pad[b]));
            if (ImGui::Button(label, ImVec2(-1, 0))) {
                s_capture_player = p;
                s_capture_button = b;
                s_capture_pad = 1;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    check("The left stick also moves the directions", &c->stick_as_dpad);
    if (ImGui::Button("Auto-map controller")) {
        settings_default_pad(c);
    }
    ImGui::SameLine();
    if (ImGui::Button("Keyboard defaults")) {
        settings_default_keys(c, p);
    }
}

static void controls_tab(void)
{
    poll_capture();
    if (ImGui::BeginTabBar("players")) {
        for (int p = 0; p < SETTINGS_PLAYERS; p++) {
            char name[32];
            snprintf(name, sizeof(name), "Player %d", p + 1);
            if (ImGui::BeginTabItem(name)) {
                ImGui::PushID(p);
                controls_player(p);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

static void sound_tab(void)
{
    Settings *s = &g_settings_pc;
    section("VOLUME");
    row("Master");
    ImGui::SliderInt("##master", &s->volume_master, 0, 100, "%d %%");
    row("Music");
    ImGui::SliderInt("##music", &s->volume_music, 0, 100, "%d %%");
    row("Effects");
    ImGui::SliderInt("##effects", &s->volume_effects, 0, 100, "%d %%");
    help("Heard at once in the game (F1).");
}

static void reset_all(void)
{
    char disc[SETTINGS_PATH_MAX];
    strcpy(disc, g_settings_pc.disc_path);
    settings_defaults(&g_settings_pc);
    strcpy(g_settings_pc.disc_path, disc);
    apply_window_settings();
}

int settings_capturing(void)
{
    return s_capture_player >= 0;
}

/* The settings with tabs (the in-game window, F1). */
int settings_screen(void)
{
    int open = 1;
    if (ImGui::BeginTabBar("settings")) {
        if (ImGui::BeginTabItem("Graphics")) { graphics_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Controls")) { controls_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Sound")) { sound_tab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(160 * ui_scale(), 0)) && s_capture_player < 0) {
        settings_save();
        open = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all")) {
        reset_all();
    }
    return open;
}

/* ---- the launcher --------------------------------------------------- */

/* layout, in pixels at 96 DPI */
#define MARGIN                  52.0f
#define TAG_SIZE                15.0f
#define TITLE_SIZE              66.0f
#define MENU_SIZE               30.0f
#define MENU_WIDTH              360.0f
#define PAGE_TITLE_SIZE         40.0f
#define TAB_SIZE                22.0f
#define TAB_WIDTH               200.0f
#define SMALL_SIZE              14.0f
#define PANEL_LEFT              280.0f      /* the settings panel's left edge */
#define WINDOW_BUTTON           40.0f       /* minimise / close, top right */

/* part of the picture kept in view: the ball on its track */
static const ImVec2 BACKGROUND_FOCUS(0.69f, 0.50f);

enum { PAGE_MENU, PAGE_SETTINGS };
enum { TAB_GRAPHICS, TAB_CONTROLS, TAB_SOUND, TAB_COUNT };

static UiImage s_background;
static int s_page = PAGE_MENU;
static int s_tab = TAB_GRAPHICS;
static int s_focus = 1;                 /* put the keyboard/controller focus on the page */

void launcher_shutdown(void)
{
    ui_image_free(&s_background);
}

/* Text with a soft shadow, so it stays readable on the photo. */
static void shadowed_text(ImDrawList *draw, ImFont *font, float size, ImVec2 pos, ImU32 colour,
                          const char *text)
{
    float off = size / 20 > 1 ? size / 20 : 1;
    draw->AddText(font, size, ImVec2(pos.x + off, pos.y + off), IM_COL32(0, 0, 0, 170), text);
    draw->AddText(font, size, pos, colour, text);
}

/*
 * A menu item: large text, no box; when selected (hovered or focused) a
 * green bar and a glow fading to the right.  The mouse moves the focus
 * too, so there is always one selection.  Returns TRUE when chosen.
 */
static bool s_mouse_focus;              /* the selection follows the mouse, not the keys */

/* TRUE when the keyboard or a controller moves the selection */
static bool nav_key_pressed(void)
{
    static const ImGuiKey KEYS[] = { ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Tab,
                                     ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,
                                     ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown };
    for (int i = 0; i < (int) (sizeof(KEYS) / sizeof(KEYS[0])); i++) {
        if (ImGui::IsKeyPressed(KEYS[i])) {
            return true;
        }
    }
    return false;
}

static bool menu_item(const char *label, float size, float width, bool enabled, bool current)
{
    float scale = ui_scale();
    ImFont *font = ui_font_display();
    float height = size * 1.55f;

    ImGui::BeginDisabled(!enabled);
    /* (its own highlight instead of Dear ImGui's focus frame) */
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool chosen = ImGui::InvisibleButton(label, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (enabled && ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0
                                              || ImGui::GetIO().MouseDelta.y != 0)) {
        ImGui::SetKeyboardFocusHere(-1);
        s_mouse_focus = true;
    }
    bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList *draw = ImGui::GetWindowDrawList();

    if (hot) {
        draw->AddRectFilledMultiColor(p0, p1, COL_ACCENT_SOFT, COL_ACCENT_NONE, COL_ACCENT_NONE,
                                      COL_ACCENT_SOFT);
    }
    if (hot || current) {
        float inset = height * 0.22f;
        draw->AddRectFilled(ImVec2(p0.x, p0.y + inset), ImVec2(p0.x + 4 * scale, p1.y - inset),
                            hot ? COL_ACCENT : COL_TEXT_FAINT);
    }
    ImU32 colour = !enabled ? COL_TEXT_DISABLED : hot || current ? COL_TEXT : COL_TEXT_DIM;
    ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0, label);
    shadowed_text(draw, font, size, ImVec2(p0.x + (hot ? 24 : 18) * scale, (p0.y + p1.y - text.y) / 2),
                  colour, label);
    return chosen;
}

/* The button hints at the bottom right: [key] action ... */
static void hints(ImDrawList *draw, float right, float y, const char *const *pairs, int count)
{
    float scale = ui_scale();
    ImFont *font = ui_font_bold(), *plain = ImGui::GetFont();
    float size = SMALL_SIZE * scale, pad = 7 * scale, gap = 22 * scale;
    float x = right;
    for (int i = count - 1; i >= 0; i--) {
        const char *key = pairs[i * 2], *action = pairs[i * 2 + 1];
        ImVec2 a = plain->CalcTextSizeA(size, FLT_MAX, 0, action);
        ImVec2 k = font->CalcTextSizeA(size, FLT_MAX, 0, key);
        x -= a.x;
        shadowed_text(draw, plain, size, ImVec2(x, y), COL_TEXT_DIM, action);
        x -= 8 * scale + k.x + 2 * pad;
        draw->AddRect(ImVec2(x, y - 3 * scale), ImVec2(x + k.x + 2 * pad, y + k.y + 3 * scale), COL_KEY,
                      5 * scale, 0, 1.2f * scale);
        draw->AddText(font, size, ImVec2(x + pad, y), COL_TEXT, key);
        x -= gap;
    }
}

/* Minimise and close, top right (the splash has no title bar).  Returns
 * TRUE for close. */
static bool window_buttons(float w)
{
    float scale = ui_scale();
    float size = WINDOW_BUTTON * scale;
    bool close = false;
    const char *labels[2] = { "\xE2\x80\x93", "\xC3\x97" };     /* en dash, multiplication sign */
    for (int i = 0; i < 2; i++) {
        ImVec2 p0(w - size * (2 - i), 0);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("window", ImVec2(size, size * 0.8f));
        ImGui::PopID();
        bool hover = ImGui::IsItemHovered();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        if (hover) {
            draw->AddRectFilled(p0, ImVec2(p0.x + size, size * 0.8f),
                                i == 1 ? IM_COL32(196, 43, 28, 230) : IM_COL32(255, 255, 255, 40));
        }
        float text_size = 20 * scale;
        ImVec2 t = ImGui::GetFont()->CalcTextSizeA(text_size, FLT_MAX, 0, labels[i]);
        draw->AddText(ImGui::GetFont(), text_size,
                      ImVec2(p0.x + (size - t.x) / 2, (size * 0.8f - t.y) / 2),
                      hover ? COL_TEXT : COL_TEXT_DIM, labels[i]);
        if (clicked) {
            if (i == 0) {
                platform_minimize();
            } else {
                close = true;
            }
        }
    }
    return close;
}

/* The picture, darkened where the text is. */
static void draw_background(float w, float h, bool settings)
{
    ImDrawList *draw = ImGui::GetBackgroundDrawList();

    ui_draw_image_cover(draw, &s_background, ImVec2(0, 0), ImVec2(w, h), BACKGROUND_FOCUS);
    if (settings) {
        draw->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(4, 12, 32, 205));
        return;
    }
    float solid = w * 0.34f, fade = w * 0.80f;
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(solid, h), COL_SHADE);
    draw->AddRectFilledMultiColor(ImVec2(solid, 0), ImVec2(fade, h), COL_SHADE, COL_SHADE_NONE,
                                  COL_SHADE_NONE, COL_SHADE);
    draw->AddRectFilledMultiColor(ImVec2(0, h * 0.62f), ImVec2(w, h), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));
}

static void title(float x, float y)
{
    float scale = ui_scale();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImFont *font = ui_font_display();
    float size = TITLE_SIZE * scale;

    shadowed_text(draw, ui_font_bold(), TAG_SIZE * scale, ImVec2(x + 2 * scale, y), COL_ACCENT,
                  "PC PORT");
    y += TAG_SIZE * scale * 1.6f;
    shadowed_text(draw, font, size, ImVec2(x, y), COL_TEXT, "ROLL");
    shadowed_text(draw, font, size, ImVec2(x, y + size * 0.88f), COL_ACCENT, "AWAY");
}

/* The disc: a dot for its state, the text and the file. */
static void disc_status(float x, float y)
{
    float scale = ui_scale();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float size = SMALL_SIZE * scale;
    ImU32 dot = s_disc_status == DISC_OK ? COL_ACCENT
                : s_disc_status == DISC_OTHER_DUMP ? IM_COL32(242, 214, 90, 255)
                : s_disc_status == DISC_NONE ? COL_TEXT_FAINT : IM_COL32(255, 115, 102, 255);
    draw->AddCircleFilled(ImVec2(x + 4 * scale, y + size * 0.55f), 4 * scale, dot);
    shadowed_text(draw, font, size, ImVec2(x + 16 * scale, y), COL_TEXT, s_disc_detail);
    const char *second = g_settings_pc.disc_path[0] ? g_settings_pc.disc_path
                                                    : "Drop a .bin, .cue or .chd on the window.";
    const char *name = second;
    for (const char *p = second; *p; p++) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    shadowed_text(draw, font, size * 0.9f, ImVec2(x + 16 * scale, y + size * 1.45f), COL_TEXT_FAINT,
                  name);
}

static void choose_disc_dialog(void)
{
    char path[SETTINGS_PATH_MAX];
    if (platform_open_file_dialog(path, sizeof(path))) {
        choose_disc(path);
    }
}

static void open_page(int page)
{
    if (s_page == PAGE_SETTINGS && page != PAGE_SETTINGS) {
        settings_save();
    }
    s_page = page;
    s_focus = 1;
}

/* the main menu; returns an enum LauncherAction */
static int menu_page(float w, float h)
{
    float scale = ui_scale();
    float margin = MARGIN * scale;
    int action = LAUNCHER_STAY;

    title(margin, margin * 0.9f);
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, h * 0.47f));
    if (s_focus && disc_playable()) {
        ImGui::SetKeyboardFocusHere();
    }
    if (menu_item("PLAY", MENU_SIZE * scale, MENU_WIDTH * scale, disc_playable(), false)) {
        action = LAUNCHER_PLAY;
    }
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, ImGui::GetCursorScreenPos().y));
    if (s_focus && !disc_playable()) {
        ImGui::SetKeyboardFocusHere();
    }
    s_focus = 0;
    if (menu_item("CHOOSE DISC", MENU_SIZE * scale, MENU_WIDTH * scale, true, false)) {
        choose_disc_dialog();
    }
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, ImGui::GetCursorScreenPos().y));
    if (menu_item("SETTINGS", MENU_SIZE * scale, MENU_WIDTH * scale, true, false)) {
        open_page(PAGE_SETTINGS);
    }
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, ImGui::GetCursorScreenPos().y));
    if (menu_item("QUIT", MENU_SIZE * scale, MENU_WIDTH * scale, true, false)) {
        action = LAUNCHER_QUIT;
    }

    disc_status(margin, h - margin - SMALL_SIZE * scale * 1.6f);
    static const char *const HINTS[] = { "Enter / A", "Select", "Esc / B", "Quit" };
    hints(ImGui::GetWindowDrawList(), w - margin * 0.6f, h - margin * 0.75f, HINTS, 2);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        action = LAUNCHER_QUIT;
    }
    return action;
}

/* the settings: categories on the left, the rows in a panel */
static void settings_page(float w, float h)
{
    static const char *const TABS[TAB_COUNT] = { "GRAPHICS", "CONTROLS", "SOUND" };
    float scale = ui_scale();
    float margin = MARGIN * scale;

    shadowed_text(ImGui::GetWindowDrawList(), ui_font_display(), PAGE_TITLE_SIZE * scale,
                  ImVec2(margin, margin * 0.8f), COL_TEXT, "SETTINGS");
    float y = margin * 0.8f + PAGE_TITLE_SIZE * scale * 1.7f;
    if (nav_key_pressed()) {
        s_mouse_focus = false;
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, y));
        if (s_focus && i == s_tab) {
            ImGui::SetKeyboardFocusHere();
        }
        if (menu_item(TABS[i], TAB_SIZE * scale, TAB_WIDTH * scale, true, i == s_tab)) {
            s_tab = i;
        }
        if (ImGui::IsItemFocused() && !s_mouse_focus) {
            s_tab = i;                  /* the keys show each page in turn; the mouse clicks */
        }
        y = ImGui::GetCursorScreenPos().y;
    }
    s_focus = 0;
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, y + 16 * scale));
    if (menu_item("BACK", TAB_SIZE * scale, TAB_WIDTH * scale, true, false)) {
        open_page(PAGE_MENU);
    }
    /* (out of the way: at the bottom, level with the panel's) */
    float reset_size = SMALL_SIZE * 1.1f * scale;
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, h - margin * 1.45f - reset_size * 1.55f));
    if (menu_item("Reset all", reset_size, TAB_WIDTH * scale, true, false)) {
        reset_all();
    }

    /* the panel */
    float left = PANEL_LEFT * scale, top = margin * 0.8f;
    ImGui::SetCursorScreenPos(ImVec2(left, top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(COL_PANEL));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * scale, 16 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 8 * scale));
    ImGui::BeginChild("panel", ImVec2(w - left - margin * 0.6f, h - top - margin * 1.45f),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    switch (s_tab) {
    case TAB_GRAPHICS: graphics_tab(); break;
    case TAB_CONTROLS: controls_tab(); break;
    default: sound_tab(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    static const char *const HINTS[] = { "Enter / A", "Select", "Esc / B", "Back" };
    hints(ImGui::GetWindowDrawList(), w - margin * 0.6f, h - margin * 0.75f, HINTS, 2);
    if (!settings_capturing()
        && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        open_page(PAGE_MENU);
    }
}

void launcher_init(void)
{
    ui_image_from_resource(IDR_LAUNCHER_BACKGROUND, &s_background);
    if (g_settings_pc.disc_path[0]) {
        s_disc_status = disc_check(g_settings_pc.disc_path, s_disc_detail, sizeof(s_disc_detail));
    }
}

/* the layout's size at 96 DPI; the window scales it (keeping proportions) */
#define LAYOUT_WIDTH            1000.0f
#define LAYOUT_HEIGHT           600.0f
#define MIN_LAYOUT_SCALE        0.6f
#define MAX_LAYOUT_SCALE        4.0f

int launcher_frame(void)
{
    char dropped[SETTINGS_PATH_MAX];
    int action = LAUNCHER_STAY;
    int iw, ih;

    if (platform_take_dropped_file(dropped, sizeof(dropped))) {
        choose_disc(dropped);
    }
    platform_client_size(&iw, &ih);
    float w = (float) iw, h = (float) ih;

    /* everything scales with the window: our own sizes by s_scale, Dear
     * ImGui's spacing by a scaled copy of the style and its text by a
     * pushed font size (both put back at the end) */
    float dpi = platform_dpi_scale();
    float k = w / (LAYOUT_WIDTH * dpi) < h / (LAYOUT_HEIGHT * dpi) ? w / (LAYOUT_WIDTH * dpi)
                                                                   : h / (LAYOUT_HEIGHT * dpi);
    k = k < MIN_LAYOUT_SCALE ? MIN_LAYOUT_SCALE : k > MAX_LAYOUT_SCALE ? MAX_LAYOUT_SCALE : k;
    s_scale = dpi * k;
    ImGuiStyle &style = ImGui::GetStyle();
    ImGuiStyle saved = style;
    style.ScaleAllSizes(k);
    s_in_launcher = 1;

    draw_background(w, h, s_page == PAGE_SETTINGS);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    if (s_focus) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNavCursorVisible(true);   /* Enter / A acts on the selection at once */
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * ui_scale()));
    ImGui::Begin("launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                       | ImGuiWindowFlags_NoSavedSettings
                                       | ImGuiWindowFlags_NoBackground
                                       | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushFont(NULL, saved.FontSizeBase * k);
    if (s_page == PAGE_MENU) {
        action = menu_page(w, h);
    } else {
        settings_page(w, h);
    }
    if (window_buttons(w)) {
        action = LAUNCHER_QUIT;
    }
    /* the splash has no title bar: drag it by any free spot */
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()
        && ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive()) {
        platform_begin_window_drag();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);
    style = saved;
    s_in_launcher = 0;
    return action;
}
