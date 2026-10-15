#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Game data (EE build 24973569), resolved by signature. See re/STRUCTS.md §1-3.

// Relation to the player, from the engine's own rule on friend_type (entity_is_enemy_base, fn 0x14084EC60).
enum class Relation : uint8_t
{
    Friendly,
    Neutral,
    Hostile
};
enum class Category : uint8_t
{
    Human,
    Mutant,
    Fauna
};
const char* friend_type_name(uint8_t t);  // efriend_type names from the game's script data (none, enemy, friend, ...)

// AI mental state (brain +0xA51); names from the brain's own debug dump (fn 0x1409423C0).
enum class Mood : uint8_t
{
    Unknown,  // no AI brain
    Calm,     // free
    Alert,    // disturbed, light alert, alert
    Combat    // uber alert, danger
};
const char* mental_state_name(uint8_t m);  // free, disturbed, light alert, alert, uber alert, danger

// The NPC's own vision (re/STRUCTS.md §6): the engine's live values, read every frame. Humans and mutants alike.
struct Vision
{
    bool ok = false;          // vision object found and validated
    bool has_entry = false;   // the NPC holds a value for the player (no entry = never gained sight = 0)
    float value = 0;          // its accumulated value for the player
    // live thresholds (the brain lowers them when alerted and swaps in a combat set, e.g. seen 0.55)
    float noticed = 0;        // value >= noticed: "disturbed" / goes to look (vision +0x68)
    float light = 0;          // value >= light: light alert (+0x70)
    float seen = 0;           // value >= seen: spotted, combat (+0x74) = 100 %
    float range = 0;          // metres at the centre of the view (x k_centre); x k_edge at the FOV edge
    float back_range = 0;     // all-round sight within this distance (rate as at the FOV edge)
    float fov = 0;            // radians, full angle, around the body forward
    float k_edge = 0, k_centre = 0, min_dist = 0;
    float lum_offset = 0, gain = 0, vel_k = 0, min_lum = 0, far_sub = 0, near_dist = 0;
    float force_range = 0;    // in view and closer than this: spotted instantly
    float peri_dist = 0, peri_angle = 0;  // peripheral zone around the head direction (radians, full angle)
    uint32_t period = 0;      // ms; gain per update is scaled by dt / period
    bool surv = false;        // "surv" threshold set in use
    float rate = 0;           // observed change of the value, per second (smoothed)
};

// What makes the player visible (fn 0x14081B8A0): V = speed_term x env x max(light, flash) x stance.
struct PlayerVisibility
{
    bool ok = false;
    float light = 0;          // light on the player (+0x2370 x +0x27DC)
    float flash = 0;          // weapon / torch flash floor while active, else 0
    float stance = 1;         // +0x9C4: 1 on foot, 10 in a boat
    float speed = 0;          // m/s, from the position over game time
    float speed_min = 0, speed_k = 0, speed_base = 1;  // speed term = (speed > min ? speed x k : 0) + base
    bool crouched = false;    // movement state nibble == 1: the vegetation factor applies
    int env_mode = 0;         // 0 standing (env exactly 1), 1 crouched (vegetation computed), 2 crouched, not computable
    float cover = 0;          // vegetation density around the player
    float env = 1;            // k / (k + cover)
    float visibility = 0;     // V
};

// Predicted gain from the engine formula (fn 0x140F9B9B0) for the current geometry, for the "why" panel.
struct VisionPrediction
{
    bool in_cone = false, in_back = false, in_force = false;
    float angle = 0;          // radians between the NPC's body forward and the direction to the player
    float r_eff = 0;          // effective range at that angle
    float falloff = 0;        // ln(r_eff / d) / ln(r_eff / min_dist), 0 out of range
    float light_term = 0;     // max(V - min_lum, near floor) + offset
    float speed_mult = 1;     // 1 + speed x vel_k
    float per_sec = 0;        // predicted value gain per second while in sight
};
VisionPrediction vision_predict(const struct Npc& n, const PlayerVisibility& pv, const float player_pos[3]);

// Where the NPC is going and what its AI is doing (re/STRUCTS.md §7): the movement motor inside the brain (brain +0xDA0)
// and the brain's current task with its script of "parts". Engine data only; the UI marks estimates (time of arrival).
constexpr int kRoutePts = 48;
constexpr int kRouteParts = 16;
struct Route
{
    bool ok = false;           // motor found (back pointers match)
    bool moving = false;       // has a goal and a path
    uint32_t goal_kind = 0;    // motor +0x70 (0 none; 7 = move to a position, seen live)
    int n = 0;                 // points in walking order: pts[0] = next waypoint ... pts[n-1] = destination
    int total = 0;             // points in the engine's path (n is capped at kRoutePts; first and last always kept)
    float pts[kRoutePts][3];
    bool final_pt = false;     // the destination carries the "final point" flag (bit 0 at point +0x60)
    bool has_dir = false;      // facing at the destination (motor +0x120, or the face object's +0x50)
    float dir[3];
    const char* task = "";     // task type: a static string in the exe ("patrol", "cover", "alert: search", ...)
    char script[40];           // the task's script ("scr_search_play_path"), "" none
    uint8_t nparts = 0;        // parts from the current one on (parts[0] = current)
    uint8_t parts[kRouteParts];
    uint8_t nprev = 0;         // up to 3 parts before the current one, nearest first
    uint8_t prev[3];
};
const char* part_name(uint8_t type);  // "part_walk_path", ... from the exe's table; "" when unknown
void part_names_offline(const std::vector<std::string>& names);  // offline preview: names from a captured frame

struct Npc
{
    uint16_t id;
    const char* cls;    // class name, e.g. "NOSALIS"
    char name[48];      // entity name (copied from the game's string pool)
    float pos[3];       // feet (transform row 3)
    float fwd[3];       // body forward (transform row 2)
    float vis_fov;      // vision FOV in degrees from the NPC's own vision params, 0 = unknown
    float vis_range;    // vision range in metres, 0 = unknown
    float height;       // rough marker height in metres (per class, not game data)
    float health;
    float max_health;   // first value seen for this entity (raised if a later value is higher)
    float dist;         // metres from the player
    uint8_t friend_type;  // entity +0x3D9 (efriend_type)
    Relation rel;
    Category cat;
    bool has_brain;       // AI brain at entity +0xF10
    uint8_t mental;       // brain +0xA51 raw value
    Mood mood;
    bool targets_player;  // the brain's current enemy (brain +0xC70) is the player
    const char* display;  // readable name: story character, else class (static string)
    float awareness;      // 0..1 from the AI state (free 0 .. danger 1); 1 when targeting the player
    int8_t los;           // line of sight to the player: -1 unknown, 0 hidden, 1 visible
    Vision vis;           // exact detection data (vis.ok false: no vision object, awareness is the estimate)
    Route route;          // movement and AI task (route.ok false: not read, out of route range or layout unknown)
};

// Detection percent: the NPC's value against its live "seen" threshold (100 = the moment it spots you).
inline float detect_pct(const Npc& n) { return n.vis.ok && n.vis.seen > 0.f ? 100.f * n.vis.value / n.vis.seen : 0.f; }

// Trigger / script zone (class O_BASEZONE): oriented box = local bounding box (zone +0x150 / +0x160) in the zone's
// transform. The precise shapes (spheres / boxes in the trigger object at +0x178) are not decoded.
struct Zone
{
    uint16_t id;
    char name[48];
    float xf[16];       // world transform (rows right / up / forward / position)
    float bmin[3], bmax[3];
    float dist;         // player -> zone origin
    bool trigger;       // name starts with "trigger"
    bool player_inside;
};

struct FrameData
{
    bool ok = false;           // player + camera readable this frame
    const char* why = "";      // when !ok: which check failed (shown in the menu, logged on change)
    bool paused = false;       // the game's pause stack holds a reason other than "app_inactive" (Esc menu, ...)
    char pause_reason[40]{};   // its top reason, e.g. "ui_menu [default]"
    float player_pos[3]{};
    float vp[16]{};            // camera view-projection, row-major, row vectors (v * VP)
    float aspect = 0;          // camera aspect (viewport width / height)
    float cam_pos[3]{};        // camera position
    float cam_fwd[3]{};        // camera forward
    uint32_t game_time = 0;    // engine game time (ms)
    uint8_t player_friend_type = 0;
    PlayerVisibility pv;       // the player's visibility factors (exact detection data)
    bool detect_ok = false;    // the engine's vision data is available (signatures + layout check passed)
    bool route_ok = false;     // the AI movement / task data is available (signatures passed)
    std::vector<Npc> npcs;     // creatures placed in the world (hidden flag clear), any distance
    std::vector<Zone> zones;   // zones within the zone distance (only when zone display is on)
    int tracked = 0;           // creature slots known in the entity table
    char map[64]{};
};

enum class GameInit
{
    Ok,
    WrongBuild,    // refuse to run at all
    MissingData,   // build OK but a signature failed: hooks may run, no markers
};

GameInit game_init(char* err, size_t errlen);
void game_update(FrameData& out);  // once per frame, render thread

// World -> screen with the game's row-vector VP. Returns false when behind the camera.
bool world_to_screen(const float vp[16], const float p[3], float w, float h, float& sx, float& sy);
