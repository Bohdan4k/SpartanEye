#include "display.h"

#include <cmath>
#include <cstring>

const char* intent_label(Intent i)
{
    switch (i)
    {
    case Intent::Patrol: return "patrol point";
    case Intent::Cover: return "take cover";
    case Intent::Investigate: return "investigate";
    case Intent::Search: return "search";
    case Intent::Body: return "check body";
    case Intent::Attack: return "attack";
    case Intent::Retreat: return "fall back";
    case Intent::Follow: return "follow";
    case Intent::Action: return "action / animation";
    case Intent::Wait: return "wait";
    case Intent::Idle: return "idle";
    case Intent::Scripted: return "scripted";
    default: return "";
    }
}

Intent part_intent(const char* p)
{
    if (!p || strncmp(p, "part_", 5) != 0)
        return Intent::None;
    p += 5;
    // Plain movement and bookkeeping: they carry the NPC somewhere but don't say why.
    static const char* const kPlain[] = {
        "invalid", "walk_path", "run_path", "walk_path_slowly", "walk_path_for_meters", "run_path_for_meters",
        "move_path_for_meters", "slowly_walk_path_for_meters", "walk_path_within_dist", "run_path_within_dist",
        "move_path", "move_path_tracking_goal", "wait_for_movement", "wait_for_turning", "face_path", "set_scr",
        "set_fail_scr", "clear_fail_scr", "set_param", "mem_set", "mem_clear", "precond_set", "precond_clear",
        "call_callback", "set_path_completed_callback", "set_scr_failed_callback", "stop_moving", "movement_type",
        "set_route_search_time", "set_tolerance_distance", "set_action", "set_state", "set_inner_state",
        "clear_inner_state", "action_cooldown", "combat_actions_cooldown", "cover_combat_cooldown",
        "enable_turn_on_place", "disable_ik", "aim_state", "set_move_ideal", "set_p2p_move_ideal", "clear_move_ideal",
        "finish_move_ideal", "finish_p2p_move_ideal", "face_ideal", "face_reasonable", "face_none", "clear"};
    for (const char* k : kPlain)
        if (strcmp(p, k) == 0)
            return Intent::None;
    struct Rule
    {
        const char* key;
        Intent intent;
    };
    static const Rule kRules[] = {
        {"corpse", Intent::Body},        {"cover", Intent::Cover},          {"fallback", Intent::Retreat},
        {"move_away", Intent::Retreat},  {"panic", Intent::Retreat},        {"hold_aloof", Intent::Retreat},
        {"retreat", Intent::Retreat},    {"patrol", Intent::Patrol},        {"threat", Intent::Investigate},
        {"smell", Intent::Investigate},  {"search", Intent::Search},        {"lastposition", Intent::Search},
        {"saveposition", Intent::Search}, {"random_node", Intent::Search},  {"wander", Intent::Search},
        {"walkaround", Intent::Search},  {"enemy", Intent::Attack},         {"attack", Intent::Attack},
        {"melee", Intent::Attack},       {"weapon_fire", Intent::Attack},   {"shooting_target", Intent::Attack},
        {"chase", Intent::Attack},       {"lead", Intent::Follow},          {"follow", Intent::Follow},
        {"script_action", Intent::Action}, {"target_action", Intent::Action}, {"wait_for_action", Intent::Action},
        {"play_motion", Intent::Action}, {"danger_activity", Intent::Action}, {"static_idle", Intent::Action},
        {"play_sound", Intent::Action},  {"wait", Intent::Wait},
    };
    for (const Rule& r : kRules)
        if (strstr(p, r.key))
            return r.intent;
    return Intent::None;
}

Intent route_intent(const Route& r, bool* from_parts)
{
    if (from_parts)
        *from_parts = true;
    for (int i = 0; i < r.nparts; ++i)  // from the current part on: what it does when it gets there
    {
        const Intent k = part_intent(part_name(r.parts[i]));
        if (k != Intent::None)
            return k;
    }
    for (int i = 0; i < r.nprev; ++i)  // the parts that set up the current movement ("get_path_to_cover")
    {
        const Intent k = part_intent(part_name(r.prev[i]));
        if (k != Intent::None)
            return k;
    }
    if (from_parts)
        *from_parts = false;
    const char* t = r.task ? r.task : "";
    if (!*t)
        return Intent::None;
    if (strstr(t, "patrol"))
        return Intent::Patrol;
    if (strstr(t, "cover"))
        return Intent::Cover;
    if (strstr(t, "search"))
        return Intent::Search;
    if (strstr(t, "approach") || strstr(t, "light_alert"))
        return Intent::Investigate;
    if (strstr(t, "combat") || strstr(t, "fire"))
        return Intent::Attack;
    if (strcmp(t, "do_nothing") == 0)
        return Intent::Idle;
    if (strcmp(t, "script") == 0)
        return Intent::Scripted;
    return Intent::None;
}

const char* stage_name(Stage s)
{
    switch (s)
    {
    case Stage::Low: return "low";
    case Stage::Noticed: return "noticed";
    case Stage::Light: return "light";
    case Stage::Spotted: return "spotted";
    default: return "none";
    }
}

Stage raw_stage(const Npc& n)
{
    if (n.targets_player)
        return Stage::Spotted;
    const Vision& v = n.vis;
    if (!v.ok || !v.has_entry || !(v.value > 0.f) || !(v.seen > 0.f))
        return Stage::None;
    // Highest threshold first: in the combat set "noticed" (5.5) and "light" (5) lie above "seen" (0.55).
    if (v.value >= v.seen)
        return Stage::Spotted;
    if (v.value >= v.light)
        return Stage::Light;
    if (v.value >= v.noticed)
        return Stage::Noticed;
    return 100.f * v.value / v.seen >= kGlimpsePct ? Stage::Low : Stage::None;
}

float raw_pct(const Npc& n)
{
    if (n.targets_player)
        return 100.f;
    const Vision& v = n.vis;
    if (!v.ok)  // no vision object: the AI-state estimate
        return n.awareness * 100.f;
    if (!v.has_entry || !(v.value > 0.f) || !(v.seen > 0.f))
        return 0.f;
    const float p = 100.f * v.value / v.seen;
    return p > 100.f ? 100.f : p;
}

bool raw_threat(const Npc& n, Stage displayed)
{
    return n.health > 0.f && n.has_brain && n.rel != Relation::Friendly &&
           (n.targets_player || n.mental >= 10 || displayed != Stage::None);
}

char raw_icon(const Npc& n, Stage displayed)
{
    if (n.health <= 0.f || !n.has_brain || n.rel == Relation::Friendly)
        return 0;
    if (displayed == Stage::Spotted || n.mental >= 40)
        return '!';
    if (displayed == Stage::Noticed || displayed == Stage::Light || n.mental >= 10)
        return '?';
    return 0;
}

namespace
{
int icon_rank(char c) { return c == '!' ? 2 : c == '?' ? 1 : 0; }

// Shared "change after a hold" step: `since` tracks how long `want` has differed from `have`.
template <class T>
void hold_step(T& have, T want, double& since, double t, double hold)
{
    if (want == have)
    {
        since = -1.0;
        return;
    }
    if (since < 0.0)
        since = t;
    if (t - since >= hold)
    {
        have = want;
        since = -1.0;
    }
}
}  // namespace

void display_update(DisplayTrack& d, const Npc& n, double t)
{
    Shown& s = d.s;
    s.raw_stage = raw_stage(n);
    const float prev_raw = s.raw_pct;
    s.raw_pct = raw_pct(n);
    if (!d.init)
    {
        d.init = true;
        d.t = t;
        s.stage = s.raw_stage;
        s.pct = s.stage == Stage::Spotted ? 100.f : s.raw_pct;
        s.trend = 0.f;
        s.trend_dir = 0;
        s.threat = raw_threat(n, s.stage);
        s.icon = raw_icon(n, s.stage);
        s.los = n.los;
        return;
    }
    const double dt = t > d.t ? t - d.t : 0.0;
    d.t = t;

    // Trend of the raw percent (the engine updates the value in steps every 100-300 ms: smooth, then hold).
    if (dt > 0.0)
    {
        const float inst = (s.raw_pct - prev_raw) / static_cast<float>(dt);
        const float k = static_cast<float>(dt) / kTrendTau;
        s.trend += (inst - s.trend) * (k < 1.f ? k : 1.f);
    }

    // Stage: up at once; down after a hold (longer when leaving "spotted"), straight to the current raw stage. While
    // the percent is rising again the drop waits (it would come straight back), up to twice the hold.
    if (s.raw_stage >= s.stage)
    {
        s.stage = s.raw_stage;
        d.stage_low_since = -1.0;
    }
    else
    {
        const double hold = s.stage == Stage::Spotted ? kSpottedHold : kStageHold;
        if (d.stage_low_since < 0.0)
            d.stage_low_since = t;
        const double held = t - d.stage_low_since;
        if (held >= hold && (s.trend <= kTrendMin || held >= 2.0 * hold))
        {
            s.stage = s.raw_stage;
            d.stage_low_since = -1.0;
        }
    }

    // Percent: 100 while "spotted" is shown; else up at once, down at most kFallRate per second.
    if (s.stage == Stage::Spotted)
        s.pct = 100.f;
    else if (s.raw_pct >= s.pct)
        s.pct = s.raw_pct;
    else
    {
        const float p = s.pct - kFallRate * static_cast<float>(dt);
        s.pct = p > s.raw_pct ? p : s.raw_pct;
    }
    const int dir = s.trend > kTrendMin ? 1 : s.trend < -kTrendMin ? -1 : 0;
    if (dir == s.trend_dir)
        d.trend_since = -1.0;
    else
    {
        if (d.trend_since < 0.0 || d.trend_pending != dir)
        {
            d.trend_pending = dir;
            d.trend_since = t;
        }
        if (t - d.trend_since >= kTrendHold)
        {
            s.trend_dir = dir;
            d.trend_since = -1.0;
        }
    }

    // Threat: on at once, off after kThreatHold; a death ends it at once.
    if (n.health <= 0.f)
    {
        s.threat = false;
        d.threat_off_since = -1.0;
    }
    else if (raw_threat(n, s.stage))
    {
        s.threat = true;
        d.threat_off_since = -1.0;
    }
    else
        hold_step(s.threat, false, d.threat_off_since, t, kThreatHold);

    // Icon: up at once, down after a hold.
    const char ri = raw_icon(n, s.stage);
    if (icon_rank(ri) >= icon_rank(s.icon))
    {
        s.icon = ri;
        d.icon_low_since = -1.0;
    }
    else
        hold_step(s.icon, ri, d.icon_low_since, t, n.health <= 0.f ? 0.0 : kIconHold);

    // Line of sight: a sighting (or the first data at all) shows at once; visible -> hidden and known -> unknown
    // after a hold.
    if (n.los == 1 || (s.los == -1 && n.los == 0))
    {
        s.los = n.los;
        d.los_since = -1.0;
    }
    else
        hold_step(s.los, n.los, d.los_since, t, n.los == 0 ? kLosHideHold : kLosForgetHold);
}
