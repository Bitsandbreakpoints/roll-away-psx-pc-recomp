/*
 * disc_assets.h - what the level editor reads from the player's disc
 * (the same image the game uses, from the launcher's settings.ini): the
 * worlds' block textures and the game's own levels, to look at or start
 * from.  Nothing of the game is in the editor itself.
 */
#ifndef EDITOR_DISC_ASSETS_H
#define EDITOR_DISC_ASSETS_H

#include <stdint.h>
#include <string>
#include <vector>

#define WORLD_TEXTURE_SIZE      64

/* the textures of a world's blocks, 0xAABBGGRR */
enum WorldTexture {
    WORLD_TEX_TOP = 0,
    WORLD_TEX_SIDE,
    WORLD_TEX_COUNT
};

/* Opens the disc of settings.ini (next to the editor) or `path`; FALSE
 * with a reason. */
bool assets_open(const char *path, std::string *error);
bool assets_ready();
const std::string &assets_disc_path();

/* The block textures of world `theme` (LEVEL_THEME_COUNT of them); FALSE
 * if the disc has none. */
bool assets_world_textures(int theme, std::vector<uint32_t> textures[WORLD_TEX_COUNT]);

/* the game's levels of a world */
struct DiscLevel {
    std::string name;                   /* "LEVEL 1", "BONUS 2", ... */
    int file;                           /* 0 the world's .PAK, 1 its FI.PAK, 2 COPYCAT.PAK */
    int index;
};
bool assets_list_levels(int theme, std::vector<DiscLevel> *levels);
bool assets_read_level(int theme, const DiscLevel &level, std::vector<uint8_t> *data);

/* the settings.ini path of the game next to the editor */
std::string assets_game_folder();

#endif /* EDITOR_DISC_ASSETS_H */
