#pragma once
#include <windows.h>

// The game reads mouse and keyboard through DirectInput8 (imports DirectInput8Create). While the menu is open, the
// hooks below hand the game empty input (key-up / button-up events still pass, so nothing sticks) and collect mouse
// deltas for the menu cursor. INSERT / END / HOME are detected from DirectInput and from GetAsyncKeyState.

bool input_create_hooks();  // MinHook hooks on IDirectInputDevice8{A,W}::GetDeviceState/GetDeviceData (not enabled)

struct InputFrame
{
    bool insert_pressed = false;
    bool end_pressed = false;
    bool report_pressed = false;  // HOME
    bool di_mouse = false;  // DirectInput mouse data arrived recently -> use relative motion
    float dx = 0, dy = 0;   // accumulated DirectInput mouse motion since the last poll (menu open only)
    bool lmb = false, rmb = false;
};

void input_poll(InputFrame& f, HWND game_hwnd);  // render thread, once per frame
void input_set_keys(int vk_menu, int vk_report, int vk_unload);  // rebind (any thread)
