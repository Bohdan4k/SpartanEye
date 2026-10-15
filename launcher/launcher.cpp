// Launcher: starts Metro Exodus Enhanced Edition through Steam (if it is not running yet), waits until the game is
// ready and loads metro_npc_overlay.dll (next to this exe) into it.
//
//   SpartanEye.exe              start the game via Steam if needed, then load the overlay
//   SpartanEye.exe --inject     only load the overlay into a running game (never starts Steam)
//   SpartanEye.exe --unload     ask a loaded overlay to unload itself (same as END in game)
//   SpartanEye.exe --shortcut   create a desktop shortcut to this exe
//
// Classic LoadLibraryW via CreateRemoteThread. Reads the game's PE header to refuse other builds. Writes nothing to
// disk except the optional shortcut; the game folder is never touched.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "../src/version.h"

namespace
{
const wchar_t kGameExe[] = L"MetroExodus.exe";
const wchar_t kDllName[] = L"metro_npc_overlay.dll";
const wchar_t kLogName[] = L"metro_npc_overlay.log";
const wchar_t kSteamUrl[] = L"steam://rungameid/1449560";  // Metro Exodus Enhanced Edition
const DWORD kBuildTimestamp = GAME_BUILD_TIMESTAMP;          // version.h
const DWORD kStartTimeoutMs = 180000;                       // Steam start-up + SteamStub + shader warm-up
const DWORD kReadyTimeoutMs = 300000;
const DWORD kSettleMs = 4000;                               // after the window and D3D12 appear

bool g_own_console = false;  // started by double-click: keep the window open on errors

void say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

int finish(int code)
{
    if (g_own_console)
    {
        if (code != 0)
        {
            say("\nPress Enter to close.");
            (void)getchar();
        }
        else
            Sleep(4000);
    }
    return code;
}

int fail(const char* fmt, ...)
{
    printf("\nERROR: ");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    return finish(1);
}

DWORD find_game(int& count)
{
    count = 0;
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, kGameExe) == 0)
        {
            pid = e.th32ProcessID;
            ++count;
        }
    CloseHandle(snap);
    return pid;
}

bool process_alive(DWORD pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h)
        return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// Module base of `name` in `pid`, or nullptr. A process that is still starting can make the snapshot fail
// (ERROR_BAD_LENGTH / ERROR_PARTIAL_COPY): the caller simply retries later.
BYTE* find_module(DWORD pid, const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return nullptr;
    MODULEENTRY32W m{sizeof(m)};
    BYTE* base = nullptr;
    for (BOOL ok = Module32FirstW(snap, &m); ok; ok = Module32NextW(snap, &m))
        if (_wcsicmp(m.szModule, name) == 0)
        {
            base = m.modBaseAddr;
            break;
        }
    CloseHandle(snap);
    return base;
}

struct WinSearch
{
    DWORD pid;
    bool found;
};

BOOL CALLBACK find_window_cb(HWND w, LPARAM lp)
{
    auto* s = reinterpret_cast<WinSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    RECT r{};
    if (pid == s->pid && IsWindowVisible(w) && GetWindow(w, GW_OWNER) == nullptr && GetClientRect(w, &r) &&
        r.right - r.left >= 320 && r.bottom - r.top >= 200)
    {
        s->found = true;
        return FALSE;
    }
    return TRUE;
}

bool has_main_window(DWORD pid)
{
    WinSearch s{pid, false};
    EnumWindows(find_window_cb, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

std::wstring own_dir()
{
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L"";
    wchar_t* slash = wcsrchr(p, L'\\');
    if (!slash)
        return L"";
    slash[1] = 0;
    return p;
}

// Reads the game's PE timestamp from its memory. 0 = unreadable (still starting).
DWORD game_timestamp(HANDLE proc, DWORD pid)
{
    BYTE* exe = find_module(pid, kGameExe);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!exe || !ReadProcessMemory(proc, exe, &dos, sizeof(dos), nullptr) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadProcessMemory(proc, exe + dos.e_lfanew, &nt, sizeof(nt), nullptr) || nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return nt.FileHeader.TimeDateStamp;
}

std::string read_file(const std::wstring& path)
{
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got)
        out.append(buf, got);
    CloseHandle(f);
    return out;
}

// One line of the overlay's log that contains `key`, without the time / thread prefix.
std::string log_line(const std::string& log, const char* key)
{
    size_t at = log.find(key);
    if (at == std::string::npos)
        return "";
    size_t start = log.rfind('\n', at);
    start = start == std::string::npos ? 0 : start + 1;
    size_t end = log.find_first_of("\r\n", at);
    std::string line = log.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t body = line.find("] ");
    return body == std::string::npos ? line : line.substr(body + 2);
}

int do_shortcut()
{
    std::wstring dir = own_dir();
    if (dir.empty())
        return fail("cannot determine my own folder");
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return fail("cannot determine my own path");
    std::wstring exe = self;
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        return fail("cannot find the desktop folder");
    std::wstring lnk = std::wstring(desktop) + L"\\" + kModTitleW + L".lnk";
    CoTaskMemFree(desktop);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return fail("COM initialisation failed");
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr))
    {
        link->SetPath(exe.c_str());
        link->SetWorkingDirectory(dir.c_str());
        link->SetDescription(L"Start Metro Exodus Enhanced Edition with the NPC overlay");
        link->SetIconLocation(exe.c_str(), 0);
        IPersistFile* file = nullptr;
        hr = link->QueryInterface(IID_PPV_ARGS(&file));
        if (SUCCEEDED(hr))
        {
            hr = file->Save(lnk.c_str(), TRUE);
            file->Release();
        }
        link->Release();
    }
    CoUninitialize();
    if (FAILED(hr))
        return fail("could not create the shortcut (0x%08lX)", static_cast<unsigned long>(hr));
    say("Desktop shortcut created: %ls", lnk.c_str());
    say("It starts the game through Steam and loads the overlay. Delete it any time.");
    return finish(0);
}

int do_unload()
{
    int count = 0;
    DWORD pid = find_game(count);
    if (count == 0)
        return fail("Metro Exodus is not running.");
    wchar_t ev_name[64];
    swprintf_s(ev_name, L"Local\\MetroNpcOverlay_Unload_%lu", pid);
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, ev_name);
    if (!ev)
        return fail("the overlay is not loaded in the game.");
    SetEvent(ev);
    CloseHandle(ev);
    say("Unload requested. The overlay removes itself within a second.");
    return finish(0);
}

// Waits for the game process: started by us through Steam, or already running.
DWORD wait_for_game(bool start_steam)
{
    int count = 0;
    DWORD pid = find_game(count);
    if (count > 1)
    {
        fail("more than one MetroExodus.exe is running; close the extra one.");
        return 0;
    }
    if (pid)
        return pid;
    if (!start_steam)
    {
        fail("Metro Exodus is not running. Start it (or run me without --inject to start it through Steam).");
        return 0;
    }
    say("Starting Metro Exodus Enhanced Edition through Steam...");
    HINSTANCE r = ShellExecuteW(nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32)
    {
        fail("could not open %ls - is Steam installed?", kSteamUrl);
        return 0;
    }
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < kStartTimeoutMs)
    {
        Sleep(1000);
        pid = find_game(count);
        if (pid && count == 1)
            return pid;
    }
    fail("the game did not start within %lu s. Is Metro Exodus Enhanced Edition installed in Steam (and Steam "
         "logged in)? Start it by hand and run me again.",
         kStartTimeoutMs / 1000);
    return 0;
}

// The overlay hooks DXGI / D3D12: wait until the game has a window and has loaded both, then a little longer.
bool wait_until_ready(DWORD pid)
{
    say("Waiting for the game window...");
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG ready_at = 0;
    while (GetTickCount64() - t0 < kReadyTimeoutMs)
    {
        if (!process_alive(pid))
        {
            fail("the game closed before the overlay could be loaded.");
            return false;
        }
        bool ready = has_main_window(pid) && find_module(pid, L"d3d12.dll") && find_module(pid, L"dxgi.dll");
        if (ready && !ready_at)
            ready_at = GetTickCount64();
        if (!ready)
            ready_at = 0;
        if (ready_at && GetTickCount64() - ready_at >= kSettleMs)
            return true;
        Sleep(500);
    }
    fail("the game window did not appear within %lu s.", kReadyTimeoutMs / 1000);
    return false;
}

int do_launch(bool start_steam)
{
    say("%ls %s - single-player only, reads game memory, writes none.", kModTitleW, kVersionString);
    std::wstring dir = own_dir();
    if (dir.empty())
        return fail("cannot determine my own folder");
    std::wstring dll = dir + kDllName;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail("metro_npc_overlay.dll is missing next to this exe. Extract the whole zip into one folder "
                    "(your antivirus may have removed the DLL - see the README).");
    // The overlay keeps its settings and log next to the DLL (e.g. not possible under Program Files).
    HANDLE probe = CreateFileW((dir + L"write_test.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (probe == INVALID_HANDLE_VALUE)
        say("WARNING: this folder is not writable: settings will not be saved and there will be no log.\n"
            "         Move the mod folder somewhere you can write to (e.g. Documents or Desktop).");
    else
        CloseHandle(probe);

    DWORD pid = wait_for_game(start_steam);
    if (!pid)
        return 1;  // fail() already reported and waited
    if (!wait_until_ready(pid))
        return 1;
    if (find_module(pid, kDllName))
    {
        say("The overlay is already loaded. In game: INSERT = menu, END = unload.");
        return finish(0);
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                              FALSE, pid);
    if (!proc)
        return fail("cannot open the game process (error %lu). If the game runs as administrator, run me as "
                    "administrator too.",
                    GetLastError());

    // Build guard: the overlay knows exactly one build. Anything else (another Steam patch, Epic / GOG, the original
    // 2019 edition) is refused here, before anything is loaded.
    DWORD ts = game_timestamp(proc, pid);
    if (ts != kBuildTimestamp)
    {
        CloseHandle(proc);
        if (!ts)
            return fail("cannot read the game's executable header. Try again at the main menu.");
        return fail("unsupported game version (exe timestamp %lu, this release supports %lu = Steam Enhanced "
                    "Edition build " GAME_BUILD_ID ").\n       Nothing was loaded. Check for a newer release of this mod "
                    "(see the README, \"After a game update\").",
                    ts, kBuildTimestamp);
    }

    const std::wstring log_path = dir + kLogName;
    const std::string prev_log = read_file(log_path);  // the DLL replaces it; until then it is the last session's
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr))
    {
        if (remote)
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return fail("could not write the DLL path into the game (error %lu).", GetLastError());
    }
    // kernel32 is mapped at the same address in every process of this session.
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, load, remote, 0, nullptr);
    DWORD code = 0;
    if (th)
    {
        WaitForSingleObject(th, 15000);
        GetExitCodeThread(th, &code);
        CloseHandle(th);
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    if (!th || !code)
        return fail("Windows refused to load the overlay DLL into the game. An antivirus may be blocking it "
                    "(see the README).");

    // The DLL reports through its log: wait for its verdict.
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 10000)
    {
        Sleep(250);
        std::string log = read_file(log_path);
        char loaded[64];
        snprintf(loaded, sizeof(loaded), "pid %lu\r\n", pid);
        if (log == prev_log || log.find("metro_npc_overlay loaded") == std::string::npos ||
            log.find(loaded) == std::string::npos)
            continue;  // not this session's log yet
        if (log.find("hooks enabled") != std::string::npos)
        {
            std::string partial = log_line(log, "game data unavailable");
            if (!partial.empty())
                return fail("the overlay loaded but cannot read the game data:\n       %s\n       Log: %ls",
                            partial.c_str(), log_path.c_str());
            say("Overlay loaded. In game: INSERT = menu, HOME = NPC report, END = unload.");
            return finish(0);
        }
        for (const char* bad : {"refusing to run", "render hooks failed", "MH_Initialize failed",
                                "MH_ApplyQueued failed", "CreateEvent failed", "another copy of the overlay"})
        {
            std::string line = log_line(log, bad);
            if (!line.empty())
                return fail("the overlay did not start:\n       %s\n       Log: %ls", line.c_str(), log_path.c_str());
        }
    }
    say("The overlay was loaded, but its log gave no answer within 10 s.");
    say("If no \"loaded\" message appears in game, see the log: %ls", log_path.c_str());
    return finish(0);
}
}  // namespace

int wmain(int argc, wchar_t** argv)
{
    DWORD procs[2];
    g_own_console = GetConsoleProcessList(procs, 2) <= 1;
    SetConsoleTitleW(kModTitleW);
    const wchar_t* arg = argc > 1 ? argv[1] : L"";
    if (argc > 2 || (argc == 2 && wcscmp(arg, L"--inject") != 0 && wcscmp(arg, L"--unload") != 0 &&
                     wcscmp(arg, L"--shortcut") != 0))
    {
        say("usage: SpartanEye.exe [--inject | --unload | --shortcut]");
        return finish(2);
    }
    if (wcscmp(arg, L"--unload") == 0)
        return do_unload();
    if (wcscmp(arg, L"--shortcut") == 0)
        return do_shortcut();
    return do_launch(wcscmp(arg, L"--inject") != 0);
}
