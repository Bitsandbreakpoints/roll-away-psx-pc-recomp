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
