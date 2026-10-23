/*
 * hooks.c - small changes to the game's own decisions: each hook_<name>()
 * runs just before one instruction of the game (`hook` lines of
 * recomp/config/main.cfg) and changes the registers it is about to test.
 */
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "psx/log.h"

int g_save_every_level;

/*
 * 0x41568, after a level: the game offers to save only when the level's
 * number in the world (a0, 0..14) is a multiple of 5 - it tests
 * `bne a0, v0` with v0 the number rounded down to one.  With the setting
 * the test always passes, so the save screen comes after every level
 * (everything else the game checks, e.g. no bonus level, stays).
 */
void hook_save_every_level(void)
{
    static uint32_t logged = 0xFFFFFFFF;
    if (GPR[4] != logged) {
        logged = GPR[4];
        psx_log("save check before level %d of the world: %s", (int) GPR[4] + 1,
                g_save_every_level ? "every level" : GPR[4] == GPR[2] ? "a save point" : "no");
    }
    if (g_save_every_level) {
        GPR[2] = GPR[4];
    }
}

/* (custom_level.c) */
int custom_level_world(void);

/*
 * 0x403B8 and 0x403D4 (USA), just after the game has read the world it
 * is to load next (a0): with a level of the player's, that level's world
 * is loaded instead, for its textures, sky and music.
 */
void hook_custom_level_world(void)
{
    int world = custom_level_world();
    if (world >= 0) {
        GPR[4] = (uint32_t) world;
    }
}

/*
 * 0x4CCF8 (USA), the start of the function that loads a file of a world
 * (a0 the world, a1 the kind: 0 .TGI, 1 .SFX, 2 .PAK, 3 .GGI, 4 FI.PAK):
 * with a level of the player's, the textures and levels are its world's
 * (.SFX and .GGI are the first world's alone, for all of them).
 */
void hook_custom_level_file(void)
{
    int world = custom_level_world();
    if (world >= 0 && GPR[4] < 10 && (GPR[5] == 0 || GPR[5] == 2 || GPR[5] == 4)) {
        GPR[4] = (uint32_t) world;
    }
}

/* (custom_level.c) */
int custom_level_type(void);

/*
 * 0x27BD4 and 0x35FC8 (USA), as a level is set up: the game makes it a
 * bonus level (its own look, the BONUS letters; the first test) and opens
 * the exit (the second) when the number of keys (v0) is 0.  A level of
 * the player's says which it is: a normal level without keys keeps its
 * look and has the exit open at once.
 */
void hook_custom_level_type(void)
{
    int type = custom_level_type();
    if (type == 0 && GPR[2] == 0) {
        GPR[2] = 1;                     /* (not 0 keys: no bonus level) */
    } else if (type == 1) {
        GPR[2] = 0;
    }
}
