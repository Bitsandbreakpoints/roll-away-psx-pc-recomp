/*
 * disc_assets.cpp - textures and levels from the player's disc
 * (disc_assets.h).
 *
 * A world's .TGI ends with the pictures the game copies to the PS1's
 * video memory: rectangles (x, y, width, height in 16-bit units) and
 * their pixels.  The blocks are 64 x 64, 8 bits per pixel, from (0, 256)
 * on; their palettes are rows of (768, y).  The .PAK files hold the
 * levels, zlib-compressed (see runtime/custom_level.c).
 */
#include <windows.h>
#include "editor/disc_assets.h"
#include "disc/disc.h"
#include "level/level_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (miniz, built with libchdr) */
extern "C" void *tinfl_decompress_mem_to_heap(const void *source, size_t source_len,
                                              size_t *out_len, int flags);
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1

#define VRAM_W                  1024
#define VRAM_H                  512

static bool s_open;
static std::string s_path;

std::string assets_game_folder()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    char out[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, out, sizeof(out), NULL, NULL);
    return out;
}

/* the disc in the game's settings.ini ("[disc]", "path = ...") */
static std::string settings_disc()
{
    std::string ini = assets_game_folder() + "settings.ini";
    FILE *f = fopen(ini.c_str(), "rb");
    if (!f) {
        return "";
    }
    char line[2048];
    bool in_disc = false;
    std::string path;
    while (fgets(line, sizeof(line), f)) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
            *--end = 0;
        }
        if (line[0] == '[') {
            in_disc = !strcmp(line, "[disc]");
        } else if (in_disc && !strncmp(line, "path", 4)) {
            const char *eq = strchr(line, '=');
            if (eq) {
                eq++;
                while (*eq == ' ') {
                    eq++;
                }
                path = eq;
            }
        }
    }
    fclose(f);
    return path;
}

bool assets_open(const char *path, std::string *error)
{
    std::string want = path && path[0] ? path : settings_disc();
    if (want.empty()) {
        *error = "No disc chosen: choose your Roll Away disc in the game's launcher (or here).";
        return false;
    }
    if (s_open) {
        disc_close();
        s_open = false;
    }
    if (!disc_open(want.c_str())) {
        *error = "The disc cannot be read: " + want;
        return false;
    }
    uint32_t lba, size;
    if (!disc_find_file("\\HIRO\\HIRO.PAK;1", &lba, &size)) {
        disc_close();
        *error = "This is not a Roll Away / Kula World disc.";
        return false;
    }
    s_open = true;
    s_path = want;
    return true;
}

bool assets_ready()
{
    return s_open;
}

const std::string &assets_disc_path()
{
    return s_path;
}

static bool read_file(const char *path, std::vector<uint8_t> *out)
{
    uint32_t lba, size;
    if (!s_open || !disc_find_file(path, &lba, &size) || size > 16u * 1024 * 1024) {
        return false;
    }
    uint32_t sectors = (size + DISC_SECTOR_DATA - 1) / DISC_SECTOR_DATA;
    out->assign((size_t) sectors * DISC_SECTOR_DATA, 0);
    if (!disc_read_sectors(lba, sectors, out->data())) {
        return false;
    }
    out->resize(size);
    return true;
}

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t) (p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

/* a plausible picture rectangle at `p` */
static bool rect_at(const std::vector<uint8_t> &t, size_t p, uint32_t *bytes)
{
    if (p + 8 > t.size()) {
        return false;
    }
    int x = u16(&t[p]), y = u16(&t[p + 2]), w = u16(&t[p + 4]), h = u16(&t[p + 6]);
    if (x >= VRAM_W || y >= VRAM_H || w <= 0 || h <= 0 || x + w > VRAM_W || y + h > VRAM_H
        || w * h > 65536) {
        return false;
    }
    *bytes = 8 + (uint32_t) (w * h * 2);
    return true;
}

/* the pictures at the end of a .TGI, copied into a video memory */
static bool tgi_to_vram(const std::vector<uint8_t> &t, std::vector<uint16_t> *vram)
{
    size_t n = t.size(), start = n;
    for (size_t s = 0; s + 8 <= n && start == n; s += 4) {
        size_t p = s;
        int chunks = 0;
        uint32_t bytes;
        while (rect_at(t, p, &bytes)) {
            p += bytes;
            chunks++;
        }
        if (p == n && chunks > 3) {
            start = s;
        }
    }
    if (start == n) {
        return false;
    }
    vram->assign(VRAM_W * VRAM_H, 0);
    uint32_t bytes;
    for (size_t p = start; p < n && rect_at(t, p, &bytes); p += bytes) {
        int x = u16(&t[p]), y = u16(&t[p + 2]), w = u16(&t[p + 4]), h = u16(&t[p + 6]);
        for (int r = 0; r < h; r++) {
            memcpy(&(*vram)[(size_t) (y + r) * VRAM_W + x], &t[p + 8 + (size_t) r * w * 2],
                   (size_t) w * 2);
        }
    }
    return true;
}

static uint32_t colour15(uint16_t c)
{
    uint32_t r = (c & 31) * 255 / 31, g = (c >> 5 & 31) * 255 / 31, b = (c >> 10 & 31) * 255 / 31;
    return 0xFF000000u | b << 16 | g << 8 | r;
}

/* the 64 x 64, 8-bit cell (cx, cy) of the block pages with palette row `clut` */
static void cell_texture(const std::vector<uint16_t> &vram, int cx, int cy, int clut,
                         std::vector<uint32_t> *out)
{
    out->resize(WORLD_TEXTURE_SIZE * WORLD_TEXTURE_SIZE);
    const uint8_t *bytes = (const uint8_t *) vram.data();
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int index = bytes[((size_t) (256 + cy * 64 + y) * VRAM_W + cx * 32) * 2 + x];
            (*out)[y * 64 + x] = colour15(vram[(size_t) clut * VRAM_W + 768 + index]);
        }
    }
}

bool assets_world_textures(int theme, std::vector<uint32_t> textures[WORLD_TEX_COUNT])
{
    if (theme < 0 || theme >= LEVEL_THEME_COUNT) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "\\%s\\%s.TGI;1", g_level_theme_dirs[theme], g_level_theme_dirs[theme]);
    std::vector<uint8_t> tgi;
    std::vector<uint16_t> vram;
    if (!read_file(path, &tgi) || !tgi_to_vram(tgi, &vram)) {
        return false;
    }
    /* (the cells and palettes the game draws a plain block with) */
    cell_texture(vram, 0, 0, 1, &textures[WORLD_TEX_TOP]);
    cell_texture(vram, 1, 0, 2, &textures[WORLD_TEX_SIDE]);
    return true;
}

static const char *pak_path(int theme, int file, char *buffer, int size)
{
    const char *dir = g_level_theme_dirs[theme];
    if (file == 0) {
        snprintf(buffer, (size_t) size, "\\%s\\%s.PAK;1", dir, dir);
    } else if (file == 1) {
        snprintf(buffer, (size_t) size, "\\%s\\%sFI.PAK;1", dir, dir);
    } else {
        snprintf(buffer, (size_t) size, "\\HIRO\\COPYCAT.PAK;1");
    }
    return buffer;
}

bool assets_list_levels(int theme, std::vector<DiscLevel> *levels)
{
    levels->clear();
    for (int file = 0; file < (theme == 0 ? 3 : 2); file++) {
        char path[64];
        std::vector<uint8_t> pak;
        if (!read_file(pak_path(theme, file, path, sizeof(path)), &pak) || pak.size() < 4) {
            continue;
        }
        uint32_t n = u32(&pak[0]);
        if (n > 64 || 4 + 12 * n > pak.size()) {
            continue;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t name = u32(&pak[4 + 8 * n + 4 * i]);
            std::string text;
            for (uint32_t k = name; k < pak.size() && pak[k] >= ' '; k++) {
                text += (char) pak[k];
            }
            if (text.compare(0, 3, "OBJ") == 0) {
                continue;                   /* (not a level) */
            }
            levels->push_back({ text, file, (int) i });
        }
    }
    return !levels->empty();
}

bool assets_read_level(int theme, const DiscLevel &level, std::vector<uint8_t> *data)
{
    char path[64];
    std::vector<uint8_t> pak;
    if (!read_file(pak_path(theme, level.file, path, sizeof(path)), &pak) || pak.size() < 4) {
        return false;
    }
    uint32_t n = u32(&pak[0]);
    if ((uint32_t) level.index >= n || 4 + 8 * n > pak.size()) {
        return false;
    }
    uint32_t off = u32(&pak[4 + 8 * level.index]), len = u32(&pak[8 + 8 * level.index]);
    if (off + len > pak.size()) {
        return false;
    }
    size_t out_len = 0;
    void *raw = tinfl_decompress_mem_to_heap(&pak[off], len, &out_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!raw) {
        return false;
    }
    data->assign((uint8_t *) raw, (uint8_t *) raw + out_len);
    free(raw);
    return true;
}
