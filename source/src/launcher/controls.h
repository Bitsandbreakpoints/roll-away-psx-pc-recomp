/*
 * controls.h - the players' controls (settings) as PS1 controller
 * buttons.
 */
#ifndef LAUNCHER_CONTROLS_H
#define LAUNCHER_CONTROLS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The XInput controller (0..3) a player uses, or -1 ("automatic": player
 * p takes the p-th connected controller). */
int controls_player_pad(int player);

/* The player's PS1 buttons as the controller reports them (active high,
 * 0x0001 Select ... 0x8000 Square); -1 if the player has no controller. */
int controls_psx_buttons(int player);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_CONTROLS_H */
