#include "config.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "input.h"
#include "log.h"
#include "state.h"
#include "version.h"

namespace
{
enum class FT
{
    Bool,
    Float,
    Color,  // RGBA, stored as #RRGGBBAA
    Key,    // virtual-key code, stored by name
    Int     // integer in [lo, hi] (enumerations)
};

struct Field
{
    const char* key;
    FT type;
    size_t offset;
    float lo, hi;  // Float range
};

#define F_BOOL(m) {#m, FT::Bool, offsetof(Settings, m), 0, 0}
#define F_FLOAT(m, lo, hi) {#m, FT::Float, offsetof(Settings, m), lo, hi}
#define F_COLOR(m) {#m, FT::Color, offsetof(Settings, m), 0, 0}
#define F_KEY(m) {#m, FT::Key, offsetof(Settings, m), 0, 0}
#define F_INT(m, lo, hi) {#m, FT::Int, offsetof(Settings, m), lo, hi}

const Field kFields[] = {
    F_INT(version, 1, 1000),
    F_BOOL(enabled), F_FLOAT(max_dist, 10.f, 500.f), F_BOOL(show_dead),
    F_BOOL(show_human), F_BOOL(show_mutant), F_BOOL(show_fauna),
    F_BOOL(show_friendly), F_BOOL(show_neutral), F_BOOL(show_hostile),
    F_BOOL(show_state), F_BOOL(show_health), F_BOOL(show_arrows), F_INT(arrows_mode, 0, 2),
    F_FLOAT(arrow_max_dist, 5.f, 500.f), F_BOOL(debug_labels),
    F_BOOL(smart_detail), F_INT(calm_detail, 0, 2), F_FLOAT(detail_dist, 5.f, 500.f), F_BOOL(focus_crosshair),
    F_BOOL(dist_fade), F_FLOAT(fade_min_alpha, 0.1f, 1.f), F_BOOL(state_icons), F_INT(health_when, 0, 2),
    F_INT(box_style, 0, 2), F_FLOAT(box_thickness, 0.5f, 6.f), F_INT(hb_pos, 0, 3), F_INT(hb_style, 0, 2),
    F_BOOL(lbl_name), F_BOOL(lbl_distance), F_BOOL(lbl_class), F_BOOL(lbl_health), F_FLOAT(label_scale, 0.6f, 2.f),
    F_BOOL(label_declutter),
    F_BOOL(show_awareness), F_BOOL(alerts), F_FLOAT(alert_threshold, 1.f, 100.f), F_BOOL(alert_on_target),
    F_BOOL(alert_hostile_only), F_FLOAT(alert_max_dist, 5.f, 500.f), F_INT(alert_pos, 0, 2), F_BOOL(alert_sound),
    F_BOOL(show_cones), F_INT(cones_mode, 0, 2), F_FLOAT(cone_max_dist, 5.f, 200.f), F_FLOAT(cone_length, 1.f, 50.f), F_FLOAT(cone_fov, 10.f, 360.f),
    F_BOOL(cone_real), F_BOOL(cone_back), F_BOOL(cone_periph),
    F_BOOL(det_list), F_BOOL(det_list_hide_empty), F_FLOAT(det_list_dist, 5.f, 200.f), F_INT(det_list_rows, 1, 20), F_INT(det_list_pos, 0, 2),
    F_BOOL(det_factors),
    F_BOOL(show_routes), F_INT(routes_mode, 0, 1), F_FLOAT(route_max_dist, 5.f, 200.f), F_BOOL(route_labels),
    F_BOOL(los_enabled), F_INT(los_style, 0, 2), F_FLOAT(los_alpha, 0.05f, 1.f), F_INT(los_unknown, 0, 1),
    F_BOOL(show_zones), F_BOOL(zone_triggers_only), F_FLOAT(zone_max_dist, 5.f, 300.f), F_BOOL(zone_labels),
    F_BOOL(zone_alerts), F_BOOL(show_when_paused),
    F_COLOR(col_friendly), F_COLOR(col_neutral), F_COLOR(col_hostile), F_COLOR(col_dead),
    F_KEY(key_menu), F_KEY(key_report), F_KEY(key_unload),
};

const KeyName kKeys[] = {
    {VK_INSERT, "INSERT"}, {VK_HOME, "HOME"}, {VK_END, "END"}, {VK_DELETE, "DELETE"}, {VK_PRIOR, "PAGE_UP"},
    {VK_NEXT, "PAGE_DOWN"}, {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F3, "F3"}, {VK_F4, "F4"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_PAUSE, "PAUSE"}, {VK_SCROLL, "SCROLL_LOCK"},
    {VK_NUMPAD0, "NUMPAD_0"}, {VK_NUMPAD1, "NUMPAD_1"}, {VK_NUMPAD2, "NUMPAD_2"}, {VK_NUMPAD3, "NUMPAD_3"},
    {VK_NUMPAD4, "NUMPAD_4"}, {VK_NUMPAD5, "NUMPAD_5"}, {VK_NUMPAD6, "NUMPAD_6"}, {VK_NUMPAD7, "NUMPAD_7"},
    {VK_NUMPAD8, "NUMPAD_8"}, {VK_NUMPAD9, "NUMPAD_9"},
};

Settings g_saved;               // what is on disk (or defaults if nothing was saved yet)
ULONGLONG g_dirty_since = 0;    // first frame the settings differed from g_saved

std::wstring ini_path() { return std::wstring(log_dir()) + L"metro_npc_overlay.ini"; }

void apply_keys() { input_set_keys(g_settings.key_menu, g_settings.key_report, g_settings.key_unload); }

bool key_from_name(const char* s, int& vk)
{
    for (const auto& k : kKeys)
        if (_stricmp(k.name, s) == 0)
        {
            vk = k.vk;
            return true;
        }
    return false;
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool same(const Settings& a, const Settings& b)
{
    for (const Field& f : kFields)
    {
        const char* pa = reinterpret_cast<const char*>(&a) + f.offset;
        const char* pb = reinterpret_cast<const char*>(&b) + f.offset;
        size_t n = f.type == FT::Bool ? sizeof(bool) : 4;
        if (memcmp(pa, pb, n) != 0)
            return false;
    }
    return true;
}
}  // namespace

const KeyName* config_key_list(int& count)
{
    count = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));
    return kKeys;
}

const char* config_key_name(int vk)
{
    for (const auto& k : kKeys)
        if (k.vk == vk)
            return k.name;
    return "?";
}

void config_load()
{
    g_settings = Settings{};
    HANDLE f = CreateFileW(ini_path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        logf("config: no metro_npc_overlay.ini yet, using defaults");
        g_saved = g_settings;
        apply_keys();
        return;
    }
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0 && text.size() < (1 << 20))
        text.append(buf, got);
    CloseHandle(f);

    g_settings.version = 1;  // files written before presets have no version line
    int applied = 0, ignored = 0;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            ++ignored;
            continue;
        }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        const Field* fld = nullptr;
        for (const Field& x : kFields)
            if (k == x.key)
                fld = &x;
        if (!fld)
        {
            ++ignored;
            continue;
        }
        char* base = reinterpret_cast<char*>(&g_settings) + fld->offset;
        bool ok = true;
        switch (fld->type)
        {
        case FT::Bool:
            if (v == "1" || _stricmp(v.c_str(), "true") == 0)
                *reinterpret_cast<bool*>(base) = true;
            else if (v == "0" || _stricmp(v.c_str(), "false") == 0)
                *reinterpret_cast<bool*>(base) = false;
            else
                ok = false;
            break;
        case FT::Float:
        {
            char* end = nullptr;
            float x = strtof(v.c_str(), &end);
            if (end == v.c_str() || !std::isfinite(x))
                ok = false;
            else
                *reinterpret_cast<float*>(base) = x < fld->lo ? fld->lo : (x > fld->hi ? fld->hi : x);
            break;
        }
        case FT::Color:
        {
            unsigned r, g, b, a = 255;
            if (v.size() == 9 && v[0] == '#' && sscanf_s(v.c_str() + 1, "%2x%2x%2x%2x", &r, &g, &b, &a) == 4)
                *reinterpret_cast<uint32_t*>(base) = (a << 24) | (b << 16) | (g << 8) | r;  // IM_COL32 layout
            else
                ok = false;
            break;
        }
        case FT::Key:
        {
            int vk = 0;
            if (key_from_name(v.c_str(), vk))
                *reinterpret_cast<int*>(base) = vk;
            else
                ok = false;
            break;
        }
        case FT::Int:
        {
            char* end = nullptr;
            long x = strtol(v.c_str(), &end, 10);
            if (end == v.c_str())
                ok = false;
            else
                *reinterpret_cast<int*>(base) = static_cast<int>(x < fld->lo ? fld->lo : (x > fld->hi ? fld->hi : x));
            break;
        }
        }
        ok ? ++applied : ++ignored;
    }
    // Three different keys are required; fall back to the defaults on a clash.
    if (g_settings.key_menu == g_settings.key_report || g_settings.key_menu == g_settings.key_unload ||
        g_settings.key_report == g_settings.key_unload)
    {
        Settings d;
        g_settings.key_menu = d.key_menu;
        g_settings.key_report = d.key_report;
        g_settings.key_unload = d.key_unload;
        logf("config: duplicate key bindings, keys reset to defaults");
    }
    if (g_settings.version < 3)
    {
        const int from = g_settings.version;
        // 1 -> 2: neutral used to be yellow, which detection now uses: move an unchanged old default to the new one.
        if (from < 2 && g_settings.col_neutral == 0xFF3CD2F0)
            g_settings.col_neutral = Settings{}.col_neutral;
        // 2 -> 3: Stealth draws cones for threats and the crosshair NPC only (was: every hostile within range).
        if (from == 2 && g_settings.cones_mode == 1)
            g_settings.cones_mode = 0;
        logf("config: settings file version %d -> 3 (new options at their defaults)", from);
        g_settings.version = 3;
    }
    g_saved = g_settings;
    apply_keys();
    logf("config: loaded metro_npc_overlay.ini (%d values, %d ignored)", applied, ignored);
}

void config_save()
{
    std::string out = "# " MOD_TITLE " settings. Written by the overlay; edit while the game is closed.\r\n"
                      "# Keys: INSERT HOME END DELETE PAGE_UP PAGE_DOWN F1-F4 F6-F11 PAUSE SCROLL_LOCK NUMPAD_0-9\r\n";
    char line[160];
    for (const Field& f : kFields)
    {
        const char* base = reinterpret_cast<const char*>(&g_settings) + f.offset;
        switch (f.type)
        {
        case FT::Bool: snprintf(line, sizeof(line), "%s = %d\r\n", f.key, *reinterpret_cast<const bool*>(base) ? 1 : 0); break;
        case FT::Float: snprintf(line, sizeof(line), "%s = %.6g\r\n", f.key, *reinterpret_cast<const float*>(base)); break;
        case FT::Color:
        {
            uint32_t c = *reinterpret_cast<const uint32_t*>(base);
            snprintf(line, sizeof(line), "%s = #%02X%02X%02X%02X\r\n", f.key, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
                     c >> 24);
            break;
        }
        case FT::Key: snprintf(line, sizeof(line), "%s = %s\r\n", f.key, config_key_name(*reinterpret_cast<const int*>(base))); break;
        case FT::Int: snprintf(line, sizeof(line), "%s = %d\r\n", f.key, *reinterpret_cast<const int*>(base)); break;
        }
        out += line;
    }
    std::wstring path = ini_path(), tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        logf("config: cannot write %ls (error %lu)", tmp.c_str(), GetLastError());
        return;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(h);
    // Write-then-rename: a crash mid-write never leaves a half-written ini.
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        logf("config: saving failed (error %lu)", GetLastError());
        DeleteFileW(tmp.c_str());
        return;
    }
    g_saved = g_settings;
    g_dirty_since = 0;
}

void config_autosave()
{
    if (same(g_settings, g_saved))
    {
        g_dirty_since = 0;
        return;
    }
    if (g_settings.key_menu != g_saved.key_menu || g_settings.key_report != g_saved.key_report ||
        g_settings.key_unload != g_saved.key_unload)
        apply_keys();
    ULONGLONG now = GetTickCount64();
    if (!g_dirty_since)
        g_dirty_since = now;
    else if (now - g_dirty_since > 1000)
        config_save();
}

void config_flush()
{
    if (!same(g_settings, g_saved))
        config_save();
}

void config_reset()
{
    g_settings = Settings{};
    apply_keys();
    config_save();
    logf("config: reset to defaults");
}

// Presets set what is drawn and how much detail; filters, distances, colours, keys, sizes, alert / line-of-sight
// options and the Debug tab are kept. Every preset sets the same fields, so config_preset can match them exactly.
void config_apply_preset(Settings& s, int p)
{
    const bool min = p == 1, full = p == 3;
    if (p < 1 || p > 3)
        return;
    s.smart_detail = !full;
    s.calm_detail = min ? 0 : full ? 2 : 1;
    s.detail_dist = min ? 25.f : 35.f;
    s.dist_fade = !full;
    s.state_icons = true;
    s.box_style = min ? 1 : 0;
    s.show_health = true;
    s.health_when = min ? 2 : full ? 0 : 1;
    s.lbl_name = s.lbl_distance = true;
    s.lbl_class = s.lbl_health = s.debug_labels = full;
    s.show_state = s.show_awareness = true;
    s.show_arrows = true;
    s.arrows_mode = min ? 0 : full ? 2 : 1;
    s.show_cones = !min;
    s.cones_mode = full ? 2 : 0;
    s.cone_real = s.cone_back = true;
    s.cone_periph = full;
    s.det_list = true;
    s.det_list_hide_empty = !full;
    s.det_factors = full;
    s.show_routes = !min;
    s.routes_mode = full ? 1 : 0;
    s.route_labels = true;
}

int config_preset(const Settings& s)
{
    for (int p = 1; p <= 3; ++p)
    {
        Settings t = s;
        config_apply_preset(t, p);
        if (same(t, s))
            return p;
    }
    return 0;
}
