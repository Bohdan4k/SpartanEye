#pragma once
#include "game.h"

// What the overlay shows for one NPC, derived from the engine's raw values with holds and hysteresis so nothing
// blinks. The rules are written down in re/STRUCTS.md §6.6 (keep both in sync). Pure logic: no ImGui, no game
// memory, so overlay_replay can run the same code over detect_log sessions.

// Detection stages from the NPC's live thresholds (the engine's own vision value; re/STRUCTS.md §6).
enum class Stage : uint8_t
{
    None,     // nothing on you (or below a glimpse)
    Low,      // gaining sight, below "noticed"
    Noticed,  // >= noticed: disturbed, comes to look
    Light,    // >= light alert threshold
    Spotted   // >= seen threshold (100 %) or the brain's enemy is you
};
const char* stage_name(Stage s);

struct Shown
{
    Stage raw_stage = Stage::None;  // this frame, from the engine values
    float raw_pct = 0.f;            // this frame: value / live "seen", 0..100 (100 while targeting you)
    Stage stage = Stage::None;      // displayed: rises at once, falls after a hold
    float pct = 0.f;                // displayed: rises at once, falls at most kFallRate per second
    float trend = 0.f;              // change of raw_pct, %/s (smoothed)
    int trend_dir = 0;              // displayed trend: -1 falling, 0 steady, 1 rising
    bool threat = false;            // detecting you / alerted / in combat (held): full detail, cone, arrow, list
    char icon = 0;                  // '!' / '?' / 0 (held)
    int8_t los = -1;                // line of sight: -1 unknown, 0 hidden, 1 visible (held)
};

struct DisplayTrack
{
    Shown s;
    bool init = false;
    double t = 0.0;                 // time of the last update (seconds, any monotonic clock)
    double stage_low_since = -1.0;  // raw stage below the displayed one since
    double threat_off_since = -1.0;
    double icon_low_since = -1.0;
    double los_since = -1.0;        // raw line of sight differs from the displayed one since
    double trend_since = -1.0;
    int trend_pending = 0;
};

// Tunables (seconds, percent). STRUCTS.md §6.6 quotes these values.
constexpr float kGlimpsePct = 1.f;        // below this share of "seen", a value is not a stage ("Low" starts here)
constexpr double kStageHold = 1.0;        // a lower stage must persist this long before it is shown
constexpr double kSpottedHold = 2.0;      // ... for leaving "spotted"
constexpr float kFallRate = 40.f;         // displayed percent falls at most this many points per second
constexpr double kThreatHold = 3.0;       // a threat stays a threat this long after its last reason
constexpr double kIconHold = 1.0;         // "!" -> "?" -> none only after this long
constexpr double kLosHideHold = 1.0;      // visible -> hidden after this long without a fresh sighting
constexpr double kLosForgetHold = 2.0;    // known -> unknown after this long without data
constexpr float kTrendTau = 0.4f;         // smoothing of the percent change
constexpr float kTrendMin = 3.f;          // %/s needed to call it rising / falling
constexpr double kTrendHold = 0.5;

Stage raw_stage(const Npc& n);
float raw_pct(const Npc& n);
// Threat = alive, has an AI, not friendly, and (targets you, alerted or more, or displayed stage >= Low).
bool raw_threat(const Npc& n, Stage displayed);
char raw_icon(const Npc& n, Stage displayed);

// Advances the display state to time t with this frame's engine data. The first call takes the raw state as is.
void display_update(DisplayTrack& d, const Npc& n, double t);

// What the NPC is about to do, from its AI task and script parts (re/STRUCTS.md §7.3).
enum class Intent : uint8_t
{
    None,
    Patrol,       // patrol path / patrol point
    Cover,        // going to / staying in cover
    Investigate,  // a threat: a sound, a sighting, a comrade's alarm (the AI does not say which)
    Search,       // searching: last known position, search path, random nodes
    Body,         // found a body (part_corpse_found)
    Attack,       // chasing / attacking an enemy
    Retreat,      // fall back, move away, panic
    Follow,       // following / leading someone
    Action,       // scripted action or animation at a spot (use an object, idle animation)
    Wait,         // waiting at the spot
    Idle,         // task "do_nothing"
    Scripted      // level script ("script" task): the parts are not visible
};
const char* intent_label(Intent i);
// Category of one part ("part_get_path_to_cover" -> Cover); None for plain movement and bookkeeping parts.
Intent part_intent(const char* part);
// Intent of a route: the first meaningful part from the current one on (what it does when it gets there), else the
// parts that led to the current movement, else the task type. `from_parts` = decided by the script, not the task.
Intent route_intent(const Route& r, bool* from_parts = nullptr);
