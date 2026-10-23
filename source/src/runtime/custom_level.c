/*
 * custom_level.c - the player's own level in place of the game's
 * (custom_level.h).
 *
 * A world's .PAK holds its levels zlib-compressed: a count n, n pairs
 * (offset, size), n offsets of their names ("LEVEL 1\n"), the names,
 * then the data.  The replacement keeps the header and names and points
 * every entry at the one compressed copy of the player's level - except
 * the last of a world's file, "OBJ LEVEL" (or "LESSON", the tutorial),
 * which the game uses for other things and which is kept.  The game
 * reads a .PAK by its size on the disc, so the replacement must fit in
 * the file's sectors (it does: a level compresses to a few kB).
 */
#include "runtime/custom_level.h"
#include "level/level_file.h"
#include "disc/disc.h"
#include "psx/log.h"
#include "runtime/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (miniz, built with libchdr) */
int mz_compress2(unsigned char *dest, unsigned long *dest_len, const unsigned char *source,
                 unsigned long source_len, int level);

#define MAX_FILES               32

typedef struct {
    uint32_t lba, sectors;
    uint8_t *data;                      /* sectors * 2048 */
} Replaced;

static LevelFile s_level;
static int s_active;
static char s_name[LEVEL_NAME_SIZE + 64];
static Replaced s_files[MAX_FILES];
static int s_file_count;
static int s_started;

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static void forget_files(void)
{
    for (int i = 0; i < s_file_count; i++) {
        free(s_files[i].data);
    }
    s_file_count = 0;
}

int custom_level_set(const char *path, char *error, int error_size)
{
    forget_files();
    level_file_free(&s_level);
    s_active = 0;
    s_name[0] = 0;
    if (!path || !path[0]) {
        return 1;
    }
    if (!level_file_load(path, &s_level, error, error_size)) {
        return 0;
    }
    s_active = 1;
    if (s_level.name[0]) {
        snprintf(s_name, sizeof(s_name), "%s", s_level.name);
    } else {
        const char *base = path;
        for (const char *p = path; *p; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        snprintf(s_name, sizeof(s_name), "%s", base);
        char *dot = strrchr(s_name, '.');
        if (dot) {
            *dot = 0;
        }
    }
    psx_log("custom level: %s (%s, %d bytes)", s_name, g_level_theme_names[s_level.theme],
            s_level.size);
    return 1;
}

int custom_level_active(void)
{
    return s_active;
}

const char *custom_level_name(void)
{
    return s_name;
}

/* (hooks.c) the level's type (level_file.h LevelType) */
int custom_level_type(void)
{
    return s_active ? s_level.type : LEVEL_TYPE_GAME_RULE;
}

/* (hooks.c) the world the game is to load, -1 for its own choice */
int custom_level_world(void)
{
    return s_active ? s_level.theme : -1;
}

/* the game's "a game is on" word (0 in the title and menus, 1 in a
 * game), per version; found by comparing the memory in the menus and in
 * a game */
static uint32_t playing_flag(void)
{
    static const struct { const char *exe; uint32_t address; } FLAGS[] = {
        { "SLUS_007.24", 0x800A3438 }, { "SCES_010.00", 0x800A2EC0 }, { "SCPS_100.64", 0x800A1E34 },
    };
    for (int i = 0; i < (int) (sizeof(FLAGS) / sizeof(FLAGS[0])); i++) {
        if (g_rt_module && !strcmp(g_rt_module->disc_name, FLAGS[i].exe)) {
            return FLAGS[i].address;
        }
    }
    return 0;
}

int custom_level_started(void)
{
    if (s_active && !s_started) {
        uint32_t address = playing_flag();
        const uint32_t *word = address ? (const uint32_t *) rt_pointer(address) : NULL;
        if (!word || *word == 1) {
            s_started = 1;
            psx_log("custom level: a game is on");
        }
    }
    return s_started;
}

/* One .PAK: `packed` is the player's level, compressed. */
static void replace_pak(const char *path, const uint8_t *packed, uint32_t packed_size)
{
    uint32_t lba, size;
    if (s_file_count >= MAX_FILES || !disc_find_file(path, &lba, &size)) {
        return;
    }
    uint32_t sectors = (size + DISC_SECTOR_DATA - 1) / DISC_SECTOR_DATA;
    uint32_t room = sectors * DISC_SECTOR_DATA;
    uint8_t *original = (uint8_t *) calloc(sectors, DISC_SECTOR_DATA);
    uint8_t *made = (uint8_t *) calloc(sectors, DISC_SECTOR_DATA);
    if (!original || !made || !disc_read_sectors(lba, sectors, original)) {
        free(original);
        free(made);
        return;
    }
    uint32_t n = get32(original);
    uint32_t names_end = n && n <= 64 ? get32(original + 4) : 0;    /* the first entry's data */
    if (n == 0 || n > 64 || names_end > size || names_end < 4 + 12 * n) {
        psx_log("custom level: %s is not a level archive", path);
        free(original);
        free(made);
        return;
    }
    /* the last entry is kept if it is not a level */
    uint32_t last_name = get32(original + 4 + 8 * n + 4 * (n - 1));
    int keep_last = last_name + 6 < size
                    && (!strncmp((char *) original + last_name, "OBJ", 3)
                        || !strncmp((char *) original + last_name, "LESSON", 6));
    memcpy(made, original, names_end);
    uint32_t level_at = (names_end + 3) & ~3u;
    uint32_t at = level_at + packed_size;
    if (keep_last) {
        uint32_t off = get32(original + 4 + 8 * (n - 1)), len = get32(original + 8 + 8 * (n - 1));
        at = (at + 3) & ~3u;
        if (off + len > size || at + len > room) {
            at = room + 1;                                  /* (does not fit) */
        } else {
            memcpy(made + at, original + off, len);
            put32(made + 4 + 8 * (n - 1), at);
            at += len;
        }
    }
    free(original);
    if (at > room) {
        psx_log("custom level: the level does not fit in %s (%u bytes compressed, %u sectors); "
                "its levels stay the game's", path, packed_size, sectors);
        free(made);
        return;
    }
    memcpy(made + level_at, packed, packed_size);
    for (uint32_t i = 0; i < n - (uint32_t) keep_last; i++) {
        put32(made + 4 + 8 * i, level_at);
        put32(made + 8 + 8 * i, packed_size);
    }
    Replaced *r = &s_files[s_file_count++];
    r->lba = lba;
    r->sectors = sectors;
    r->data = made;
}

void custom_level_prepare(void)
{
    forget_files();
    s_started = 0;
    if (!s_active) {
        return;
    }
    unsigned long packed_size = (unsigned long) s_level.size + s_level.size / 100 + 1024;
    uint8_t *packed = (uint8_t *) malloc(packed_size);
    if (!packed || mz_compress2(packed, &packed_size, s_level.data, (unsigned long) s_level.size, 9)) {
        psx_log("custom level: cannot compress the level");
        free(packed);
        return;
    }
    char path[64];
    for (int t = 0; t < LEVEL_THEME_COUNT; t++) {
        const char *dir = g_level_theme_dirs[t];
        snprintf(path, sizeof(path), "\\%s\\%s.PAK;1", dir, dir);
        replace_pak(path, packed, (uint32_t) packed_size);
        snprintf(path, sizeof(path), "\\%s\\%sFI.PAK;1", dir, dir);
        replace_pak(path, packed, (uint32_t) packed_size);
    }
    replace_pak("\\HIRO\\COPYCAT.PAK;1", packed, (uint32_t) packed_size);
    psx_log("custom level: %lu bytes compressed, %d archives replaced", packed_size, s_file_count);
    free(packed);
}

int custom_level_read_sector(uint32_t lba, void *out)
{
    for (int i = 0; i < s_file_count; i++) {
        const Replaced *r = &s_files[i];
        if (lba >= r->lba && lba < r->lba + r->sectors) {
            memcpy(out, r->data + (size_t) (lba - r->lba) * DISC_SECTOR_DATA, DISC_SECTOR_DATA);
            return 1;
        }
    }
    return 0;
}
