#pragma once
#include "game.h"

// Draws NPC markers (background draw list, always through walls) and, when open, the INSERT menu.
void ui_setup_fonts();  // once, right after ImGui::CreateContext
void ui_draw(const FrameData& fd, float w, float h, bool menu_open);
void ui_toast(const char* msg, float seconds);  // short banner at the top of the screen
// Menu tab to show on the next frame (0 = first) and whether to expand every section; used by the offline preview.
void ui_menu_request(int tab, bool open_sections);
bool ui_toast_active();
