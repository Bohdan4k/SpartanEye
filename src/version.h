#pragma once
// One place for the release name and version (code + Windows version resources, overlay/res/*.rc).
#define MOD_VERSION_MAJOR 1
#define MOD_VERSION_MINOR 0
#define MOD_VERSION_PATCH 0
#define MOD_VERSION_STR "1.0.0"
#define MOD_TITLE "Spartan Eye"
#define MOD_AUTHOR "root"
// The one supported game build (Steam, Enhanced Edition): MetroExodus.exe PE timestamp. Test builds override it
// (CMake -DGAME_BUILD_TIMESTAMP_OVERRIDE=...) to exercise the "unsupported version" path on the real game.
#ifndef GAME_BUILD_TIMESTAMP
#define GAME_BUILD_TIMESTAMP 1787840176
#endif
#define GAME_BUILD_ID "24973569"

#ifndef RC_INVOKED
inline constexpr const char* kVersionString = MOD_VERSION_STR;
inline constexpr const wchar_t* kModTitleW = L"" MOD_TITLE;
#endif
