#pragma once
#include <string>
#include <vector>

#include "game.h"

// Frame capture for offline UI previews (tools/preview): the overlay writes the FrameData it is about to draw as text,
// then skips drawing for a few frames so a screenshot of the clean game image can be taken. Read-only for the game.
// Triggered by the named event Local\MetroNpcOverlay_Capture_<pid> (tools/capture.py), never by game input.

// Writes `fd` (+ the viewport size) to `path`. Returns false on I/O errors.
bool capture_write(const FrameData& fd, float w, float h, const wchar_t* path);

// Reads a file written by capture_write. Strings (names, classes, display names) are kept alive in `pool`.
bool capture_read(const char* path, FrameData& fd, float& w, float& h, std::vector<std::string>& pool);
