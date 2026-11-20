#include "input.h"
#include "nes.h"
#include <windows.h>
#include <mmsystem.h>

// Dynamic XInput definitions
typedef struct _XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD;

typedef struct _XINPUT_STATE {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000

typedef DWORD (WINAPI *XInputGetState_t)(DWORD dwUserIndex, XINPUT_STATE *pState);
static XInputGetState_t pXInputGetState = nullptr;
static HMODULE hXInput = nullptr;

static uint8_t keyboard_state = 0;
static uint8_t joystick_state = 0;

void input_init() {
    const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char *dll : dlls) {
        hXInput = LoadLibraryA(dll);
        if (hXInput) {
            pXInputGetState = (XInputGetState_t)GetProcAddress(hXInput, "XInputGetState");
            if (pXInputGetState) break;
            FreeLibrary(hXInput);
            hXInput = nullptr;
        }
    }
}

void input_shutdown() {
    if (hXInput) {
        FreeLibrary(hXInput);
        hXInput = nullptr;
        pXInputGetState = nullptr;
    }
}

void input_set_key(WPARAM vk, int down) {
    // Map keyboard keys to NES buttons:
    // bit 0: A
    // bit 1: B
    // bit 2: Select
    // bit 3: Start
    // bit 4: Up
    // bit 5: Down
    // bit 6: Left
    // bit 7: Right
    static const struct { int vk; int bit; } map[] = {
        {'Z', 0}, {'X', 1}, {VK_RSHIFT, 2}, {VK_RETURN, 3},
        {VK_UP, 4}, {VK_DOWN, 5}, {VK_LEFT, 6}, {VK_RIGHT, 7},
        {VK_SPACE, 0}, {'C', 0}, {'V', 1},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].vk == (int)vk) {
            if (down) keyboard_state |= (uint8_t)(1 << map[i].bit);
            else keyboard_state &= (uint8_t)~(1 << map[i].bit);
        }
    }
}

void input_reset_keys() {
    keyboard_state = 0;
}

static uint8_t poll_xinput() {
    if (!pXInputGetState) return 0;
    XINPUT_STATE state;
    for (DWORD i = 0; i < 4; i++) {
        if (pXInputGetState(i, &state) == 0) { // ERROR_SUCCESS
            uint8_t joy = 0;
            WORD btn = state.Gamepad.wButtons;

            // NES A = Gamepad A or Y
            if ((btn & XINPUT_GAMEPAD_A) || (btn & XINPUT_GAMEPAD_Y)) joy |= (1 << 0);
            // NES B = Gamepad B or X
            if ((btn & XINPUT_GAMEPAD_B) || (btn & XINPUT_GAMEPAD_X)) joy |= (1 << 1);
            // NES Select = Back
            if (btn & XINPUT_GAMEPAD_BACK) joy |= (1 << 2);
            // NES Start = Start
            if (btn & XINPUT_GAMEPAD_START) joy |= (1 << 3);

            // D-pad directions
            if (btn & XINPUT_GAMEPAD_DPAD_UP) joy |= (1 << 4);
            if (btn & XINPUT_GAMEPAD_DPAD_DOWN) joy |= (1 << 5);
            if (btn & XINPUT_GAMEPAD_DPAD_LEFT) joy |= (1 << 6);
            if (btn & XINPUT_GAMEPAD_DPAD_RIGHT) joy |= (1 << 7);

            // Analog thumbstick (deadzone ~ 10000)
            if (state.Gamepad.sThumbLY > 10000) joy |= (1 << 4);
            if (state.Gamepad.sThumbLY < -10000) joy |= (1 << 5);
            if (state.Gamepad.sThumbLX < -10000) joy |= (1 << 6);
            if (state.Gamepad.sThumbLX > 10000) joy |= (1 << 7);

            return joy;
        }
    }
    return 0;
}

static uint8_t poll_winmm() {
    UINT num_devs = joyGetNumDevs();
    if (num_devs == 0) return 0;

    JOYINFOEX ji = {};
    ji.dwSize = sizeof(JOYINFOEX);
    ji.dwFlags = JOY_RETURNALL;

    for (UINT i = 0; i < num_devs && i < 4; i++) {
        if (joyGetPosEx(i, &ji) == JOYERR_NOERROR) {
            uint8_t joy = 0;
            // Buttons: Button 1 (bit 0), Button 2 (bit 1), Button 3 (bit 2), Button 4 (bit 3)
            // Common layout:
            // Button 1 (A): NES A
            // Button 2 (B): NES B
            // Button 3 (X): NES B
            // Button 4 (Y): NES A
            if ((ji.dwButtons & 0x01) || (ji.dwButtons & 0x08)) joy |= (1 << 0); // A
            if ((ji.dwButtons & 0x02) || (ji.dwButtons & 0x04)) joy |= (1 << 1); // B
            // Select: Button 7 or 9
            if ((ji.dwButtons & 0x40) || (ji.dwButtons & 0x100)) joy |= (1 << 2);
            // Start: Button 8 or 10
            if ((ji.dwButtons & 0x80) || (ji.dwButtons & 0x200)) joy |= (1 << 3);

            // Axes (range ~0 to 65535, center ~32767)
            if (ji.dwYpos < 16384) joy |= (1 << 4);      // Up
            else if (ji.dwYpos > 49152) joy |= (1 << 5); // Down
            if (ji.dwXpos < 16384) joy |= (1 << 6);      // Left
            else if (ji.dwXpos > 49152) joy |= (1 << 7); // Right

            // POV Hat (D-pad on many DirectInput pads)
            // Value in hundredths of degrees: 0 = Up, 9000 = Right, 18000 = Down, 27000 = Left
            // 65535 = centered
            if (ji.dwPOV != 0xFFFF && ji.dwPOV <= 36000) {
                if (ji.dwPOV >= 31500 || ji.dwPOV <= 4500) joy |= (1 << 4);       // Up
                if (ji.dwPOV >= 4500 && ji.dwPOV <= 13500) joy |= (1 << 7);      // Right
                if (ji.dwPOV >= 13500 && ji.dwPOV <= 22500) joy |= (1 << 5);     // Down
                if (ji.dwPOV >= 22500 && ji.dwPOV <= 31500) joy |= (1 << 6);     // Left
            }

            if (joy != 0) return joy;
        }
    }
    return 0;
}

void input_poll() {
    uint8_t joy = poll_xinput();
    if (!joy) joy = poll_winmm();
    joystick_state = joy;
    controller_state = keyboard_state | joystick_state;
}
