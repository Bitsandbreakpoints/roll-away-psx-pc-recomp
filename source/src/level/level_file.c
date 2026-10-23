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

/* the header of a .rlv: magic, version, theme, data size, name */
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
    int capacity = LEVEL_MAX_BYTES + HEADER_SIZE;
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
        out->theme = buffer[6] | buffer[7] << 8;
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
    memmove(buffer, buffer + offset, (size_t) size);
    out->data = buffer;
    out->size = size;
    return 1;
}

int level_file_save(const char *path, const LevelFile *level)
{
    int total = HEADER_SIZE + level->size;
    uint8_t *buffer = (uint8_t *) calloc(1, (size_t) total);
    if (!buffer) {
        return 0;
    }
    memcpy(buffer, LEVEL_FILE_MAGIC, 4);
    buffer[4] = LEVEL_FILE_VERSION;
    buffer[6] = (uint8_t) level->theme;
    for (int i = 0; i < 4; i++) {
        buffer[8 + i] = (uint8_t) ((uint32_t) level->size >> (8 * i));
    }
    strncpy((char *) buffer + 12, level->name, LEVEL_NAME_SIZE - 1);
    memcpy(buffer + HEADER_SIZE, level->data, (size_t) level->size);
    int ok = file_write(path, buffer, total);
    free(buffer);
    return ok;
}

void level_file_free(LevelFile *level)
{
    free(level->data);
    level->data = NULL;
    level->size = 0;
}
