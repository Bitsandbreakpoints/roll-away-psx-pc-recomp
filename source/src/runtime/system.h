/*
 * system.h - runs the game (the recompiled PS1 program) inside the PC
 * port: what the PC side (main_pc.cpp) sees of it.
 */
#ifndef RUNTIME_SYSTEM_H
#define RUNTIME_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called once per PS1 frame (vertical blank): shows the frame, handles
 * the window and waits for the next 1/60 s.  Returns FALSE to stop the
 * game.
 */
typedef int (*PsxFrameHook)(void);

/*
 * The buttons of controller port 0 or 1 as the PS1 pad reports them
 * (active high, 0x0001 Select ... 0x8000 Square); -1: nothing connected.
 */
typedef int (*PsxPadReader)(int port);

void psx_set_pad_reader(PsxPadReader reader);

/* Receives the sound of one frame: interleaved stereo at 44.1 kHz. */
typedef void (*PsxAudioSink)(const int16_t *samples, int frames);

void psx_set_audio_sink(PsxAudioSink sink);

/*
 * Sound in real time: the sound device's own thread asks for each frame
 * of sound (psx_sound_frame()) when it needs it.  The game's code and the
 * sound never run at the same time: the game holds a lock and lets go of
 * it only while it waits for the vertical blank.  Off (the default), one
 * frame of sound is mixed at every vertical blank and handed to the
 * PsxAudioSink, in step with the game (recordings, tests).
 */
#define PSX_SOUND_FRAME_SAMPLES 735     /* 44100 / 60 */

void psx_set_sound_realtime(int on);

/* The next 1/60 s of sound (silence while no game runs). */
void psx_sound_frame(int16_t out[PSX_SOUND_FRAME_SAMPLES * 2]);

/* Volumes in percent (the settings). */
void psx_set_sound_volumes(int master, int music, int effects);

/* Frames that will not be shown (fast forward): not drawn, not heard. */
extern int g_psx_skip_frames;

/* Maps the PS1 memory; once at start-up.  FALSE if that fails. */
int psx_system_init(void);

/* Runs the game from power-on until the frame hook returns FALSE or the
 * game stops with an error (psx_fatal_message()).  The disc image must be
 * open (disc_open()). */
void psx_run_game(PsxFrameHook hook);

/* Why the game stopped, if it was an error (else NULL). */
const char *psx_fatal_message(void);

/* One vertical blank: the frame hook. */
void psx_vblank(void);

/* (runtime) the emulated devices: power-on state / the game has stopped */
void psx_devices_reset(void);
void psx_devices_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_SYSTEM_H */
