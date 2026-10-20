/*
 * input_win32.cpp - keyboard (GetAsyncKeyState) and XInput controllers.
 */
#include <windows.h>
#include <xinput.h>
#include <string.h>
#include "platform/input.h"
#include "platform/platform.h"

#define STICK_THRESHOLD         16000   /* of 32767: a stick direction counts as pressed */
#define TRIGGER_THRESHOLD       64      /* of 255 */

typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);

static XInputGetStateFn s_get_state;
static int s_tried_load;
static int s_connected[INPUT_MAX_PADS];
static unsigned s_pad_bits[INPUT_MAX_PADS];          /* bit per enum PadInput */
static unsigned s_pad_prev[INPUT_MAX_PADS];
static unsigned char s_keys[256];
static unsigned char s_keys_prev[256];
static int s_poll_count;

static void load_xinput(void)
{
    static const wchar_t *dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    s_tried_load = 1;
    for (const wchar_t *name : dlls) {
        HMODULE dll = LoadLibraryW(name);
        if (dll) {
            s_get_state = (XInputGetStateFn) (void *) GetProcAddress(dll, "XInputGetState");
            if (s_get_state) {
                return;
            }
        }
    }
}

static unsigned pad_bits(const XINPUT_GAMEPAD *g)
{
    static const struct { WORD mask; int input; } BUTTONS[] = {
        { XINPUT_GAMEPAD_DPAD_UP, PAD_DPAD_UP }, { XINPUT_GAMEPAD_DPAD_DOWN, PAD_DPAD_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, PAD_DPAD_LEFT }, { XINPUT_GAMEPAD_DPAD_RIGHT, PAD_DPAD_RIGHT },
        { XINPUT_GAMEPAD_A, PAD_A }, { XINPUT_GAMEPAD_B, PAD_B },
        { XINPUT_GAMEPAD_X, PAD_X }, { XINPUT_GAMEPAD_Y, PAD_Y },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, PAD_LB }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, PAD_RB },
        { XINPUT_GAMEPAD_START, PAD_START }, { XINPUT_GAMEPAD_BACK, PAD_BACK },
        { XINPUT_GAMEPAD_LEFT_THUMB, PAD_L3 }, { XINPUT_GAMEPAD_RIGHT_THUMB, PAD_R3 },
    };
    unsigned bits = 0;
    for (const auto &b : BUTTONS) {
        if (g->wButtons & b.mask) {
            bits |= 1u << b.input;
        }
    }
    if (g->bLeftTrigger > TRIGGER_THRESHOLD) bits |= 1u << PAD_LT;
    if (g->bRightTrigger > TRIGGER_THRESHOLD) bits |= 1u << PAD_RT;
    if (g->sThumbLY > STICK_THRESHOLD) bits |= 1u << PAD_LSTICK_UP;
    if (g->sThumbLY < -STICK_THRESHOLD) bits |= 1u << PAD_LSTICK_DOWN;
    if (g->sThumbLX < -STICK_THRESHOLD) bits |= 1u << PAD_LSTICK_LEFT;
    if (g->sThumbLX > STICK_THRESHOLD) bits |= 1u << PAD_LSTICK_RIGHT;
    return bits;
}

void input_poll(void)
{
    if (!s_tried_load) {
        load_xinput();
    }
    for (int i = 0; i < INPUT_MAX_PADS; i++) {
        XINPUT_STATE state;
        s_pad_prev[i] = s_pad_bits[i];
        s_pad_bits[i] = 0;
        /* polling a missing pad is slow: check unconnected ones once a second */
        if (!s_get_state || (!s_connected[i] && s_poll_count % PLATFORM_REFRESH_HZ != i)) {
            continue;
        }
        memset(&state, 0, sizeof(state));
        s_connected[i] = s_get_state(i, &state) == ERROR_SUCCESS;
        if (s_connected[i]) {
            s_pad_bits[i] = pad_bits(&state.Gamepad);
        }
    }
    memcpy(s_keys_prev, s_keys, sizeof(s_keys));
    int focused = GetForegroundWindow() == (HWND) platform_hwnd();
    for (int vk = 1; vk < 256; vk++) {
        s_keys[vk] = focused && (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
    }
    s_poll_count++;
}

int input_pad_connected(int pad)
{
    return pad >= 0 && pad < INPUT_MAX_PADS && s_connected[pad];
}

int input_pad_down(int pad, int pad_input)
{
    if (pad < 0 || pad >= INPUT_MAX_PADS || pad_input < 0) {
        return 0;
    }
    return (s_pad_bits[pad] >> pad_input) & 1;
}

int input_key_down(int vk)
{
    return vk > 0 && vk < 256 && s_keys[vk];
}

int input_new_key(void)
{
    for (int vk = 8; vk < 256; vk++) {
        /* skip mouse buttons and the generic Shift/Ctrl/Alt codes (the
         * left/right ones are reported too) */
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
            continue;
        }
        if (s_keys[vk] && !s_keys_prev[vk]) {
            return vk;
        }
    }
    return 0;
}

int input_new_pad_input(int pad)
{
    if (pad < 0 || pad >= INPUT_MAX_PADS) {
        return PAD_NONE;
    }
    unsigned pressed = s_pad_bits[pad] & ~s_pad_prev[pad];
    for (int i = 0; i < PAD_INPUT_COUNT; i++) {
        if (pressed & (1u << i)) {
            return i;
        }
    }
    return PAD_NONE;
}

const char *input_key_name(int vk)
{
    static char name[64];
    wchar_t wname[64];
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lparam = (LONG) (scan << 16);

    if (vk <= 0) {
        return "-";
    }
    switch (vk) {           /* extended keys need the extended bit for their name */
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU:
    case VK_DIVIDE:
        lparam |= 1 << 24;
        break;
    }
    if (GetKeyNameTextW(lparam, wname, 64) <= 0) {
        wsprintfW(wname, L"Key %d", vk);
    }
    WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), NULL, NULL);
    return name;
}

const char *input_pad_input_name(int pad_input)
{
    static const char *NAMES[PAD_INPUT_COUNT] = {
        "D-pad up", "D-pad down", "D-pad left", "D-pad right",
        "A", "B", "X", "Y",
        "LB", "RB", "LT", "RT",
        "Start", "Back", "Left stick (press)", "Right stick (press)",
        "Left stick up", "Left stick down",
        "Left stick left", "Left stick right",
    };
    return pad_input >= 0 && pad_input < PAD_INPUT_COUNT ? NAMES[pad_input] : "-";
}

const char *input_psx_button_name(int button)
{
    static const char *NAMES[PSX_BUTTON_COUNT] = {
        "Up", "Down", "Left", "Right",
        "Cross", "Circle", "Square", "Triangle",
        "L1", "R1", "L2", "R2",
        "Start", "Select",
    };
    return button >= 0 && button < PSX_BUTTON_COUNT ? NAMES[button] : "?";
}
