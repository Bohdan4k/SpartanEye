// Metro Exodus EE NPC overlay: injected DLL (see injector/). Single-player debug overlay, read-only on game data.
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "MinHook.h"
#include "config.h"
#include "game.h"
#include "input.h"
#include "log.h"
#include "render.h"
#include "state.h"
#include "version.h"

std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_unload_requested{false};
std::atomic<int> g_active_calls{0};

static HMODULE g_self = nullptr;
static HANDLE g_unload_event = nullptr;

void request_unload()
{
    g_unload_requested.store(true);
    if (g_unload_event)
        SetEvent(g_unload_event);
}

static DWORD WINAPI main_thread(LPVOID)
{
    log_open(g_self);
    logf("metro_npc_overlay loaded (%s %s), pid %lu", MOD_TITLE, kVersionString, GetCurrentProcessId());
    wchar_t mtx_name[64];
    swprintf_s(mtx_name, L"Local\\MetroNpcOverlay_%lu", GetCurrentProcessId());
    HANDLE mtx = CreateMutexW(nullptr, FALSE, mtx_name);
    bool hooked = false;
    char err[256] = "";
    do
    {
        if (!mtx || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            logf("another copy of the overlay is already running in this process; exiting");
            break;
        }
        GameInit gi = game_init(err, sizeof(err));
        if (gi == GameInit::WrongBuild)
        {
            logf("refusing to run: %s", err);
            break;
        }
        if (gi == GameInit::MissingData)
            logf("game data unavailable (%s): the menu works, no markers", err);
        config_load();
        if (MH_Initialize() != MH_OK)
        {
            logf("MH_Initialize failed");
            break;
        }
        if (!render_create_hooks(err, sizeof(err)))
        {
            logf("render hooks failed: %s", err);
            MH_Uninitialize();
            break;
        }
        if (!input_create_hooks())
            logf("DirectInput hooks unavailable: menu uses the Windows cursor, game input is not blocked");
        // Enable everything created except the ExecuteCommandLists fallback (render.cpp enables it on demand).
        // Named so `metro_inject --unload` can request the same clean unload as the END key.
        wchar_t ev_name[64];
        swprintf_s(ev_name, L"Local\\MetroNpcOverlay_Unload_%lu", GetCurrentProcessId());
        g_unload_event = CreateEventW(nullptr, TRUE, FALSE, ev_name);
        if (!g_unload_event)
        {
            logf("CreateEvent failed");
            MH_Uninitialize();
            break;
        }
        // One atomic apply: enabling everything and then disabling the fallback would race with the render thread,
        // which may enable the fallback itself in between.
        MH_QueueEnableHook(MH_ALL_HOOKS);
        render_queue_fallback_off();
        MH_STATUS st = MH_ApplyQueued();
        if (st != MH_OK)
        {
            logf("MH_ApplyQueued failed: %s", MH_StatusToString(st));
            MH_Uninitialize();
            break;
        }
        hooked = true;
        logf("hooks enabled. INSERT = menu, END = unload");
    } while (false);

    if (hooked)
    {
        WaitForSingleObject(g_unload_event, INFINITE);
        logf("unloading: disabling hooks");
        MH_DisableHook(MH_ALL_HOOKS);
        Sleep(250);  // let threads that just entered a detour register in g_active_calls
        int waited = 0;
        while (g_active_calls.load() > 0 && waited < 3000)
        {
            Sleep(10);
            waited += 10;
        }
        if (g_active_calls.load() > 0)
        {
            // A thread is still inside our code: freeing the DLL would crash the game. Stay loaded, inert.
            logf("unload aborted: %d thread(s) still inside a hook; DLL stays loaded but inactive",
                 g_active_calls.load());
            log_close();
            return 0;
        }
        config_flush();
        render_shutdown();
        MH_Uninitialize();
        logf("unloaded cleanly");
    }
    log_close();
    if (g_unload_event)
        CloseHandle(g_unload_event);
    if (mtx)
        CloseHandle(mtx);
    FreeLibraryAndExitThread(g_self, 0);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        HANDLE t = CreateThread(nullptr, 0, main_thread, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
