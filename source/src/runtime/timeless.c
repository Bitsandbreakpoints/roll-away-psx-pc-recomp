/*
 * timeless.c - the setting "Timeless Arcade" (the launcher's Game tab):
 * an Arcade game without the time limit, and without the power-ups about
 * time (the hourglass that turns the time round, the clock that stops it).
 * All three versions; the hooks are `hook` lines of recomp/config/
 * main.cfg (USA), eu.cfg and jp.cfg (the addresses are listed there).
 *
 * An Arcade game is noted when it is chosen (USA and Europe: Arcade or
 * Load Game in the 1 Player menu - the saved games are Arcade games;
 * Japan: Start in the 1 Player menu, Load Game in the title's menu) and
 * forgotten in the title's menu (the demo starts from there) - Time Trial
 * (Time Attack) keeps its time.
 * In an Arcade game with the setting on the time does not run (the
 * count-down subtracts 0), when a level is unpacked (0x15E000) the
 * hourglasses and clocks on its blocks are taken away, and the HUD's
 * hourglass is not drawn: its three primitives (shadow, sand, frame; one
 * set per picture buffer) are linked into the ordering table keeping
 * their length byte, which is made 0 - an empty entry.
 */
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "psx/log.h"

#define LEVEL_BUFFER            0x8015E000u     /* the level, unpacked */

/* the level data (level/level_file.h) */
#define LEVEL_CELLS             (34 * 34 * 34)
#define PROPERTY_WORDS          128
#define THING_TIME_STOP         8
#define THING_TIMER_FLIP        35

int g_timeless_arcade;                  /* the setting */
static int s_arcade;                    /* an Arcade game is played */

static int timeless(void)
{
    return g_timeless_arcade && s_arcade;
}

static void note_arcade(int arcade)
{
    s_arcade = arcade;
    if (s_arcade && g_timeless_arcade) {
        psx_log("timeless: an Arcade game without the time limit");
    }
}

/* X in the 1 Player menu (USA 0x48BFC, Europe 0x4936C): the choice (v1) -
 * 0 Arcade, 1 Time Trial, 2 Load Game, 3 Back */
void hook_timeless_choose(void)
{
    note_arcade(GPR[3] == 0 || GPR[3] == 2);
}

/* Japan 0x47B2C, O in the 1 Player menu: the choice (v1) - 0 Start,
 * 1 Time Attack, 2 Exit */
void hook_timeless_choose_jp(void)
{
    note_arcade(GPR[3] == 0);
}

/* Japan 0x4770C, Load Game in the title's menu (each frame of the memory
 * card screen; the title's hook runs before it) */
void hook_timeless_load_jp(void)
{
    s_arcade = 1;
}

/* The title's menu (USA 0x48818, Europe 0x48F88, Japan 0x474A8, where
 * its handler starts) - no Arcade game (a demo comes from here) */
void hook_timeless_title(void)
{
    s_arcade = 0;
}

/* The time left minus the frames gone, a0 (USA 0x3B4AC, Europe 0x3BD28,
 * Japan 0x3BB04) */
void hook_timeless_time(void)
{
    if (timeless()) {
        GPR[4] = 0;
    }
}

/* The HUD's hourglass is linked into the ordering table (USA 0x1ED5C,
 * Europe 0x1F634: its last primitive at t1 + a1; Japan 0x1F81C: a3 + a1);
 * the other two lie 40 and 80 bytes before it.  Without the time limit
 * their length is 0 (nothing drawn), else as it was. */
#define HOURGLASS_PRIMS         3
#define HOURGLASS_PRIM_BYTES    40

static void hourglass(uint32_t last)
{
    static uint8_t length[HOURGLASS_PRIMS];
    for (int i = 0; i < HOURGLASS_PRIMS; i++) {
        uint8_t *tag_length = (uint8_t *) rt_pointer(last - (uint32_t) (i * HOURGLASS_PRIM_BYTES) + 3);
        if (!tag_length) {
            return;
        }
        if (timeless()) {
            if (*tag_length) {
                length[i] = *tag_length;
                *tag_length = 0;
            }
        } else if (!*tag_length && length[i]) {
            *tag_length = length[i];
        }
    }
}

void hook_timeless_hourglass(void)
{
    hourglass(GPR[9] + GPR[5]);
}

void hook_timeless_hourglass_jp(void)
{
    hourglass(GPR[7] + GPR[5]);
}

/* A level has been unpacked (USA 0x405A0, Europe 0x40D7C, Japan 0x400A0)
 * - no hourglasses or clocks */
void hook_timeless_level(void)
{
    if (!timeless()) {
        return;
    }
    uint16_t *level = (uint16_t *) rt_pointer(LEVEL_BUFFER);
    if (!level) {
        return;
    }
    static const uint16_t none[16] = { 0, 0xFFFF, 0xFFFF, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0xFFFF,
                                       0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    int count = level[LEVEL_CELLS + 2], removed = 0;
    for (int k = 0; k < count && k < 1024; k++) {
        uint16_t *p = level + LEVEL_CELLS + 3 + k * PROPERTY_WORDS;
        if (p[0] > 4) {
            continue;                   /* (only blocks carry things) */
        }
        for (int s = 0; s < 6; s++) {
            uint16_t *side = p + 1 + 16 * s;
            if (side[0] == THING_TIME_STOP || side[0] == THING_TIMER_FLIP) {
                memcpy(side, none, sizeof(none));
                removed++;
            }
        }
    }
    if (removed) {
        psx_log("timeless: %d time power-up(s) taken away", removed);
    }
}
