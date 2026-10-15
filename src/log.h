#pragma once
#include <windows.h>

// File log next to the DLL (metro_npc_overlay.log), truncated per injection. Thread-safe, flushed per line.
void log_open(HMODULE self);
void logf(const char* fmt, ...);
void log_close();
const wchar_t* log_dir();  // folder of the DLL with a trailing backslash ("" if unknown)
