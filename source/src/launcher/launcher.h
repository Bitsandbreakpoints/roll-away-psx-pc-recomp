/*
 * launcher.h - the first screen: choose the disc image, change the
 * settings, start the game.
 */
#ifndef LAUNCHER_LAUNCHER_H
#define LAUNCHER_LAUNCHER_H

/* launcher_frame() result */
enum LauncherAction {
    LAUNCHER_STAY = 0,
    LAUNCHER_PLAY,
    LAUNCHER_QUIT
};

/* Loads the background and checks the disc from the settings (if any). */
void launcher_init(void);
void launcher_shutdown(void);

/* Builds one frame of the launcher UI (between ui_begin_frame and
 * ui_end_frame); returns an enum LauncherAction. */
int launcher_frame(void);

/* TRUE while the game runs: the display settings apply to the window at
 * once (in the launcher it stays a splash window). */
void launcher_set_in_game(int in_game);

/* The settings screen alone (also used over the paused game). Returns
 * FALSE once the player closes it. */
int settings_screen(void);

/* TRUE while the settings wait for a key or button to assign (Esc then
 * cancels that, not the screen). */
int settings_capturing(void);

#endif /* LAUNCHER_LAUNCHER_H */
