/*
 * custom_level.c - the player's own levels in place of the game's
 * (custom_level.h).
 *
 * A world's .PAK holds its levels zlib-compressed: a count n, n pairs
 * (offset, size), n offsets of their names ("LEVEL 1\n"), the names,
 * then the data.  The replacement keeps the header and names, holds the
 * compressed levels of the player's chain (the first level, the levels
 * its exits lead to, theirs ...) and points every entry at the first -
 * except the last of a world's file, "OBJ LEVEL" (or "LESSON", the
 * tutorial), which the game uses for other things and which is kept.
 * The game reads a .PAK by its size on the disc, so the replacement must
 * fit in the file's sectors (a level compresses to a few kB; levels that
 * do not fit are left out and logged).
 *
 * The game keeps the .PAK of the world in memory (0x156000) and unpacks
 * a level from it whenever one starts; just before that (hook
 * custom_level_loading) the entries are pointed at the level of the chain
 * now played.  When a level is finished (hook custom_level_advance: the
 * game's "how it ended", 3 the hidden exit) the chain moves on to the
 * level that exit leads to, and if that one is of another world the
 * game is made to load it (its "load the world" flag).  In the title
 * and menus the chain is back at its first level.
 */
#include "runtime/custom_level.h"
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "level/level_file.h"
#include "disc/disc.h"
#include "psx/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (miniz, built with libchdr) */
int mz_compress2(unsigned char *dest, unsigned long *dest_len, const unsigned char *source,
                 unsigned long source_len, int level);

#define MAX_FILES               32
#define MAX_CHAIN               64
#define PAK_BUFFER              0x80156000u     /* the world's .PAK in the game's memory */
#define PAK_ROOM                0x7000u         /* (the level is unpacked at 0x15E000) */

typedef struct {
    LevelFile file;
    char path[LEVEL_PATH_SIZE];
    uint8_t *packed;
    uint32_t packed_size;
    int next, hidden;                   /* chain indices, -1 none */
} ChainLevel;

typedef struct {
    uint32_t lba, sectors;
    uint8_t *data;                      /* sectors * 2048 */
    uint32_t count, names_end;          /* its entries; where its names end */
    int keep_last;
    uint32_t at[MAX_CHAIN];             /* where each level of the chain is, 0 missing */
} Replaced;

/* the game's memory per version: "a game is on" (0 in the title and
 * menus, 1 in a game; found by comparing the memory there) and the
 * gp offset of "load the world" */
static const struct {
    const char *exe;
    uint32_t playing;
    uint32_t reload_world;
} VERSIONS[] = {
    { "SLUS_007.24", 0x800A3438, 0x4DC },
    { "SCES_010.00", 0x800A2EC0, 0x4CC },
    { "SCPS_100.64", 0x800A1E34, 0x4B4 },
};

static ChainLevel s_chain[MAX_CHAIN];
static int s_chain_count;
static int s_current;                   /* the level of the chain played now */
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

static int version_index(void)
{
    for (int i = 0; i < (int) (sizeof(VERSIONS) / sizeof(VERSIONS[0])); i++) {
        if (g_rt_module && !strcmp(g_rt_module->disc_name, VERSIONS[i].exe)) {
            return i;
        }
    }
    return -1;
}

static void forget_files(void)
{
    for (int i = 0; i < s_file_count; i++) {
        free(s_files[i].data);
    }
    s_file_count = 0;
}

static void forget_chain(void)
{
    for (int i = 0; i < s_chain_count; i++) {
        level_file_free(&s_chain[i].file);
        free(s_chain[i].packed);
    }
    s_chain_count = 0;
    s_current = 0;
}

static const char *base_name(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

/* the chain index of `path` (read and compressed if new), -1 if it cannot be read */
static int chain_add(const char *path, char *error, int error_size)
{
    char full[LEVEL_PATH_SIZE];
    if (!_fullpath(full, path, sizeof(full))) {
        snprintf(full, sizeof(full), "%s", path);
    }
    for (int i = 0; i < s_chain_count; i++) {
        if (!_stricmp(s_chain[i].path, full)) {
            return i;
        }
    }
    if (s_chain_count >= MAX_CHAIN) {
        snprintf(error, (size_t) error_size, "More than %d levels follow each other.", MAX_CHAIN);
        return -1;
    }
    ChainLevel *c = &s_chain[s_chain_count];
    memset(c, 0, sizeof(*c));
    if (!level_file_load(full, &c->file, error, error_size)) {
        return -1;
    }
    unsigned long size = (unsigned long) c->file.size + c->file.size / 100 + 1024;
    c->packed = (uint8_t *) malloc(size);
    if (!c->packed || mz_compress2(c->packed, &size, c->file.data, (unsigned long) c->file.size, 9)) {
        level_file_free(&c->file);
        free(c->packed);
        snprintf(error, (size_t) error_size, "The level cannot be compressed.");
        return -1;
    }
    c->packed_size = (uint32_t) size;
    snprintf(c->path, sizeof(c->path), "%s", full);
    c->next = c->hidden = -1;
    return s_chain_count++;
}

int custom_level_set(const char *path, char *error, int error_size)
{
    forget_files();
    forget_chain();
    s_active = 0;
    s_name[0] = 0;
    if (!path || !path[0]) {
        return 1;
    }
    if (chain_add(path, error, error_size) < 0) {
        return 0;
    }
    /* the levels the exits lead to, and theirs */
    for (int i = 0; i < s_chain_count; i++) {
        for (int part = 0; part < 2; part++) {
            const char *link = part ? s_chain[i].file.next_hidden : s_chain[i].file.next;
            if (!link[0]) {
                continue;
            }
            char target[LEVEL_PATH_SIZE], why[256];
            level_file_resolve(s_chain[i].path, link, target, sizeof(target));
            int k = chain_add(target, why, sizeof(why));
            if (k < 0) {
                psx_log("custom level: %s leads to %s: %s", base_name(s_chain[i].path), target, why);
            }
            *(part ? &s_chain[i].hidden : &s_chain[i].next) = k;
        }
    }
    s_active = 1;
    const LevelFile *first = &s_chain[0].file;
    if (first->name[0]) {
        snprintf(s_name, sizeof(s_name), "%s", first->name);
    } else {
        snprintf(s_name, sizeof(s_name), "%s", base_name(path));
        char *dot = strrchr(s_name, '.');
        if (dot) {
            *dot = 0;
        }
    }
    for (int i = 0; i < s_chain_count; i++) {
        psx_log("custom level %d: %s (%s, %u bytes compressed), exit -> %d, hidden exit -> %d", i,
                base_name(s_chain[i].path), g_level_theme_names[s_chain[i].file.theme],
                s_chain[i].packed_size, s_chain[i].next, s_chain[i].hidden);
    }
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

/* in the title and menus the chain starts again */
static void sync_with_game(void)
{
    int v = version_index();
    const uint32_t *playing = v >= 0 ? (const uint32_t *) rt_pointer(VERSIONS[v].playing) : NULL;
    if (playing && *playing != 1 && s_current) {
        s_current = 0;
        psx_log("custom level: back to the first level");
    }
}

/* (hooks.c) the level's type (level_file.h LevelType) */
int custom_level_type(void)
{
    return s_active ? s_chain[s_current].file.type : LEVEL_TYPE_GAME_RULE;
}

/* (hooks.c) the world the game is to load, -1 for its own choice */
int custom_level_world(void)
{
    if (!s_active) {
        return -1;
    }
    sync_with_game();
    return s_chain[s_current].file.theme;
}

/* (hooks.c) a level is about to be unpacked from the .PAK in memory:
 * its entries lead to the level of the chain played now */
void custom_level_loading(void)
{
    if (!s_active) {
        return;
    }
    sync_with_game();
    uint8_t *pak = (uint8_t *) rt_pointer(PAK_BUFFER);
    if (!pak) {
        return;
    }
    uint32_t n = get32(pak);
    for (int f = 0; f < s_file_count; f++) {
        const Replaced *r = &s_files[f];
        if (r->count != n || memcmp(pak + 4 + 8 * n, r->data + 4 + 8 * n, r->names_end - 4 - 8 * n)) {
            continue;
        }
        int level = r->at[s_current] ? s_current : 0;
        if (level != s_current) {
            psx_log("custom level: %s is not in this world's archive (too big): the first level instead",
                    base_name(s_chain[s_current].path));
        }
        for (uint32_t i = 0; i < n - (uint32_t) r->keep_last; i++) {
            put32(pak + 4 + 8 * i, r->at[level]);
            put32(pak + 8 + 8 * i, s_chain[level].packed_size);
        }
        return;
    }
}

/* (hooks.c) a level ended; `how` is the game's: > 0 finished, 3 by the
 * hidden exit */
void custom_level_advance(int how)
{
    if (!s_active || how <= 0) {
        return;
    }
    const ChainLevel *now = &s_chain[s_current];
    int next = how == 3 && now->hidden >= 0 ? now->hidden : now->next;
    if (next < 0) {
        return;                         /* (no level after it: it is played again) */
    }
    psx_log("custom level: %s finished%s, next %s", base_name(now->path), how == 3 ? " (hidden exit)" : "",
            base_name(s_chain[next].path));
    /* another world: the game loads it before the level (as after its
     * own last level of a world) */
    int v = version_index();
    if (s_chain[next].file.theme != now->file.theme && v >= 0) {
        WR32(GPR[28] + VERSIONS[v].reload_world, 1);
        psx_log("custom level: the world changes to %s", g_level_theme_names[s_chain[next].file.theme]);
    }
    s_current = next;
}

int custom_level_started(void)
{
    if (s_active && !s_started) {
        int v = version_index();
        const uint32_t *word = v >= 0 ? (const uint32_t *) rt_pointer(VERSIONS[v].playing) : NULL;
        if (!word || *word == 1) {
            s_started = 1;
            psx_log("custom level: a game is on");
        }
    }
    return s_started;
}

/* One .PAK with the chain's levels (as many as fit). */
static void replace_pak(const char *path)
{
    uint32_t lba, size;
    if (s_file_count >= MAX_FILES || !disc_find_file(path, &lba, &size)) {
        return;
    }
    uint32_t sectors = (size + DISC_SECTOR_DATA - 1) / DISC_SECTOR_DATA;
    uint32_t room = sectors * DISC_SECTOR_DATA;
    if (room > PAK_ROOM) {
        room = PAK_ROOM;
    }
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
    Replaced *r = &s_files[s_file_count];
    memset(r, 0, sizeof(*r));
    /* the last entry is kept if it is not a level */
    uint32_t last_name = get32(original + 4 + 8 * n + 4 * (n - 1));
    r->keep_last = last_name + 6 < size
                   && (!strncmp((char *) original + last_name, "OBJ", 3)
                       || !strncmp((char *) original + last_name, "LESSON", 6));
    memcpy(made, original, names_end);
    uint32_t at = (names_end + 3) & ~3u;
    if (r->keep_last) {
        uint32_t off = get32(original + 4 + 8 * (n - 1)), len = get32(original + 8 + 8 * (n - 1));
        if (off + len > size || at + len > room) {
            free(original);
            free(made);
            return;
        }
        memcpy(made + at, original + off, len);
        put32(made + 4 + 8 * (n - 1), at);
        at = (at + len + 3) & ~3u;
    }
    free(original);
    for (int i = 0; i < s_chain_count; i++) {
        if (at + s_chain[i].packed_size > room) {
            psx_log("custom level: %s does not fit in %s", base_name(s_chain[i].path), path);
            continue;
        }
        memcpy(made + at, s_chain[i].packed, s_chain[i].packed_size);
        r->at[i] = at;
        at = (at + s_chain[i].packed_size + 3) & ~3u;
    }
    if (!r->at[0]) {
        psx_log("custom level: the level does not fit in %s; its levels stay the game's", path);
        free(made);
        return;
    }
    for (uint32_t i = 0; i < n - (uint32_t) r->keep_last; i++) {
        put32(made + 4 + 8 * i, r->at[0]);
        put32(made + 8 + 8 * i, s_chain[0].packed_size);
    }
    r->lba = lba;
    r->sectors = sectors;
    r->data = made;
    r->count = n;
    r->names_end = names_end;
    s_file_count++;
}

void custom_level_prepare(void)
{
    forget_files();
    s_started = 0;
    s_current = 0;
    if (!s_active) {
        return;
    }
    char path[64];
    for (int t = 0; t < LEVEL_THEME_COUNT; t++) {
        const char *dir = g_level_theme_dirs[t];
        snprintf(path, sizeof(path), "\\%s\\%s.PAK;1", dir, dir);
        replace_pak(path);
        snprintf(path, sizeof(path), "\\%s\\%sFI.PAK;1", dir, dir);
        replace_pak(path);
    }
    replace_pak("\\HIRO\\COPYCAT.PAK;1");
    psx_log("custom level: %d level(s), %d archives replaced", s_chain_count, s_file_count);
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
