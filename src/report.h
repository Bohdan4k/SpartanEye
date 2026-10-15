#pragma once
#include "game.h"

// Appends a snapshot of the in-world creatures within `radius` metres (all of them, ignoring the display filters) to
// metro_npc_reports.log next to the DLL. Returns the number of NPCs written, or -1 if nothing could be written.
int report_write(const FrameData& fd, float radius);
