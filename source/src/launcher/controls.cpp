/*
 * controls.cpp - the players' controls (settings) as PS1 controller
 * buttons.
 *
 * "Automatic" uses the player's XInput controller if one is connected;
 * player 1 can always use the keyboard as well.
 */
#include <windows.h>
#include "launcher/controls.h"
#include "launcher/settings.h"
#include "platform/input.h"

/* the controller's button bits, in enum PsxButton order */
static const int PSX_BUTTON_BITS[PSX_BUTTON_COUNT] = {
    0x0010, 0x0040, 0x0080, 0x0020,     /* up, down, left, right */
    0x4000, 0x2000, 0x8000, 0x1000,     /* cross, circle, square, triangle */
    0x0400, 0x0800, 0x0100, 0x0200,     /* L1, R1, L2, R2 */
    0x0008, 0x0001,                     /* start, select */
};

/* the stick also works as the D-pad, if the player wants */
static const int STICK_FOR_DPAD[4] = {
    PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT,
};

int controls_player_pad(int player)
{
    int device = g_settings_pc.players[player].device;
    if (device >= DEVICE_PAD_1 && device <= DEVICE_PAD_4) {
        return device - DEVICE_PAD_1;
    }
    if (device == DEVICE_AUTO) {
        int n = 0;
        for (int i = 0; i < INPUT_MAX_PADS; i++) {
            if (input_pad_connected(i) && n++ == player) {
                return i;
            }
        }
    }
    return -1;
}

static int keyboard_buttons(const ControllerConfig *c)
{
    int buttons = 0;
    for (int b = 0; b < PSX_BUTTON_COUNT; b++) {
        if (c->keys[b] && input_key_down(c->keys[b])) {
            buttons |= PSX_BUTTON_BITS[b];
        }
    }
    return buttons;
}

static int pad_buttons(const ControllerConfig *c, int pad)
{
    int buttons = 0;
    for (int b = 0; b < PSX_BUTTON_COUNT; b++) {
        if (c->pad[b] != PAD_NONE && input_pad_down(pad, c->pad[b])) {
            buttons |= PSX_BUTTON_BITS[b];
        }
    }
    if (c->stick_as_dpad) {
        for (int d = 0; d < 4; d++) {
            if (input_pad_down(pad, STICK_FOR_DPAD[d])) {
                buttons |= PSX_BUTTON_BITS[PSX_UP + d];
            }
        }
    }
    return buttons;
}

int controls_psx_buttons(int player)
{
    const ControllerConfig *c = &g_settings_pc.players[player];
    int pad = controls_player_pad(player);

    switch (c->device) {
    case DEVICE_NONE:
        return -1;
    case DEVICE_KEYBOARD:
        return keyboard_buttons(c);
    case DEVICE_AUTO:
        if (pad >= 0) {
            return pad_buttons(c, pad) | (player == 0 ? keyboard_buttons(c) : 0);
        }
        return player == 0 ? keyboard_buttons(c) : -1;
    default:
        return pad >= 0 && input_pad_connected(pad) ? pad_buttons(c, pad) : -1;
    }
}
