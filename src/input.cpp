#include "input.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>

#include "MinHook.h"
#include "log.h"
#include "state.h"

namespace
{
// IDirectInputDevice8 vtable slots
constexpr int kVtGetCapabilities = 3;
constexpr int kVtGetDeviceState = 9;
constexpr int kVtGetDeviceData = 10;

using GetDeviceState_t = HRESULT(WINAPI*)(void* self, DWORD cb, LPVOID data);
using GetDeviceData_t = HRESULT(WINAPI*)(void* self, DWORD cb, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout, DWORD flags);
using GetCapabilities_t = HRESULT(WINAPI*)(void* self, LPDIDEVCAPS caps);

// Up to two distinct targets each (A and W interfaces may share code).
GetDeviceState_t o_gds[2] = {};
GetDeviceData_t o_gdd[2] = {};

std::atomic<int> g_dx{0}, g_dy{0};
std::atomic<uint32_t> g_buttons{0};          // bit0 = left, bit1 = right
std::atomic<ULONGLONG> g_mouse_tick{0};      // GetTickCount64 of the last DirectInput mouse read
// Overlay hotkeys, each seen through DirectInput (immediate or buffered) and GetAsyncKeyState.
struct Hotkey
{
    std::atomic<DWORD> dik;             // DIK_* scan code (derived from vk)
    std::atomic<int> vk;                // VK_* virtual key
    std::atomic<uint32_t> presses{0};   // key-down edges seen by the DirectInput hooks (game input thread)
    bool di_down = false;               // previous immediate-mode state (game input thread)
    uint32_t seen = 0;                  // render thread: presses already consumed
    bool async_down = false;            // render thread
    ULONGLONG last_fire = 0;            // render thread: debounce
};
Hotkey g_keys[3] = {{0xD2, VK_INSERT}, {0xCF, VK_END}, {0xC7, VK_HOME}};  // menu, unload, report

// Device type cache: 0 = unknown, 1 = mouse, 2 = keyboard, 3 = other.
struct DevType
{
    void* dev;
    int type;
};
DevType g_dev_types[16] = {};
SRWLOCK g_dev_lock = SRWLOCK_INIT;

int device_type(void* dev)
{
    AcquireSRWLockShared(&g_dev_lock);
    for (auto& d : g_dev_types)
        if (d.dev == dev)
        {
            int t = d.type;
            ReleaseSRWLockShared(&g_dev_lock);
            return t;
        }
    ReleaseSRWLockShared(&g_dev_lock);
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    auto getcaps = reinterpret_cast<GetCapabilities_t>((*reinterpret_cast<void***>(dev))[kVtGetCapabilities]);
    int type = 3;
    if (SUCCEEDED(getcaps(dev, &caps)))
    {
        BYTE t = GET_DIDEVICE_TYPE(caps.dwDevType);
        type = (t == DI8DEVTYPE_MOUSE) ? 1 : (t == DI8DEVTYPE_KEYBOARD) ? 2 : 3;
    }
    AcquireSRWLockExclusive(&g_dev_lock);
    for (auto& d : g_dev_types)
        if (!d.dev)
        {
            d = {dev, type};
            break;
        }
    ReleaseSRWLockExclusive(&g_dev_lock);
    return type;
}

void note_key(DWORD dik, bool down)
{
    if (!down)
        return;
    for (auto& k : g_keys)
        if (k.dik.load() == dik)
            k.presses.fetch_add(1);
}

void filter_state(DWORD cb, void* data)
{
    if (cb == 256)  // keyboard
    {
        auto keys = static_cast<BYTE*>(data);
        for (auto& k : g_keys)
        {
            bool d = (keys[k.dik.load() & 0xFF] & 0x80) != 0;
            if (d && !k.di_down)
                k.presses.fetch_add(1);
            k.di_down = d;
        }
        if (g_menu_open.load())
            memset(data, 0, 256);
    }
    else if (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2))  // mouse
    {
        auto ms = static_cast<DIMOUSESTATE*>(data);
        g_mouse_tick.store(GetTickCount64());
        if (g_menu_open.load())
        {
            g_dx.fetch_add(ms->lX);
            g_dy.fetch_add(ms->lY);
            g_buttons.store((ms->rgbButtons[0] & 0x80 ? 1u : 0u) | (ms->rgbButtons[1] & 0x80 ? 2u : 0u));
            memset(data, 0, cb);
        }
    }
}

void filter_data(void* dev, DWORD cb, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout)
{
    if (!rgdod || !inout || cb < sizeof(DWORD) * 2)
        return;
    int type = device_type(dev);
    if (type != 1 && type != 2)
        return;
    const bool menu = g_menu_open.load();
    auto at = [&](DWORD i) { return reinterpret_cast<LPDIDEVICEOBJECTDATA>(reinterpret_cast<BYTE*>(rgdod) + i * cb); };
    DWORD kept = 0;
    if (type == 1)
        g_mouse_tick.store(GetTickCount64());
    for (DWORD i = 0; i < *inout; ++i)
    {
        LPDIDEVICEOBJECTDATA ev = at(i);
        bool keep = !menu;
        if (type == 2)
        {
            bool down = (ev->dwData & 0x80) != 0;
            note_key(ev->dwOfs, down);
            keep = keep || !down;  // key releases always reach the game, so no key sticks
        }
        else
        {
            if (menu)
            {
                if (ev->dwOfs == DIMOFS_X)
                    g_dx.fetch_add(static_cast<LONG>(ev->dwData));
                else if (ev->dwOfs == DIMOFS_Y)
                    g_dy.fetch_add(static_cast<LONG>(ev->dwData));
                else if (ev->dwOfs == DIMOFS_BUTTON0 || ev->dwOfs == DIMOFS_BUTTON1)
                {
                    uint32_t bit = ev->dwOfs == DIMOFS_BUTTON0 ? 1u : 2u;
                    if (ev->dwData & 0x80)
                        g_buttons.fetch_or(bit);
                    else
                        g_buttons.fetch_and(~bit);
                }
            }
            bool is_button = ev->dwOfs >= DIMOFS_BUTTON0 && ev->dwOfs <= DIMOFS_BUTTON7;
            keep = keep || (is_button && !(ev->dwData & 0x80));
        }
        if (keep)
        {
            if (kept != i)
                memcpy(at(kept), ev, cb);
            ++kept;
        }
    }
    *inout = kept;
}

template <int I>
HRESULT WINAPI hk_gds(void* self, DWORD cb, LPVOID data)
{
    ActiveCall ac;
    HRESULT hr = o_gds[I](self, cb, data);
    if (SUCCEEDED(hr) && data)
        filter_state(cb, data);
    return hr;
}

template <int I>
HRESULT WINAPI hk_gdd(void* self, DWORD cb, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout, DWORD flags)
{
    ActiveCall ac;
    HRESULT hr = o_gdd[I](self, cb, rgdod, inout, flags);
    if (SUCCEEDED(hr))
        filter_data(self, cb, rgdod, inout);
    return hr;
}

bool device_vtable(REFIID iid, void**& vt, IUnknown*& di_out, IUnknown*& dev_out)
{
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (iid == IID_IDirectInput8W)
    {
        IDirectInput8W* di = nullptr;
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(DirectInput8Create(hinst, DIRECTINPUT_VERSION, iid, reinterpret_cast<void**>(&di), nullptr)))
            return false;
        if (FAILED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)))
        {
            di->Release();
            return false;
        }
        vt = *reinterpret_cast<void***>(dev);
        di_out = di;
        dev_out = dev;
        return true;
    }
    IDirectInput8A* di = nullptr;
    IDirectInputDevice8A* dev = nullptr;
    if (FAILED(DirectInput8Create(hinst, DIRECTINPUT_VERSION, iid, reinterpret_cast<void**>(&di), nullptr)))
        return false;
    if (FAILED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr)))
    {
        di->Release();
        return false;
    }
    vt = *reinterpret_cast<void***>(dev);
    di_out = di;
    dev_out = dev;
    return true;
}
}  // namespace

bool input_create_hooks()
{
    void* gds_targets[2] = {};
    void* gdd_targets[2] = {};
    int n_gds = 0, n_gdd = 0;
    const IID* iids[] = {&IID_IDirectInput8W, &IID_IDirectInput8A};
    for (const IID* piid : iids)
    {
        REFIID iid = *piid;
        void** vt = nullptr;
        IUnknown *di = nullptr, *dev = nullptr;
        if (!device_vtable(iid, vt, di, dev))
        {
            logf("DirectInput: could not create a dummy %s device", iid == IID_IDirectInput8W ? "W" : "A");
            continue;
        }
        void* gds = vt[kVtGetDeviceState];
        void* gdd = vt[kVtGetDeviceData];
        dev->Release();
        di->Release();
        if (n_gds < 2 && (n_gds == 0 || gds_targets[0] != gds))
            gds_targets[n_gds++] = gds;
        if (n_gdd < 2 && (n_gdd == 0 || gdd_targets[0] != gdd))
            gdd_targets[n_gdd++] = gdd;
    }
    void* gds_detours[2] = {reinterpret_cast<void*>(&hk_gds<0>), reinterpret_cast<void*>(&hk_gds<1>)};
    void* gdd_detours[2] = {reinterpret_cast<void*>(&hk_gdd<0>), reinterpret_cast<void*>(&hk_gdd<1>)};
    int ok = 0;
    for (int i = 0; i < n_gds; ++i)
    {
        MH_STATUS s = MH_CreateHook(gds_targets[i], gds_detours[i], reinterpret_cast<void**>(&o_gds[i]));
        logf("DirectInput GetDeviceState hook %d at %p: %s", i, gds_targets[i], MH_StatusToString(s));
        ok += s == MH_OK;
    }
    for (int i = 0; i < n_gdd; ++i)
    {
        MH_STATUS s = MH_CreateHook(gdd_targets[i], gdd_detours[i], reinterpret_cast<void**>(&o_gdd[i]));
        logf("DirectInput GetDeviceData hook %d at %p: %s", i, gdd_targets[i], MH_StatusToString(s));
        ok += s == MH_OK;
    }
    return ok > 0;
}

void input_set_keys(int vk_menu, int vk_report, int vk_unload)
{
    const int vks[3] = {vk_menu, vk_unload, vk_report};  // g_keys order
    for (int i = 0; i < 3; ++i)
    {
        // DirectInput codes are scan codes; extended keys (INSERT, HOME, arrows...) get bit 0x80 (DIK_INSERT = 0xD2).
        UINT sc = MapVirtualKeyW(static_cast<UINT>(vks[i]), MAPVK_VK_TO_VSC_EX);
        DWORD dik = (sc & 0xFF) | ((sc & 0xFF00) == 0xE000 ? 0x80u : 0u);
        g_keys[i].vk.store(vks[i]);
        g_keys[i].dik.store(dik);
    }
}

void input_poll(InputFrame& f, HWND game_hwnd)
{
    // GetAsyncKeyState only while the game window is in front (otherwise keys typed elsewhere would trigger us).
    const bool fg = game_hwnd && GetForegroundWindow() == game_hwnd;
    const ULONGLONG now = GetTickCount64();
    bool fired[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        Hotkey& k = g_keys[i];
        uint32_t p = k.presses.load();
        bool di_edge = p != k.seen;
        k.seen = p;
        bool a = fg && (GetAsyncKeyState(k.vk.load()) & 0x8000) != 0;
        bool a_edge = a && !k.async_down;
        k.async_down = a;
        // Both sources usually see the same press; the debounce keeps one press = one action.
        if ((di_edge || a_edge) && now - k.last_fire > 250)
        {
            fired[i] = true;
            k.last_fire = now;
        }
    }
    f.insert_pressed = fired[0];
    f.end_pressed = fired[1];
    f.report_pressed = fired[2];

    f.di_mouse = now - g_mouse_tick.load() < 500;
    f.dx = static_cast<float>(g_dx.exchange(0));
    f.dy = static_cast<float>(g_dy.exchange(0));
    if (f.di_mouse)
    {
        uint32_t b = g_buttons.load();
        f.lmb = (b & 1) != 0;
        f.rmb = (b & 2) != 0;
    }
    else
    {
        f.lmb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        f.rmb = fg && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    }
    if (!g_menu_open.load())
        g_buttons.store(0);
}
