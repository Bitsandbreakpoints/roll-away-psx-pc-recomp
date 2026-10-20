/*
 * input.h - keyboard and XInput controllers, and the PS1 controller
 * buttons they are mapped to.
 */
#ifndef PLATFORM_INPUT_H
#define PLATFORM_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* The buttons of a PS1 (digital) controller */
enum PsxButton {
    PSX_UP, PSX_DOWN, PSX_LEFT, PSX_RIGHT,
    PSX_CROSS, PSX_CIRCLE, PSX_SQUARE, PSX_TRIANGLE,
    PSX_L1, PSX_R1, PSX_L2, PSX_R2,
    PSX_START, PSX_SELECT,
    PSX_BUTTON_COUNT
};

/* Inputs of an XInput controller (buttons, triggers and stick directions) */
enum PadInput {
    PAD_NONE = -1,
    PAD_DPAD_UP, PAD_DPAD_DOWN, PAD_DPAD_LEFT, PAD_DPAD_RIGHT,
    PAD_A, PAD_B, PAD_X, PAD_Y,
    PAD_LB, PAD_RB, PAD_LT, PAD_RT,
    PAD_START, PAD_BACK, PAD_L3, PAD_R3,
    PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT,
    PAD_INPUT_COUNT
};

#define INPUT_MAX_PADS          4       /* XInput supports four controllers */

/* Reads the controllers and the keyboard; call once per frame. */
void input_poll(void);

int input_pad_connected(int pad);
int input_pad_down(int pad, int pad_input);     /* enum PadInput */
int input_key_down(int vk);                      /* only while our window has the focus */

/* A key / pad input pressed since the last poll (for "press a button"),
 * or 0 / PAD_NONE. */
int input_new_key(void);
int input_new_pad_input(int pad);

/* Names for the settings screen (UTF-8, static buffers). */
const char *input_key_name(int vk);
const char *input_pad_input_name(int pad_input);
const char *input_psx_button_name(int button);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_INPUT_H */
