#pragma once
#include <atomic>
#include <cstdint>

// User settings. Read and written only on the render (Present) thread. The defaults are the Stealth preset
// (config_apply_preset).
struct Settings
{
    int version = 3;         // settings file layout (1: before presets, 2: Stealth cones for hostiles; migrated)
    bool enabled = true;     // draw NPC markers
    bool show_when_paused = false;  // keep drawing while the game is paused (Esc menu, photo mode, ...)
    float max_dist = 100.f;  // metres from the player
    bool show_dead = false;  // NPCs with health <= 0
    // filters: category x relation
    bool show_human = true, show_mutant = true, show_fauna = true;
    bool show_friendly = true, show_neutral = true, show_hostile = true;
    bool show_state = true;     // AI mental state (calm / alert / combat) + "targeting you"
    bool show_health = true;    // health bar
    bool show_arrows = true;    // edge arrows for NPCs off screen / behind you
    int arrows_mode = 1;        // 0 threats only, 1 + hostile NPCs within arrow_max_dist, 2 every NPC in range
    float arrow_max_dist = 40.f;
    bool debug_labels = false;  // raw name, class, id, friend_type, health, mental value
    // level of detail: full info only where it matters (threats, the NPC under the crosshair)
    bool smart_detail = true;     // off: every NPC gets full detail
    int calm_detail = 1;          // calm NPCs within detail_dist: 0 marker only, 1 box + name, 2 full
    float detail_dist = 35.f;     // calm NPCs further away get the small marker only
    bool focus_crosshair = true;  // the NPC under the crosshair gets full detail
    bool dist_fade = true;        // calm NPCs fade and shrink with distance
    float fade_min_alpha = 0.4f;  // opacity at the max distance
    bool state_icons = true;      // "?" (alerted, noticing you) / "!" (spotted you, combat) above the marker
    int health_when = 1;          // health bar: 0 always, 1 when damaged or in focus, 2 in focus only
    // marker style (group 2)
    int box_style = 0;            // 0 full box, 1 corners only, 2 none
    float box_thickness = 1.6f;   // pixels at 1080p (scaled with the resolution)
    int hb_pos = 0;               // health bar: 0 top, 1 bottom, 2 left, 3 right
    int hb_style = 0;             // 0 bar, 1 bar + percent, 2 segmented
    bool lbl_name = true;         // label lines under the box
    bool lbl_distance = true;
    bool lbl_class = false;       // engine class (NOSALIS, ...) when it differs from the name
    bool lbl_health = false;      // health percent as text
    float label_scale = 1.0f;     // label font size factor
    bool label_declutter = true;  // move overlapping labels apart
    // stealth (group 3)
    bool show_awareness = true;     // detection bar + percent (exact when the NPC has a vision object)
    bool alerts = true;             // banner when an NPC becomes aware of you / targets you
    float alert_threshold = 60.f;   // awareness percent that triggers an alert
    bool alert_on_target = true;    // alert when an NPC starts targeting you
    bool alert_hostile_only = true; // only hostile NPCs raise alerts
    float alert_max_dist = 60.f;    // metres
    int alert_pos = 0;              // banner: 0 top, 1 middle, 2 bottom
    bool alert_sound = false;       // system "exclamation" sound
    bool show_cones = true;         // view cones on the ground
    int cones_mode = 0;             // 0 threats + crosshair only, 1 + hostile NPCs, 2 every NPC with an AI
    float cone_max_dist = 25.f;     // only NPCs closer than this get a cone
    float cone_length = 6.f;        // metres
    float cone_fov = 90.f;          // degrees, for NPCs without their own vision FOV
    bool cone_real = true;          // cones from the NPC's own vision (range, FOV, edge range); else the sliders
    bool cone_back = true;          // all-round zone (back range) and the instant zone (force range)
    bool cone_periph = false;       // peripheral zone (the engine uses the head; drawn from the body facing)
    // detection (exact: the engine's own vision value)
    bool det_list = true;           // "who is detecting me" panel
    bool det_list_hide_empty = true; // only while somebody is detecting you
    float det_list_dist = 60.f;     // metres
    int det_list_rows = 6;
    int det_list_pos = 0;           // 0 top right, 1 middle right, 2 bottom right
    bool det_factors = false;       // "why is my % rising" panel (player factors + the top NPC's formula terms)
    // routes and intentions (step 3)
    bool show_routes = true;        // where the NPC is going: path, stop point, facing there, what it will do
    int routes_mode = 0;            // 0 the NPC under the crosshair + threats, 1 + every NPC nearby with a route
    float route_max_dist = 40.f;    // metres; routes are read only within this distance
    bool route_labels = true;       // intention label + estimated time of arrival at the stop point
    // line of sight (group 4)
    bool los_enabled = true;        // style NPCs behind walls differently
    int los_style = 0;              // 0 faded, 1 dashed, 2 faded + dashed
    float los_alpha = 0.35f;        // opacity of hidden NPCs (faded styles)
    int los_unknown = 0;            // NPCs without line-of-sight data: 0 draw as visible, 1 draw as hidden
    // zones (group 5, Debug tab)
    bool show_zones = false;        // trigger / script zones as boxes
    bool zone_triggers_only = true; // only zones named trigger*
    float zone_max_dist = 30.f;     // metres (zone origin)
    bool zone_labels = true;
    bool zone_alerts = false;       // banner when the player enters a shown zone
    // colours, IM_COL32 layout (0xAABBGGRR). Relation only: yellow / orange / red belong to detection.
    uint32_t col_friendly = 0xFF5ADC46;  // (70, 220, 90)
    uint32_t col_neutral = 0xFFFFB464;   // (100, 180, 255); version 1 default was (240, 210, 60)
    uint32_t col_hostile = 0xFF3246FF;   // (255, 70, 50)
    uint32_t col_dead = 0xDC969696;      // (150, 150, 150, 220)
    // keys (virtual-key codes)
    int key_menu = 0x2D;    // VK_INSERT
    int key_report = 0x24;  // VK_HOME
    int key_unload = 0x23;  // VK_END
};
extern Settings g_settings;

extern std::atomic<bool> g_menu_open;         // read by the DirectInput hooks to block game input
extern std::atomic<bool> g_unload_requested;  // END pressed
extern std::atomic<int> g_active_calls;       // threads currently inside one of our detours

void request_unload();

// Counts a thread as "inside our code" for the lifetime of a detour, so unload can wait for it to leave.
struct ActiveCall
{
    ActiveCall() { g_active_calls.fetch_add(1, std::memory_order_acq_rel); }
    ~ActiveCall() { g_active_calls.fetch_sub(1, std::memory_order_acq_rel); }
    ActiveCall(const ActiveCall&) = delete;
    ActiveCall& operator=(const ActiveCall&) = delete;
};
