/*
 * custom_level.h - plays a level of the player's own (a .rlv from the
 * level editor) instead of the game's: the .PAK files of the worlds are
 * served with every level replaced by it, and the game is made to load
 * the level's world (its look and music).
 */
#ifndef RUNTIME_CUSTOM_LEVEL_H
#define RUNTIME_CUSTOM_LEVEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plays `path` (UTF-8) from the next game on; NULL or "" plays the
 * game's own levels again.  FALSE with a reason if it cannot be read. */
int custom_level_set(const char *path, char *error, int error_size);

/* TRUE while a level of the player's is chosen. */
int custom_level_active(void);

/* The chosen level's name (or its file's), for the launcher. */
const char *custom_level_name(void);

/* TRUE once the game has begun loading the level (its world): until
 * then the port presses the confirm button for the player, through the
 * intro and the menus to an Arcade game. */
int custom_level_started(void);

/* After the disc is opened: makes the replaced .PAK files. */
void custom_level_prepare(void);

/* CdRead: TRUE if sector `lba` is one of a replaced file (copied to
 * `out`, 2048 bytes). */
int custom_level_read_sector(uint32_t lba, void *out);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_CUSTOM_LEVEL_H */
