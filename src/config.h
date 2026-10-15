#pragma once

// Settings persistence: metro_npc_overlay.ini next to the DLL (plain "key = value" lines, written by us; ImGui's own
// file functions stay compiled out). Every field of Settings is listed once in config.cpp; unknown or malformed lines
// are ignored and values are clamped, so a hand-edited or old file can never break the overlay.

void config_load();           // at startup, before the hooks are enabled
void config_save();           // write now
void config_autosave();       // render thread, once per frame: saves ~1 s after the settings last changed
void config_reset();          // back to defaults (and save)
void config_flush();          // save if anything changed (on unload)

// Display presets: 1 Minimal, 2 Stealth (= the defaults), 3 Full / Debug.
struct Settings;
void config_apply_preset(Settings& s, int preset);
int config_preset(const Settings& s);  // the preset `s` matches, 0 = custom

// Keys that may be bound to overlay actions (name for the menu + virtual-key code).
struct KeyName
{
    int vk;
    const char* name;
};
const KeyName* config_key_list(int& count);
const char* config_key_name(int vk);
