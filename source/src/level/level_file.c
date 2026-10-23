/*
 * level_file.c - reading and writing .rlv level files (level_file.h).
 */
#include "level/level_file.h"
#include "platform/files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const g_level_theme_dirs[LEVEL_THEME_COUNT] = {
    "HIRO", "HILLS", "INCA", "ARCTIC", "COWBOY", "FIELD", "ATLANT", "HAZE", "MARS", "HELL"
};

const char *const g_level_theme_names[LEVEL_THEME_COUNT] = {
    "Hiro", "Hills", "Inca", "Arctic", "Cowboy", "Field", "Atlantis", "Haze", "Mars", "Hell"
};

/* the header of a .rlv: magic, version, theme (low byte; the high byte:
 * 1 the type is given, 2 a bonus level), data size, name */
#define TYPE_GIVEN              0x100
#define TYPE_BONUS              0x200
#define HEADER_SIZE             (4 + 2 + 2 + 4 + LEVEL_NAME_SIZE)

static uint16_t word(const uint8_t *data, int index)
{
    return (uint16_t) (data[2 * index] | data[2 * index + 1] << 8);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static void fail(char *error, int error_size, const char *text)
{
    if (error && error_size > 0) {
        snprintf(error, (size_t) error_size, "%s", text);
    }
}

int level_data_check(const uint8_t *data, int size, char *error, int error_size)
{
    if (size < LEVEL_MIN_BYTES || size > LEVEL_MAX_BYTES || (size & 1)) {
        fail(error, error_size, "This is not a Roll Away level (wrong size).");
        return 0;
    }
    int properties = word(data, LEVEL_CELLS + 2);
    if (properties > LEVEL_MAX_PROPERTIES
        || (LEVEL_CELLS + 3 + properties * LEVEL_PROPERTY_WORDS) * 2 > size) {
        fail(error, error_size, "This is not a Roll Away level (its property list is cut short).");
        return 0;
    }
    for (int i = 0; i < LEVEL_CELLS; i++) {
        int cell = word(data, i);
        if (cell != 0xFFFF && cell >= 5 && cell - 5 >= properties) {
            fail(error, error_size, "This is not a Roll Away level (a block refers to a "
                 "missing property).");
            return 0;
        }
    }
    return 1;
}

int level_file_load(const char *path, LevelFile *out, char *error, int error_size)
{
    memset(out, 0, sizeof(*out));
    out->type = LEVEL_TYPE_GAME_RULE;
    int capacity = LEVEL_MAX_BYTES + HEADER_SIZE + 2 * (8 + LEVEL_PATH_SIZE);
    uint8_t *buffer = (uint8_t *) malloc((size_t) capacity);
    if (!buffer) {
        fail(error, error_size, "Out of memory.");
        return 0;
    }
    int length = file_read(path, buffer, capacity);
    if (length < 0) {
        free(buffer);
        fail(error, error_size, "The level file cannot be opened.");
        return 0;
    }
    int offset = 0, size = length;
    if (length >= HEADER_SIZE && !memcmp(buffer, LEVEL_FILE_MAGIC, 4)) {
        int version = buffer[4] | buffer[5] << 8;
        if (version > LEVEL_FILE_VERSION) {
            free(buffer);
            fail(error, error_size, "The level was made by a newer version of the editor.");
            return 0;
        }
        int theme = buffer[6] | buffer[7] << 8;
        out->theme = theme & 0xFF;
        out->type = !(theme & TYPE_GIVEN) ? LEVEL_TYPE_GAME_RULE
                    : (theme & TYPE_BONUS) ? LEVEL_TYPE_BONUS : LEVEL_TYPE_NORMAL;
        size = (int) read32(buffer + 8);
        memcpy(out->name, buffer + 12, LEVEL_NAME_SIZE);
        out->name[LEVEL_NAME_SIZE - 1] = 0;
        offset = HEADER_SIZE;
        if (size > length - offset) {
            free(buffer);
            fail(error, error_size, "The level file is cut short.");
            return 0;
        }
    }
    if (out->theme < 0 || out->theme >= LEVEL_THEME_COUNT) {
        out->theme = 0;
    }
    if (!level_data_check(buffer + offset, size, error, error_size)) {
        free(buffer);
        return 0;
    }
    /* the tagged parts after the data */
    for (int at = offset + size; offset && at + 8 <= length;) {
        uint32_t part = read32(buffer + at + 4);
        if (part > (uint32_t) (length - at - 8)) {
            break;
        }
        char *dest = !memcmp(buffer + at, "NEXT", 4) ? out->next
                     : !memcmp(buffer + at, "HIDN", 4) ? out->next_hidden : NULL;
        if (dest && part < LEVEL_PATH_SIZE) {
            memcpy(dest, buffer + at + 8, part);
            dest[part] = 0;
        }
        at += 8 + (int) part;
    }
    memmove(buffer, buffer + offset, (size_t) size);
    out->data = buffer;
    out->size = size;
    return 1;
}

int level_file_save(const char *path, const LevelFile *level)
{
    int next = (int) strlen(level->next), hidden = (int) strlen(level->next_hidden);
    int total = HEADER_SIZE + level->size + (next ? 8 + next : 0) + (hidden ? 8 + hidden : 0);
    uint8_t *buffer = (uint8_t *) calloc(1, (size_t) total);
    if (!buffer) {
        return 0;
    }
    memcpy(buffer, LEVEL_FILE_MAGIC, 4);
    buffer[4] = LEVEL_FILE_VERSION;
    buffer[6] = (uint8_t) level->theme;
    if (level->type != LEVEL_TYPE_GAME_RULE) {
        buffer[7] = (uint8_t) ((TYPE_GIVEN | (level->type == LEVEL_TYPE_BONUS ? TYPE_BONUS : 0)) >> 8);
    }
    for (int i = 0; i < 4; i++) {
        buffer[8 + i] = (uint8_t) ((uint32_t) level->size >> (8 * i));
    }
    strncpy((char *) buffer + 12, level->name, LEVEL_NAME_SIZE - 1);
    memcpy(buffer + HEADER_SIZE, level->data, (size_t) level->size);
    int at = HEADER_SIZE + level->size;
    for (int part = 0; part < 2; part++) {
        const char *text = part ? level->next_hidden : level->next;
        int n = (int) strlen(text);
        if (n) {
            memcpy(buffer + at, part ? "HIDN" : "NEXT", 4);
            for (int i = 0; i < 4; i++) {
                buffer[at + 4 + i] = (uint8_t) ((uint32_t) n >> (8 * i));
            }
            memcpy(buffer + at + 8, text, (size_t) n);
            at += 8 + n;
        }
    }
    int ok = file_write(path, buffer, total);
    free(buffer);
    return ok;
}

void level_file_resolve(const char *from, const char *path, char *out, int out_size)
{
    int full = path[0] == '/' || path[0] == '\\' || (path[0] && path[1] == ':');
    const char *slash = NULL;
    for (const char *p = from; *p; p++) {
        if (*p == '/' || *p == '\\') {
            slash = p;
        }
    }
    if (full || !slash) {
        snprintf(out, (size_t) out_size, "%s", path);
    } else {
        snprintf(out, (size_t) out_size, "%.*s%s", (int) (slash - from + 1), from, path);
    }
}

void level_file_free(LevelFile *level)
{
    free(level->data);
    level->data = NULL;
    level->size = 0;
}
