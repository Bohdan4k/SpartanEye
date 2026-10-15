#include "log.h"

#include <cstdarg>
#include <cstdio>

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static bool g_lock_ready = false;
static wchar_t g_dir[MAX_PATH] = L"";

const wchar_t* log_dir() { return g_dir; }

void log_open(HMODULE self)
{
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    slash[1] = 0;
    wcscpy_s(g_dir, path);
    wchar_t old[MAX_PATH];
    wcscpy_s(old, path);
    wcscpy_s(old + (slash + 1 - path), MAX_PATH - (slash + 1 - path), L"metro_npc_overlay.old.log");
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"metro_npc_overlay.log");
    MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);  // keep the previous session's log (re-inject diagnosis)
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void logf(const char* fmt, ...)
{
    if (!g_lock_ready || g_file == INVALID_HANDLE_VALUE)
        return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int len = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond,
                       t.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + len, sizeof(line) - len - 2, fmt, ap);
    va_end(ap);
    len = (m < 0) ? len : (len + m < (int)sizeof(line) - 2 ? len + m : (int)sizeof(line) - 3);
    line[len++] = '\r';
    line[len++] = '\n';
    EnterCriticalSection(&g_lock);
    DWORD written;
    WriteFile(g_file, line, (DWORD)len, &written, nullptr);
    LeaveCriticalSection(&g_lock);
}

void log_close()
{
    if (g_file != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    if (g_lock_ready)
    {
        g_lock_ready = false;
        DeleteCriticalSection(&g_lock);
    }
}
