/*
 * level_file.h - a Roll Away level as the game keeps it (the data of one
 * entry of a world's .PAK, uncompressed) and the .rlv file the level
 * editor writes: a small header (the world the level is played in) and
 * that data.  A file of bare level data (as other Kula World editors
 * write it) is read too, as a level of the first world.
 *
 * The game's level data, 16-bit little-endian words:
 *   - 34 x 34 x 34 cells: word (x * 34 + z) * 34 + (33 - y), y up;
 *     0xFFFF empty, 0..4 a block (normal, fire, ice, invisible, acid),
 *     5.. a block whose properties are entry (value - 5) of the list;
 *   - at word 39304: number of blocks, 0, number of properties;
 *   - the properties, 128 words each (word 0 the kind); the level's
 *     information (kind 666: time) follows them, not counted.
 */
#ifndef LEVEL_LEVEL_FILE_H
#define LEVEL_LEVEL_FILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEVEL_GRID              34
#define LEVEL_CELLS             (LEVEL_GRID * LEVEL_GRID * LEVEL_GRID)
#define LEVEL_PROPERTY_WORDS    128
#define LEVEL_MAX_PROPERTIES    1024
/* bytes: the cells, the 3 counts, the properties, the information */
#define LEVEL_MIN_BYTES         ((LEVEL_CELLS + 3 + LEVEL_PROPERTY_WORDS) * 2)
#define LEVEL_MAX_BYTES         ((LEVEL_CELLS + 3 + (LEVEL_MAX_PROPERTIES + 2) * LEVEL_PROPERTY_WORDS) * 2)

#define LEVEL_THEME_COUNT       10

/* the worlds in the game's order (their folders on the disc) */
extern const char *const g_level_theme_dirs[LEVEL_THEME_COUNT];
/* their names for people */
extern const char *const g_level_theme_names[LEVEL_THEME_COUNT];

#define LEVEL_FILE_MAGIC        "RALV"
#define LEVEL_FILE_VERSION      1
#define LEVEL_NAME_SIZE         48

/* LevelFile.type: how the game plays the level */
enum LevelType {
    LEVEL_TYPE_GAME_RULE = -1,          /* as the game decides: a bonus level if it has no keys */
    LEVEL_TYPE_NORMAL = 0,              /* keys open the exit (none: it is open at once) */
    LEVEL_TYPE_BONUS = 1                /* the game's bonus level (BONUS letters) */
};

typedef struct {
    int theme;                          /* 0..LEVEL_THEME_COUNT - 1 */
    int type;                           /* enum LevelType */
    char name[LEVEL_NAME_SIZE];         /* UTF-8, may be empty */
    uint8_t *data;                      /* the game's level data */
    int size;                           /* bytes */
} LevelFile;

/* Reads a .rlv (or bare level data); FALSE with a reason in `error`. */
int level_file_load(const char *path, LevelFile *out, char *error, int error_size);

/* Writes a .rlv; FALSE on failure. */
int level_file_save(const char *path, const LevelFile *level);

void level_file_free(LevelFile *level);

/* TRUE if `data` is level data the game can read: the counts agree with
 * the size, every cell refers to an existing property. */
int level_data_check(const uint8_t *data, int size, char *error, int error_size);

#ifdef __cplusplus
}
#endif

#endif /* LEVEL_LEVEL_FILE_H */
