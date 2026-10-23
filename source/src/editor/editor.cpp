/*
 * editor.cpp - the level editor's panels and commands: the tools and the
 * palette of blocks and things (left), the level and the selection
 * (right), files, undo, the game's own levels from the disc, and "Play",
 * which starts the game (roll_away.exe next to the editor) with the
 * level.
 */
#include <windows.h>
#include <commdlg.h>
#include <stdarg.h>
#include <stdio.h>

#include "editor/editor.h"
#include "editor/disc_assets.h"
#include "level/level_file.h"

#define PANEL_WIDTH             270.0f
#define UNDO_LEVELS             200
#define ACCENT                  ImVec4(1.00f, 0.66f, 0.25f, 1.0f)
#define FILE_FILTER             L"Roll Away levels (*.rlv)\0*.rlv\0All files\0*.*\0"
#define DISC_FILTER             L"Disc images (*.chd, *.cue, *.iso, *.bin)\0*.chd;*.cue;*.iso;*.bin\0All files\0*.*\0"

Editor g_ed;

static HWND s_hwnd;
static bool s_open_disc_dialog, s_show_help, s_show_problems_on_play;
static int s_disc_world;
static std::vector<DiscLevel> s_disc_levels;
static int s_disc_choice = -1;
static std::string s_disc_error;

static const char *const TOOL_NAMES[TOOL_COUNT] = { "Build", "Erase", "Paint", "Things", "Select" };
static const char *const TOOL_HINTS[TOOL_COUNT] = {
    "Click a side of a block: a new block snaps on there. Drag for a row or a floor.",
    "Click blocks to take them away.",
    "Click blocks to make them the chosen kind.",
    "Click an open side of a block to put the chosen thing on it.",
    "Click a block's side to see and change it on the right."
};
static const char *const SIDE_NAMES[6] = { "top", "right (+X)", "front (+Z)", "back (-Z)", "left (-X)", "bottom" };
static const char *const DIRECTION_NAMES[6] = { "Up", "Right (+X)", "Forward (+Z)", "Back (-Z)", "Left (-X)", "Down" };
static const char *const SIGNAL_NAMES[4] = { "Yellow", "Blue", "Green", "Red" };

/* ---- small helpers ---- */

void editor_say(const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_ed.message = text;
    g_ed.message_time = ImGui::GetTime();
}

static std::wstring wide(const std::string &s)
{
    wchar_t out[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out, MAX_PATH * 2);
    return out;
}

static std::string utf8(const wchar_t *s)
{
    char out[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, sizeof(out), NULL, NULL);
    return out;
}

static std::string base_name(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static void update_title()
{
    std::string title = (g_ed.dirty ? "* " : "") +
                        (g_ed.path.empty() ? std::string("New level") : base_name(g_ed.path)) +
                        " - Roll Away level editor";
    SetWindowTextW(s_hwnd, wide(title).c_str());
}

void editor_snapshot()
{
    g_ed.undo.push_back(g_ed.level);
    if (g_ed.undo.size() > UNDO_LEVELS) {
        g_ed.undo.erase(g_ed.undo.begin());
    }
    g_ed.redo.clear();
}

/* the level as it was when the frame began (before a slider moved) */
static Level s_frame_start;

static void snapshot_frame_start()
{
    g_ed.undo.push_back(s_frame_start);
    if (g_ed.undo.size() > UNDO_LEVELS) {
        g_ed.undo.erase(g_ed.undo.begin());
    }
    g_ed.redo.clear();
}

void editor_changed()
{
    if (!g_ed.dirty) {
        g_ed.dirty = true;
        update_title();
    }
}

static void undo()
{
    if (g_ed.undo.empty()) {
        return;
    }
    g_ed.redo.push_back(g_ed.level);
    int theme = g_ed.level.settings.theme;
    g_ed.level = g_ed.undo.back();
    g_ed.undo.pop_back();
    if (theme != g_ed.level.settings.theme) {
        editor_world_changed();
    }
    editor_changed();
}

static void redo()
{
    if (g_ed.redo.empty()) {
        return;
    }
    g_ed.undo.push_back(g_ed.level);
    int theme = g_ed.level.settings.theme;
    g_ed.level = g_ed.redo.back();
    g_ed.redo.pop_back();
    if (theme != g_ed.level.settings.theme) {
        editor_world_changed();
    }
    editor_changed();
}

void editor_world_changed()
{
    std::vector<uint32_t> textures[WORLD_TEX_COUNT];
    if (assets_ready() && assets_world_textures(g_ed.level.settings.theme, textures)) {
        renderer_set_textures(textures[WORLD_TEX_TOP].data(), textures[WORLD_TEX_SIDE].data());
    } else {
        renderer_set_textures(NULL, NULL);
    }
}

/* ---- the editor's own settings (level_editor.ini next to it) ---- */

static std::string ini_path()
{
    return assets_game_folder() + "level_editor.ini";
}

static std::string ini_disc()
{
    FILE *f = fopen(ini_path().c_str(), "rb");
    if (!f) {
        return "";
    }
    char line[2048] = "";
    std::string disc;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "disc=", 5)) {
            disc = line + 5;
            while (!disc.empty() && (disc.back() == '\n' || disc.back() == '\r')) {
                disc.pop_back();
            }
        }
    }
    fclose(f);
    return disc;
}

static void ini_save_disc(const std::string &disc)
{
    FILE *f = fopen(ini_path().c_str(), "wb");
    if (f) {
        fprintf(f, "; Roll Away level editor\ndisc=%s\n", disc.c_str());
        fclose(f);
    }
}

/* ---- files ---- */

static bool ask_save_changes();

static bool file_dialog(bool save, const wchar_t *filter, const wchar_t *ext, std::string *path)
{
    wchar_t buffer[MAX_PATH * 2] = L"";
    if (!path->empty()) {
        wcsncpy(buffer, wide(*path).c_str(), MAX_PATH * 2 - 1);
    }
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) {
        return false;
    }
    *path = utf8(buffer);
    return true;
}

static bool write_level(const std::string &path)
{
    std::vector<uint8_t> data = g_ed.level.to_game();
    LevelFile lf = {};
    lf.theme = g_ed.level.settings.theme;
    snprintf(lf.name, sizeof(lf.name), "%s", g_ed.level.settings.name.c_str());
    lf.data = data.data();
    lf.size = (int) data.size();
    return level_file_save(path.c_str(), &lf) != 0;
}

static bool save_as();

static bool save()
{
    if (g_ed.path.empty()) {
        return save_as();
    }
    if (!write_level(g_ed.path)) {
        MessageBoxW(s_hwnd, L"The level could not be saved.", L"Roll Away level editor", MB_ICONERROR);
        return false;
    }
    g_ed.dirty = false;
    update_title();
    editor_say("Saved %s", base_name(g_ed.path).c_str());
    return true;
}

static bool save_as()
{
    std::string path = g_ed.path;
    if (path.empty() && !g_ed.level.settings.name.empty()) {
        path = g_ed.level.settings.name + ".rlv";
    }
    if (!file_dialog(true, FILE_FILTER, L"rlv", &path)) {
        return false;
    }
    g_ed.path = path;
    return save();
}

static void new_level()
{
    if (!ask_save_changes()) {
        return;
    }
    int theme = g_ed.level.settings.theme;
    g_ed.level.make_new();
    g_ed.level.settings.theme = theme;
    g_ed.path.clear();
    g_ed.undo.clear();
    g_ed.redo.clear();
    g_ed.dirty = false;
    g_ed.sel_cell = -1;
    update_title();
    view_focus();
}

static void load_data(const std::vector<uint8_t> &data, int theme, const std::string &name,
                      const std::string &path)
{
    Level fresh;
    fresh.settings.theme = theme;
    fresh.settings.name = name;
    std::string error;
    if (!fresh.from_game(data.data(), (int) data.size(), &error)) {
        MessageBoxW(s_hwnd, wide(error).c_str(), L"Roll Away level editor", MB_ICONERROR);
        return;
    }
    fresh.update_paths();
    bool world = theme != g_ed.level.settings.theme;
    g_ed.level = fresh;
    g_ed.path = path;
    g_ed.undo.clear();
    g_ed.redo.clear();
    g_ed.dirty = false;
    g_ed.sel_cell = -1;
    if (world) {
        editor_world_changed();
    }
    update_title();
    view_focus();
}

void editor_open_path(const char *path)
{
    LevelFile lf;
    char error[256];
    if (!level_file_load(path, &lf, error, sizeof(error))) {
        MessageBoxW(s_hwnd, wide(error).c_str(), L"Roll Away level editor", MB_ICONERROR);
        return;
    }
    std::vector<uint8_t> data(lf.data, lf.data + lf.size);
    std::string name = lf.name;
    int theme = lf.theme;
    level_file_free(&lf);
    load_data(data, theme, name, path);
    editor_say("Opened %s", base_name(path).c_str());
}

static void open_file()
{
    if (!ask_save_changes()) {
        return;
    }
    std::string path;
    if (file_dialog(false, FILE_FILTER, L"rlv", &path)) {
        editor_open_path(path.c_str());
    }
}

/* Yes: saved, No: thrown away, Cancel: FALSE */
static bool ask_save_changes()
{
    if (!g_ed.dirty) {
        return true;
    }
    int answer = MessageBoxW(s_hwnd, L"Save the changes to this level?", L"Roll Away level editor",
                             MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) {
        return false;
    }
    return answer == IDNO || save();
}

bool editor_close_request()
{
    return ask_save_changes();
}

static void choose_disc()
{
    std::string path = assets_disc_path();
    if (!file_dialog(false, DISC_FILTER, NULL, &path)) {
        return;
    }
    std::string error;
    if (!assets_open(path.c_str(), &error)) {
        MessageBoxW(s_hwnd, wide(error).c_str(), L"Roll Away level editor", MB_ICONERROR);
        return;
    }
    ini_save_disc(path);
    editor_world_changed();
    editor_say("Disc: %s", base_name(path).c_str());
}

/* ---- playing ---- */

static void play()
{
    std::vector<Level::Problem> problems = g_ed.level.check();
    for (const Level::Problem &p : problems) {
        if (p.error) {
            s_show_problems_on_play = true;
            return;
        }
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::string path = utf8(temp) + "roll_away_editor_test.rlv";
    if (!write_level(path)) {
        MessageBoxW(s_hwnd, L"The level could not be written for the game.", L"Roll Away level editor",
                    MB_ICONERROR);
        return;
    }
    std::wstring exe = wide(assets_game_folder() + "roll_away.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(s_hwnd, L"roll_away.exe is not next to the editor.", L"Roll Away level editor",
                    MB_ICONERROR);
        return;
    }
    std::wstring command = L"\"" + exe + L"\" --level \"" + wide(path) + L"\"";
    std::wstring folder = wide(assets_game_folder());
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    if (!CreateProcessW(exe.c_str(), line.data(), NULL, NULL, FALSE, 0, NULL, folder.c_str(), &si, &pi)) {
        MessageBoxW(s_hwnd, L"The game could not be started.", L"Roll Away level editor", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    editor_say("The game starts with the level (Esc in the game comes back).");
}

/* ---- the panels ---- */

static void tool_button(int tool)
{
    bool on = g_ed.tool == tool;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.50f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.60f, 0.22f, 1.0f));
    }
    char label[48];
    snprintf(label, sizeof(label), "%s\n  %d", TOOL_NAMES[tool], tool + 1);
    if (ImGui::Button(label, ImVec2(46, 44))) {
        g_ed.tool = tool;
        g_ed.link_pick = PICK_NONE;
    }
    if (on) {
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TOOL_HINTS[tool]);
    }
}

static void block_swatch(int kind)
{
    ImU32 col = g_block_kinds[kind].colour;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool chosen = g_ed.block_kind == kind;
    char id[16];
    snprintf(id, sizeof(id), "##kind%d", kind);
    if (ImGui::InvisibleButton(id, ImVec2(PANEL_WIDTH - 30, 22))) {
        g_ed.block_kind = kind;
        if (g_ed.tool != TOOL_BUILD && g_ed.tool != TOOL_PAINT) {
            g_ed.tool = TOOL_BUILD;
        }
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (chosen || ImGui::IsItemHovered()) {
        dl->AddRectFilled(p, ImVec2(p.x + PANEL_WIDTH - 30, p.y + 22),
                          chosen ? IM_COL32(220, 130, 40, 120) : IM_COL32(255, 255, 255, 25), 4);
    }
    dl->AddRectFilled(ImVec2(p.x + 4, p.y + 3), ImVec2(p.x + 20, p.y + 19), IM_COL32(200, 160, 100, 255), 3);
    dl->AddRectFilled(ImVec2(p.x + 4, p.y + 3), ImVec2(p.x + 20, p.y + 19), (col & 0x00FFFFFF) | 0xB0000000u, 3);
    dl->AddRect(ImVec2(p.x + 4, p.y + 3), ImVec2(p.x + 20, p.y + 19), IM_COL32(0, 0, 0, 160), 3);
    dl->AddText(ImVec2(p.x + 28, p.y + 3), IM_COL32(230, 235, 245, 255), g_block_kinds[kind].name);
}

static bool thing_button(const ThingInfo &info, int variant, float size)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool chosen = g_ed.thing_id == info.id && (!(info.flags & THING_KINDS) || g_ed.thing_variant == variant);
    char id[32];
    snprintf(id, sizeof(id), "##thing%d_%d", info.id, variant);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                      chosen ? IM_COL32(220, 130, 40, 150)
                             : ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 12),
                      5);
    draw_thing_icon(dl, ImVec2(p.x + size / 2, p.y + size / 2), size * 0.34f, info.id,
                    (info.flags & THING_KINDS) ? variant : g_ed.thing_variant & 3, 255);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", (info.flags & THING_KINDS) ? thing_variant_name(info.id, variant) : info.name);
    }
    if (clicked) {
        g_ed.thing_id = info.id;
        if (info.flags & THING_KINDS) {
            g_ed.thing_variant = variant;
        } else if (!(info.flags & (THING_COLOURED | THING_TIMED))) {
            g_ed.thing_variant = 0;
        }
        g_ed.tool = TOOL_THING;
    }
    return clicked;
}

static void left_panel(float top, float height)
{
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(PANEL_WIDTH, height));
    ImGui::Begin("##tools", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                      | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextColored(ACCENT, "TOOLS");
    for (int t = 0; t < TOOL_COUNT; t++) {
        if (t) {
            ImGui::SameLine(0, 4);
        }
        tool_button(t);
    }
    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "BLOCKS");
    for (int k = 0; k < BLOCK_KIND_COUNT; k++) {
        block_swatch(k);
    }
    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "THINGS");
    const char *group = "";
    float size = 40;
    int in_row = 0;
    int per_row = (int) ((PANEL_WIDTH - 20) / (size + 4));
    for (int i = 0; i < g_thing_count; i++) {
        const ThingInfo &info = g_things[i];
        if (strcmp(group, info.group)) {
            group = info.group;
            ImGui::TextDisabled("%s", group);
            in_row = 0;
        }
        int variants = (info.flags & THING_KINDS) ? 3 : 1;
        for (int v = 0; v < variants; v++) {
            if (in_row) {
                ImGui::SameLine(0, 4);
            }
            thing_button(info, v, size);
            if (++in_row >= per_row) {
                in_row = 0;
            }
        }
    }
    const ThingInfo *chosen = thing_info(g_ed.thing_id);
    if (chosen && (chosen->flags & (THING_COLOURED | THING_TIMED))) {
        ImGui::Spacing();
        ImGui::SetNextItemWidth(140);
        if (chosen->flags & THING_COLOURED) {
            ImGui::Combo("Colour##new", &g_ed.thing_variant, SIGNAL_NAMES, 4);
        } else {
            ImGui::SliderInt("Timing##new", &g_ed.thing_variant, 0, 3);
        }
    }
    ImGui::End();
}

/* a reference's line in the properties: its target and buttons */
static void link_row(const char *label, Link *link, int pick, const char *tip)
{
    ImGui::PushID(label);
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    if (link->valid()) {
        int x, y, z;
        Level::position(link->cell, &x, &y, &z);
        if (link->side == LINK_SELF) {
            ImGui::TextColored(ACCENT, "laser %d,%d,%d", x, y, z);
        } else {
            const Block *b = g_ed.level.block(link->cell);
            int id = b && b->kind <= BLOCK_ACID ? b->sides[link->side].id() : 0;
            const ThingInfo *info = thing_info(id);
            ImGui::TextColored(ACCENT, "%s %d,%d,%d", info ? info->name : "?", x, y, z);
        }
    } else {
        ImGui::TextDisabled("none");
    }
    bool picking = g_ed.link_pick == pick;
    if (ImGui::Button(picking ? "Click a target..." : "Choose")) {
        g_ed.link_pick = picking ? PICK_NONE : pick;
        if (!picking) {
            editor_say("%s", tip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear") && link->valid()) {
        editor_snapshot();
        *link = Link();
        editor_changed();
    }
    ImGui::PopID();
}

static void selection_panel()
{
    Level &lv = g_ed.level;
    Block *b = lv.block(g_ed.sel_cell);
    if (!b) {
        ImGui::TextDisabled("Nothing selected (Select tool, 5).");
        return;
    }
    int x, y, z;
    Level::position(g_ed.sel_cell, &x, &y, &z);
    ImGui::Text("Block at %d, %d, %d", x, y, z);
    int kind = b->kind;
    ImGui::SetNextItemWidth(150);
    const char *names[BLOCK_KIND_COUNT];
    for (int k = 0; k < BLOCK_KIND_COUNT; k++) {
        names[k] = g_block_kinds[k].name;
    }
    if (ImGui::Combo("Kind", &kind, names, BLOCK_KIND_COUNT) && kind != b->kind) {
        editor_snapshot();
        Block keep = *b;
        lv.add_block(g_ed.sel_cell, kind);
        b = lv.block(g_ed.sel_cell);
        if (keep.kind <= BLOCK_ACID && kind <= BLOCK_ACID) {
            for (int s = 0; s < 6; s++) {
                b->sides[s] = keep.sides[s];
            }
        }
        lv.update_paths();
        editor_changed();
    }
    if (b->kind == BLOCK_MOVING || b->kind == BLOCK_LASER) {
        int dir = b->prop[1] < 6 ? b->prop[1] : 0;
        ImGui::SetNextItemWidth(150);
        if (ImGui::Combo("Direction", &dir, DIRECTION_NAMES, 6)) {
            editor_snapshot();
            b->prop[1] = (uint16_t) dir;
            lv.update_paths();
            editor_changed();
        }
        int length = b->length;
        ImGui::SetNextItemWidth(150);
        bool laser = b->kind == BLOCK_LASER;
        if (ImGui::SliderInt(laser ? "Beam length" : "Distance", &length, laser ? 0 : 1, 16,
                             laser && length == 0 ? "to a block" : "%d")) {
            b->length = length;
            lv.update_paths();
            editor_changed();
        }
        if (ImGui::IsItemActivated()) {
            snapshot_frame_start();
        }
        if (!laser) {
            int speed = b->prop[17] == 0xFFFF ? 2 : b->prop[17];
            ImGui::SetNextItemWidth(150);
            if (ImGui::SliderInt("Speed", &speed, 1, 4)) {
                b->prop[17] = (uint16_t) speed;
                editor_changed();
            }
            if (ImGui::IsItemActivated()) {
                snapshot_frame_start();
            }
            ImGui::TextDisabled("It moves %s first, then back.", DIRECTION_NAMES[dir]);
        } else {
            int colour = b->prop[22] & 3;
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("Colour", &colour, SIGNAL_NAMES, 4)) {
                editor_snapshot();
                b->prop[22] = (uint16_t) colour;
                editor_changed();
            }
            bool on = b->prop[3] == 1;
            if (ImGui::Checkbox("On at the start", &on)) {
                editor_snapshot();
                b->prop[3] = on ? 1 : 2;
                editor_changed();
            }
            link_row("Power", &b->laser_power, PICK_LASER_POWER,
                     "Click the next transporter, button or laser of this power chain.");
        }
    }
    if (b->kind == BLOCK_FLASHING) {
        int timing = b->prop[2] & 3;
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderInt("Timing", &timing, 0, 3)) {
            b->prop[2] = (uint16_t) timing;
            editor_changed();
        }
        if (ImGui::IsItemActivated()) {
            snapshot_frame_start();
        }
    }
    if (b->kind > BLOCK_ACID) {
        return;
    }
    ImGui::Separator();
    int side = g_ed.sel_side;
    Thing &t = b->sides[side];
    ImGui::Text("Side: %s", SIDE_NAMES[side]);
    if (!t.id()) {
        ImGui::TextDisabled(lv.side_open(g_ed.sel_cell, side) ? "Nothing on it." : "Covered by a block.");
        return;
    }
    const ThingInfo *info = thing_info(t.id());
    ImGui::TextColored(ACCENT, "%s", thing_variant_name(t.id(), t.w[2]));
    if (!info) {
        ImGui::TextDisabled("(a thing the editor does not know: id %d)", t.id());
    } else {
        if (info->flags & THING_ROTATES) {
            ImGui::Text("Facing:");
            for (int r = 1; r <= 4; r++) {
                ImGui::SameLine();
                Vec3 d = thing_direction(side, r);
                char label[24];
                snprintf(label, sizeof(label), "%s%s##rot%d", d.x + d.y + d.z > 0 ? "+" : "-",
                         d.x != 0 ? "X" : d.y != 0 ? "Y" : "Z", r);
                bool on = t.w[1] == r;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.50f, 0.15f, 1.0f));
                }
                if (ImGui::SmallButton(label) && !on) {
                    editor_snapshot();
                    t.w[1] = (uint16_t) r;
                    editor_changed();
                }
                if (on) {
                    ImGui::PopStyleColor();
                }
            }
        }
        if (info->flags & THING_KINDS) {
            int v = t.w[2] < 3 ? t.w[2] : 0;
            const char *kinds[3] = { thing_variant_name(t.id(), 0), thing_variant_name(t.id(), 1),
                                     thing_variant_name(t.id(), 2) };
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("Kind##thing", &v, kinds, 3)) {
                editor_snapshot();
                t.w[2] = (uint16_t) v;
                editor_changed();
            }
        }
        if (info->flags & THING_COLOURED) {
            int v = t.w[2] & 3;
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("Colour##thing", &v, SIGNAL_NAMES, 4)) {
                editor_snapshot();
                t.w[2] = (uint16_t) v;
                editor_changed();
            }
        }
        if (info->flags & THING_TIMED) {
            int v = t.w[2] & 3;
            ImGui::SetNextItemWidth(150);
            if (ImGui::SliderInt("Timing##thing", &v, 0, 3)) {
                t.w[2] = (uint16_t) v;
                editor_changed();
            }
            if (ImGui::IsItemActivated()) {
                snapshot_frame_start();
            }
        }
        if (info->flags & THING_TOGGLES) {
            bool on = t.w[3] == 1;
            if (ImGui::Checkbox("On at the start##thing", &on)) {
                editor_snapshot();
                t.w[3] = on ? 1 : 2;
                editor_changed();
            }
        }
        if (t.id() == THING_TRANSPORTER) {
            link_row("Destination", &t.target, PICK_TARGET, "Click the transporter it takes the ball to.");
            link_row("Power", &t.power, PICK_POWER, "Click the next transporter, button or laser of its power chain.");
        } else if (t.id() == THING_BUTTON) {
            link_row("Powers", &t.power, PICK_POWER, "Click what the button switches (a transporter, button or laser).");
            link_row("Powers 2", &t.target, PICK_TARGET, "Click a second thing the button switches.");
        }
    }
    if (ImGui::Button("Take it away")) {
        editor_snapshot();
        Level::set_thing(&t, THING_NONE);
        editor_changed();
    }
}

static void level_panel()
{
    LevelSettings &st = g_ed.level.settings;
    char name[LEVEL_NAME_SIZE];
    snprintf(name, sizeof(name), "%s", st.name.c_str());
    ImGui::SetNextItemWidth(170);
    if (ImGui::InputText("Name", name, sizeof(name))) {
        st.name = name;
        editor_changed();
    }
    int theme = st.theme;
    ImGui::SetNextItemWidth(170);
    if (ImGui::Combo("World", &theme, g_level_theme_names, LEVEL_THEME_COUNT) && theme != st.theme) {
        editor_snapshot();
        st.theme = theme;
        editor_world_changed();
        editor_changed();
    }
    int seconds = st.seconds;
    ImGui::SetNextItemWidth(170);
    if (ImGui::SliderInt("Time (s)", &seconds, 10, 600)) {
        st.seconds = seconds;
        editor_changed();
    }
    if (ImGui::IsItemActivated()) {
        snapshot_frame_start();
    }
    if (ImGui::Checkbox("Hidden level", &st.hidden)) {
        snapshot_frame_start();
        editor_changed();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The game's level flag \"hidden\".");
    }
    if (ImGui::Checkbox("Invisible blocks reversed", &st.reverse_invisible)) {
        snapshot_frame_start();
        editor_changed();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The game's level flag: invisible blocks show, the others do not.");
    }
}

static void problems_panel(const std::vector<Level::Problem> &problems)
{
    if (problems.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Ready to play.");
        return;
    }
    for (size_t i = 0; i < problems.size(); i++) {
        const Level::Problem &p = problems[i];
        ImGui::PushID((int) i);
        ImGui::TextColored(p.error ? ImVec4(1, 0.4f, 0.35f, 1) : ImVec4(1, 0.8f, 0.3f, 1), "%s",
                           p.error ? "Error" : "Note");
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + PANEL_WIDTH - 70);
        ImGui::TextUnformatted(p.text.c_str());
        ImGui::PopTextWrapPos();
        if (p.cell >= 0 && ImGui::IsItemClicked()) {
            g_ed.sel_cell = p.cell;
            view_focus();
        }
        ImGui::PopID();
    }
}

static void right_panel(float top, float height, float width, const std::vector<Level::Problem> &problems)
{
    ImGui::SetNextWindowPos(ImVec2(width - PANEL_WIDTH - 20, top));
    ImGui::SetNextWindowSize(ImVec2(PANEL_WIDTH + 20, height));
    ImGui::Begin("##side", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextColored(ACCENT, "LEVEL");
    level_panel();
    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "SELECTED");
    selection_panel();
    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "CHECK");
    problems_panel(problems);
    ImGui::Spacing();
    if (ImGui::Button("Play the level  (F5)", ImVec2(PANEL_WIDTH, 34))) {
        play();
    }
    ImGui::End();
}

static void status_bar(float width, float height)
{
    float bar = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos(ImVec2(0, height - bar));
    ImGui::SetNextWindowSize(ImVec2(width, bar));
    ImGui::Begin("##status", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                       | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    Level &lv = g_ed.level;
    int collect = 0;
    for (int i = 0; i < g_thing_count; i++) {
        if (g_things[i].flags & THING_COLLECT) {
            collect += lv.count_things(g_things[i].id);
        }
    }
    ImGui::Text("%d blocks   special %d/%d   pick-ups %d/%d   keys %d", (int) lv.blocks().size(),
                lv.property_count(), MAX_PROPERTIES, collect, MAX_COLLECTABLES, lv.count_things(THING_KEY));
    int cell, side;
    if (view_hover(&cell, &side)) {
        int x, y, z;
        Level::position(cell, &x, &y, &z);
        ImGui::SameLine(420);
        ImGui::Text("at %d, %d, %d", x, y, z);
    }
    if (g_ed.plane || lv.blocks().empty()) {
        ImGui::SameLine(560);
        ImGui::TextDisabled("build plane %d (PgUp/PgDn)", g_ed.plane_y);
    }
    if (!g_ed.message.empty() && ImGui::GetTime() - g_ed.message_time < 6) {
        ImGui::SameLine(780);
        ImGui::TextColored(ACCENT, "%s", g_ed.message.c_str());
    }
    ImGui::End();
}

static void disc_dialog()
{
    if (s_open_disc_dialog) {
        ImGui::OpenPopup("Open a level of the game");
        s_open_disc_dialog = false;
        s_disc_levels.clear();
        s_disc_choice = -1;
        s_disc_error.clear();
        if (!assets_ready()) {
            s_disc_error = "No disc: choose your Roll Away disc (File > Choose disc).";
        } else {
            assets_list_levels(s_disc_world, &s_disc_levels);
        }
    }
    ImGui::SetNextWindowSize(ImVec2(380, 460), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Open a level of the game", NULL)) {
        return;
    }
    bool open_now = false;
    if (!s_disc_error.empty()) {
        ImGui::TextWrapped("%s", s_disc_error.c_str());
    } else {
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("World", &s_disc_world, g_level_theme_names, LEVEL_THEME_COUNT)) {
            assets_list_levels(s_disc_world, &s_disc_levels);
            s_disc_choice = -1;
        }
        ImGui::BeginChild("levels", ImVec2(0, -40), ImGuiChildFlags_Borders);
        for (int i = 0; i < (int) s_disc_levels.size(); i++) {
            if (ImGui::Selectable(s_disc_levels[i].name.c_str(), s_disc_choice == i,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                s_disc_choice = i;
                open_now = ImGui::IsMouseDoubleClicked(0);
            }
        }
        ImGui::EndChild();
    }
    if (ImGui::Button("Open", ImVec2(120, 0))) {
        open_now = true;
    }
    if (open_now && s_disc_choice >= 0) {
        std::vector<uint8_t> data;
        const DiscLevel &dl = s_disc_levels[s_disc_choice];
        if (assets_read_level(s_disc_world, dl, &data)) {
            std::string name = dl.name + " (" + g_level_theme_names[s_disc_world] + ")";
            load_data(data, s_disc_world, name, "");
            editor_say("The game's %s: save it as a level of your own to change it.", name.c_str());
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void problems_dialog(const std::vector<Level::Problem> &problems)
{
    if (s_show_problems_on_play) {
        ImGui::OpenPopup("The level cannot be played yet");
        s_show_problems_on_play = false;
    }
    if (ImGui::BeginPopupModal("The level cannot be played yet", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        for (const Level::Problem &p : problems) {
            if (p.error) {
                ImGui::BulletText("%s", p.text.c_str());
            }
        }
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void help_window()
{
    if (!s_show_help) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Controls", &s_show_help, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ACCENT, "Camera");
        ImGui::BulletText("Right button: turn around the level");
        ImGui::BulletText("Middle button or Shift + right button: move");
        ImGui::BulletText("Wheel: closer / farther;  W A S D, Q E: move;  F: look at the selection");
        ImGui::TextColored(ACCENT, "Building");
        ImGui::BulletText("1 Build, 2 Erase, 3 Paint, 4 Things, 5 Select");
        ImGui::BulletText("Build: click a side of a block, the new block snaps to it;");
        ImGui::Text("      drag to build a row, a floor or a wall");
        ImGui::BulletText("G: the build plane (blocks in the air), PgUp / PgDn its height");
        ImGui::BulletText("R: turn the selected thing;  Delete: take the selection away");
        ImGui::BulletText("Ctrl+Z / Ctrl+Y: undo / redo;  Ctrl+S: save;  F5: play");
        ImGui::TextColored(ACCENT, "The game's rules");
        ImGui::BulletText("One start; things only on open sides of normal, fire, ice,");
        ImGui::Text("      invisible and acid blocks; a block covers what was on a side");
        ImGui::BulletText("The exit opens when all keys are taken");
        ImGui::BulletText("At most %d special blocks / blocks with things, %d pick-ups", MAX_PROPERTIES,
                          MAX_COLLECTABLES);
    }
    ImGui::End();
}

static void menu_bar()
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) new_level();
        if (ImGui::MenuItem("Open...", "Ctrl+O")) open_file();
        if (ImGui::MenuItem("Open a level of the game...")) {
            if (ask_save_changes()) {
                s_disc_world = g_ed.level.settings.theme;
                s_open_disc_dialog = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) save();
        if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S")) save_as();
        ImGui::Separator();
        if (ImGui::MenuItem("Choose disc...")) choose_disc();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) g_ed.quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !g_ed.undo.empty())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !g_ed.redo.empty())) redo();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Icons", NULL, &g_ed.show_icons);
        ImGui::MenuItem("References (lines)", NULL, &g_ed.show_links);
        ImGui::MenuItem("Grid bounds", NULL, &g_ed.show_grid);
        ImGui::MenuItem("Build plane", "G", &g_ed.plane);
        if (ImGui::MenuItem("Look at the level", "F")) view_focus();
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Play (F5)")) play();
    if (ImGui::MenuItem("Help")) s_show_help = true;
    ImGui::EndMainMenuBar();
}

static void delete_selection()
{
    Block *b = g_ed.level.block(g_ed.sel_cell);
    if (!b) {
        return;
    }
    editor_snapshot();
    if (b->kind <= BLOCK_ACID && b->sides[g_ed.sel_side].id()) {
        Level::set_thing(&b->sides[g_ed.sel_side], THING_NONE);
    } else {
        g_ed.level.remove_block(g_ed.sel_cell);
        g_ed.sel_cell = -1;
    }
    editor_changed();
}

static void keys()
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
        if (ImGui::IsKeyPressed(ImGuiKey_N)) new_level();
        if (ImGui::IsKeyPressed(ImGuiKey_O)) open_file();
        if (ImGui::IsKeyPressed(ImGuiKey_S)) {
            if (io.KeyShift) save_as(); else save();
        }
        return;
    }
    for (int t = 0; t < TOOL_COUNT; t++) {
        if (ImGui::IsKeyPressed((ImGuiKey) (ImGuiKey_1 + t), false)) {
            g_ed.tool = t;
            g_ed.link_pick = PICK_NONE;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) play();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) view_focus();
    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) g_ed.plane = !g_ed.plane;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && g_ed.plane_y < GRID - 1) g_ed.plane_y++;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && g_ed.plane_y > 0) g_ed.plane_y--;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g_ed.link_pick != PICK_NONE) {
            g_ed.link_pick = PICK_NONE;
        } else {
            g_ed.sel_cell = -1;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        Block *b = g_ed.level.block(g_ed.sel_cell);
        if (b && b->kind <= BLOCK_ACID) {
            Thing &t = b->sides[g_ed.sel_side];
            const ThingInfo *info = thing_info(t.id());
            if (info && (info->flags & THING_ROTATES)) {
                editor_snapshot();
                t.w[1] = (uint16_t) (t.w[1] % 4 + 1);
                editor_changed();
            }
        }
    }
}

bool editor_init(ID3D11Device *device, ID3D11DeviceContext *context, void *hwnd)
{
    s_hwnd = (HWND) hwnd;
    if (!renderer_init(device, context)) {
        return false;
    }
    std::string error;
    if (!assets_open(NULL, &error)) {
        std::string own = ini_disc();
        if (own.empty() || !assets_open(own.c_str(), &error)) {
            editor_say("%s", error.c_str());
        }
    }
    g_ed.level.make_new();
    editor_world_changed();
    view_focus();
    update_title();
    return true;
}

void editor_shutdown()
{
    renderer_shutdown();
}

void editor_frame(int width, int height)
{
    if (!ImGui::IsAnyItemActive()) {
        s_frame_start = g_ed.level;
    }
    keys();
    menu_bar();
    std::vector<Level::Problem> problems = g_ed.level.check();
    float top = ImGui::GetFrameHeight();
    float bottom = ImGui::GetFrameHeight() + 6;
    left_panel(top, height - top - bottom);
    right_panel(top, height - top - bottom, (float) width, problems);
    status_bar((float) width, (float) height);
    disc_dialog();
    problems_dialog(problems);
    help_window();
    if (g_ed.link_pick != PICK_NONE) {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const char *text = "Click the target - Esc cancels";
        ImVec2 size = ImGui::CalcTextSize(text);
        ImVec2 p((width - size.x) / 2, top + 14);
        fg->AddRectFilled(ImVec2(p.x - 12, p.y - 6), ImVec2(p.x + size.x + 12, p.y + size.y + 6),
                          IM_COL32(20, 40, 70, 230), 6);
        fg->AddText(p, IM_COL32(255, 200, 120, 255), text);
    }
    view_frame(width, height);
}
