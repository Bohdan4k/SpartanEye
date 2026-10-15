#include "ui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "display.h"
#include "imgui.h"
#include "log.h"
#include "state.h"
#include "version.h"

Settings g_settings;

namespace
{
char g_toast[160] = "";
ULONGLONG g_toast_until = 0;
int g_tab_request = -1;      // ui_menu_request
bool g_open_sections = false;

ImGuiTabItemFlags tab_flags(int tab) { return tab == g_tab_request ? ImGuiTabItemFlags_SetSelected : 0; }

// Collapsible menu section (closed by default).
bool section(const char* label)
{
    if (g_open_sections)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    return ImGui::CollapsingHeader(label);
}

// Colour language (legend in the menu):
// - marker hue (box, small marker, name, edge arrow) = relation: friendly / neutral / hostile / dead;
// - one ramp for detection and AI state (bar, percent, "?" / "!", view cones, alerts): white calm, yellow sees you,
//   amber noticed / alerted, orange light alert, red spotted / combat;
// - health = white; behind a wall = faded and / or dashed (shape, not colour).
const ImU32 kOutline = IM_COL32(0, 0, 0, 230);
const ImU32 kCalm = IM_COL32(225, 225, 225, 255);
const ImU32 kSees = IM_COL32(240, 215, 70, 255);
const ImU32 kNoticed = IM_COL32(255, 165, 40, 255);
const ImU32 kLightAlert = IM_COL32(255, 110, 40, 255);
const ImU32 kTargetsYou = IM_COL32(255, 60, 60, 255);
const ImU32 kHealth = IM_COL32(235, 235, 235, 255);

ImU32 relation_color(Relation r)
{
    switch (r)
    {
    case Relation::Friendly: return g_settings.col_friendly;
    case Relation::Neutral: return g_settings.col_neutral;
    default: return g_settings.col_hostile;
    }
}

// AI state on the detection ramp (for NPCs whose eyes hold nothing on you, or without vision data).
ImU32 mood_color(Mood m)
{
    switch (m)
    {
    case Mood::Alert: return kNoticed;
    case Mood::Combat: return kTargetsYou;
    default: return kCalm;
    }
}

ImU32 stage_color(Stage s)
{
    switch (s)
    {
    case Stage::Low: return kSees;
    case Stage::Noticed: return kNoticed;
    case Stage::Light: return kLightAlert;
    case Stage::Spotted: return kTargetsYou;
    default: return kCalm;
    }
}

// One NPC as drawn this frame: the engine data (live, or the last known data while it fades out after leaving the
// list) plus its display state (display.h: holds and hysteresis, rules in re/STRUCTS.md §6.6).
struct Track;
struct View
{
    Npc n;
    Shown s;
    float k = 1.f;       // presence: fades in when it appears, out after it left the NPC list
    bool aimed = false;  // under the crosshair (held)
    float speed = 0.f;   // m/s over game time, smoothed (estimate, for the time of arrival)
    const Track* tr = nullptr;  // its track (previous route while it fades out)
};

// The NPC's place on the detection ramp: its displayed stage, else its AI state.
ImU32 threat_color(const View& v) { return v.s.stage != Stage::None ? stage_color(v.s.stage) : mood_color(v.n.mood); }

// Colour of the detection bar at its fill level, against the ticks drawn on it (the live thresholds).
ImU32 fill_color(const View& v)
{
    const Vision& x = v.n.vis;
    const float p = v.s.pct;
    if (p >= 99.95f)
        return kTargetsYou;
    if (!x.ok || x.seen <= 0.f || p <= 0.f)
        return stage_color(v.s.stage);
    if (p >= 100.f * x.light / x.seen)
        return kLightAlert;
    if (p >= 100.f * x.noticed / x.seen)
        return kNoticed;
    return kSees;
}

// Alerted (disturbed or more) while its eyes hold nothing on you: sound, a body, a comrade's alarm, or lost sight.
bool alerted_not_by_sight(const View& v)
{
    return v.n.has_brain && v.n.mental >= 10 && !v.n.targets_player && v.s.stage == Stage::None;
}

void color_edit(const char* label, uint32_t& c)
{
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    float f[4] = {v.x, v.y, v.z, v.w};
    if (ImGui::ColorEdit4(label, f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
        c = ImGui::ColorConvertFloat4ToU32(ImVec4(f[0], f[1], f[2], f[3]));
}

// Key choice from the safe list; keys bound to the other two actions are greyed out.
void key_combo(const char* label, int& key, int other1, int other2)
{
    int n = 0;
    const KeyName* keys = config_key_list(n);
    if (ImGui::BeginCombo(label, config_key_name(key)))
    {
        for (int i = 0; i < n; ++i)
        {
            bool taken = keys[i].vk == other1 || keys[i].vk == other2;
            if (ImGui::Selectable(keys[i].name, keys[i].vk == key, taken ? ImGuiSelectableFlags_Disabled : 0))
                key = keys[i].vk;
        }
        ImGui::EndCombo();
    }
}

// Text with a 1 px black outline: readable on bright and dark backgrounds.
void text_outlined(ImDrawList* dl, float size, ImVec2 p, ImU32 col, const char* s)
{
    ImFont* f = ImGui::GetFont();
    const ImVec2 off[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const ImVec2& o : off)
        dl->AddText(f, size, ImVec2(p.x + o.x, p.y + o.y), (kOutline & 0x00FFFFFF) | (col & 0xFF000000), s);
    dl->AddText(f, size, p, col, s);
}

ImVec2 text_size(float size, const char* s)
{
    ImVec2 t = ImGui::CalcTextSize(s);
    float k = size / ImGui::GetFontSize();
    return ImVec2(t.x * k, t.y * k);
}

bool passes_filters(const Npc& n)
{
    const Settings& s = g_settings;
    if (n.health <= 0.f && !s.show_dead)
        return false;
    bool cat = (n.cat == Category::Human && s.show_human) || (n.cat == Category::Mutant && s.show_mutant) ||
               (n.cat == Category::Fauna && s.show_fauna);
    bool rel = (n.rel == Relation::Friendly && s.show_friendly) || (n.rel == Relation::Neutral && s.show_neutral) ||
               (n.rel == Relation::Hostile && s.show_hostile);
    return cat && rel;
}

float health_frac(const Npc& n)
{
    float f = n.max_health > 0.f ? n.health / n.max_health : 0.f;
    return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
}

// ---------------------------------------------------------------------------------------------------------------
// Markers: geometry first, labels afterwards (so they can be moved apart).

struct RectF
{
    float x0, y0, x1, y1;
    bool overlaps(const RectF& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
};

struct Line
{
    char text[160];
    ImU32 col;
    float size;
};

// Level of detail: Dot = small marker only, Compact = box + one label line, Full = bars, state, every label.
enum class Detail
{
    Dot,
    Compact,
    Full
};

struct Marker
{
    const View* v;
    ImVec2 a, b;        // box (screen)
    float fx;           // feet x
    ImU32 col;
    bool dead;
    bool focus;         // threat or under the crosshair: full detail, never faded by distance
    Detail detail;
    float alpha;        // distance fade (1 = none)
    float shrink;       // distance shrink of lines and labels (1 = none)
    float label_top;    // preferred label y (below the box / bottom bar)
    Line lines[7];
    int nlines = 0;
};

float ui_scale(float h) { return h / 1080.f < 1.f ? 1.f : h / 1080.f; }

// Line-of-sight styling: is this NPC drawn as "behind a wall"? (Displayed, held line of sight.)
bool los_hidden(const View& v)
{
    if (!g_settings.los_enabled)
        return false;
    if (v.s.los < 0)
        return g_settings.los_unknown == 1;
    return v.s.los == 0;
}

ImU32 fade(ImU32 c, float k)
{
    k = k < 0.f ? 0.f : k > 1.f ? 1.f : k;
    unsigned a = static_cast<unsigned>(((c >> 24) & 0xFF) * k);
    return (c & 0x00FFFFFF) | (a << 24);
}

float g_vw = 0.f, g_vh = 0.f;  // viewport of the current frame (ui_draw)

// Parameter range [t0, t1] of segment a-b inside the viewport grown by `m` pixels (Liang-Barsky). False if outside.
bool clip_to_view(ImVec2 a, ImVec2 b, float m, float& t0, float& t1)
{
    t0 = 0.f;
    t1 = 1.f;
    const float d[2] = {b.x - a.x, b.y - a.y}, lo[2] = {-m - a.x, -m - a.y},
                hi[2] = {g_vw + m - a.x, g_vh + m - a.y};
    for (int i = 0; i < 2; ++i)
    {
        if (fabsf(d[i]) < 1e-6f)
        {
            if (lo[i] > 0.f || hi[i] < 0.f)
                return false;
            continue;
        }
        float ta = lo[i] / d[i], tb = hi[i] / d[i];
        if (ta > tb)
            std::swap(ta, tb);
        t0 = fmaxf(t0, ta);
        t1 = fminf(t1, tb);
    }
    return t0 < t1;
}

// Dashes along a-b, only where the segment crosses the viewport (a line through the screen from far outside would
// otherwise produce thousands of dashes). The dash phase is measured from `a`, so it stays put when `a` is off screen.
void dashed_line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t, float dash)
{
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    float t0, t1;
    if (len < 1.f || !clip_to_view(a, b, t + 2.f, t0, t1))
        return;
    dx /= len;
    dy /= len;
    const float period = dash * 2.f, s_end = t1 * len;
    for (float s = floorf(t0 * len / period) * period; s < s_end; s += period)
    {
        float e = fminf(s + dash, len);
        dl->AddLine(ImVec2(a.x + dx * s, a.y + dy * s), ImVec2(a.x + dx * e, a.y + dy * e), col, t);
    }
}

bool project_box(const FrameData& fd, const Npc& n, float w, float h, ImVec2& a, ImVec2& b, float& fx)
{
    float fy, hx, hy;
    const float head[3] = {n.pos[0], n.pos[1] + n.height, n.pos[2]};
    if (!world_to_screen(fd.vp, n.pos, w, h, fx, fy) || !world_to_screen(fd.vp, head, w, h, hx, hy))
        return false;
    float bh = fy - hy;
    if (bh < 6.f)
        bh = 6.f;
    float bw = bh * 0.5f < 6.f ? 6.f : bh * 0.5f;
    a = ImVec2(fx - bw * 0.5f, fy - bh);
    b = ImVec2(fx + bw * 0.5f, fy);
    return !(b.x < 0 || a.x > w || b.y < 0 || a.y > h);
}

void draw_box(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t, bool dashed = false)
{
    if (dashed && g_settings.box_style != 2)
    {
        float d = fmaxf(4.f, (b.y - a.y) * 0.08f);
        const ImVec2 c[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
        for (int i = 0; i < 4; ++i)
            dashed_line(dl, c[i], c[(i + 1) % 4], col, t, d);
        return;
    }
    const ImU32 ol = fade(kOutline, ((col >> 24) & 0xFF) / 255.f);  // a faded marker gets a faded outline
    switch (g_settings.box_style)
    {
    case 0:
        dl->AddRect(a, b, ol, 0.f, 0, t + 2.f);
        dl->AddRect(a, b, col, 0.f, 0, t);
        break;
    case 1:
    {
        float l = fminf(b.x - a.x, b.y - a.y) * 0.3f;
        const ImVec2 pts[8][2] = {{a, {a.x + l, a.y}},       {a, {a.x, a.y + l}},       {{b.x, a.y}, {b.x - l, a.y}},
                                  {{b.x, a.y}, {b.x, a.y + l}}, {{a.x, b.y}, {a.x + l, b.y}}, {{a.x, b.y}, {a.x, b.y - l}},
                                  {b, {b.x - l, b.y}},       {b, {b.x, b.y - l}}};
        for (auto& p : pts)
            dl->AddLine(p[0], p[1], ol, t + 2.f);
        for (auto& p : pts)
            dl->AddLine(p[0], p[1], col, t);
        break;
    }
    default: break;
    }
}

// Health bar along one side of the box. Returns the extra space it takes below the box (for the labels).
float draw_health(ImDrawList* dl, const Marker& m, float px, float fs)
{
    const Settings& s = g_settings;
    const float k = m.alpha;
    float frac = health_frac(m.v->n);
    float thick = fmaxf(3.f, 4.f * px);
    float gap = 2.f * px;
    ImVec2 ba, bb;
    bool vertical = s.hb_pos >= 2;
    float bw = m.b.x - m.a.x;
    if (!vertical)
    {
        float len = fmaxf(bw, fs * 2.5f);
        float y0 = s.hb_pos == 0 ? m.a.y - gap - thick : m.b.y + gap;
        ba = ImVec2(m.fx - len * 0.5f, y0);
        bb = ImVec2(m.fx + len * 0.5f, y0 + thick);
    }
    else
    {
        float x0 = s.hb_pos == 2 ? m.a.x - gap - thick : m.b.x + gap;
        ba = ImVec2(x0, m.a.y);
        bb = ImVec2(x0 + thick, m.b.y);
    }
    dl->AddRectFilled(ImVec2(ba.x - 1, ba.y - 1), ImVec2(bb.x + 1, bb.y + 1), fade(kOutline, k));
    dl->AddRectFilled(ba, bb, fade(IM_COL32(60, 60, 60, 200), k));
    const ImU32 hc = fade(kHealth, k);
    if (s.hb_style == 2)  // segmented: 10 cells
    {
        const int cells = 10;
        int lit = static_cast<int>(ceilf(frac * cells - 0.01f));
        for (int i = 0; i < lit; ++i)
        {
            float t0 = static_cast<float>(i) / cells, t1 = static_cast<float>(i + 1) / cells;
            if (!vertical)
                dl->AddRectFilled(ImVec2(ba.x + (bb.x - ba.x) * t0 + 1, ba.y), ImVec2(ba.x + (bb.x - ba.x) * t1 - 1, bb.y), hc);
            else
                dl->AddRectFilled(ImVec2(ba.x, bb.y - (bb.y - ba.y) * t1 + 1), ImVec2(bb.x, bb.y - (bb.y - ba.y) * t0 - 1), hc);
        }
    }
    else if (!vertical)
        dl->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * frac, bb.y), hc);
    else
        dl->AddRectFilled(ImVec2(ba.x, bb.y - (bb.y - ba.y) * frac), bb, hc);
    if (s.hb_style == 1)  // percent next to the bar
    {
        char t[16];
        snprintf(t, sizeof(t), "%.0f%%", frac * 100.f);
        float ts = fs * 0.75f * s.label_scale;
        ImVec2 tz = text_size(ts, t);
        ImVec2 p = !vertical ? ImVec2(bb.x + gap * 2, (ba.y + bb.y - tz.y) * 0.5f)
                             : ImVec2(s.hb_pos == 2 ? ba.x - tz.x - gap : bb.x + gap, ba.y - tz.y);
        text_outlined(dl, ts, p, hc, t);
    }
    return s.hb_pos == 1 ? gap + thick : 0.f;
}

void add_line(Marker& m, ImU32 col, float size, const char* fmt, ...)
{
    if (m.nlines >= 7)
        return;
    Line& l = m.lines[m.nlines++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l.text, sizeof(l.text), fmt, ap);
    va_end(ap);
    l.col = col;
    l.size = size;
}

void build_labels(Marker& m, float fs)
{
    const Settings& s = g_settings;
    const View& v = *m.v;
    const Npc& n = v.n;
    if (m.detail == Detail::Dot)
        return;
    const bool full = m.detail == Detail::Full;
    const float ls = fs * (full ? 0.95f : 0.85f) * s.label_scale * m.shrink;
    char head[128] = "";
    if (s.lbl_name)
        strncpy_s(head, n.display, _TRUNCATE);
    if (s.lbl_class && strcmp(n.display, n.cls) != 0)
    {
        char c[64];
        snprintf(c, sizeof(c), "%s[%s]", head[0] ? " " : "", n.cls);
        strncat_s(head, c, _TRUNCATE);
    }
    if (s.lbl_distance)
    {
        char d[32];
        snprintf(d, sizeof(d), "%s%.0f m", head[0] ? "  " : "", n.dist);
        strncat_s(head, d, _TRUNCATE);
    }
    if (head[0])
        add_line(m, m.col, ls, "%s", head);
    if (m.dead)
        add_line(m, m.col, ls, "dead");
    else if (full)
    {
        if (s.lbl_health)
            add_line(m, fade(kHealth, m.alpha), ls, "hp %.0f%%", health_frac(n) * 100.f);
        // Calm with nothing on you: no state line (the box says enough); friendly NPCs never detect you.
        const bool quiet = n.rel == Relation::Friendly || (n.mood == Mood::Calm && v.s.stage == Stage::None);
        if (s.show_state && n.has_brain && !(quiet && s.smart_detail))
        {
            char aw[24] = "";
            if (s.show_awareness && n.vis.ok)  // exact: the NPC's own value against its live "seen" threshold
                snprintf(aw, sizeof(aw), " %.0f%%%s", v.s.pct,
                         v.s.stage == Stage::Spotted ? "" : v.s.trend_dir > 0 ? " rising" : v.s.trend_dir < 0 ? " falling" : "");
            else if (s.show_awareness)  // estimate from the AI state
                snprintf(aw, sizeof(aw), " ~%.0f%%", v.s.pct);
            add_line(m, fade(threat_color(v), m.alpha), ls, "%s%s%s", mental_state_name(n.mental), aw,
                     n.targets_player ? "  - targeting you" : alerted_not_by_sight(v) ? "  - alerted, can't see you" : "");
        }
        if (s.show_routes && n.route.ok)  // what its AI is doing (re/STRUCTS.md §7.3)
        {
            const Intent in = route_intent(n.route);
            if (in != Intent::None)
                add_line(m, fade(threat_color(v), m.alpha), ls * 0.9f, "%s %s", n.route.moving ? "going to" : "now:",
                         intent_label(in));
        }
    }
    if (s.debug_labels && full)
    {
        add_line(m, IM_COL32(210, 210, 210, 255), ls * 0.85f, "%s [%s] #%u  ft=%u %s  hp %.2f/%.2f  ms=%u", n.name, n.cls,
                 n.id, n.friend_type, friend_type_name(n.friend_type), n.health, n.max_health, n.mental);
        if (n.vis.ok)
            add_line(m, IM_COL32(210, 210, 210, 255), ls * 0.85f,
                     "vision %s%.2f  thr %.2f/%.2f/%.2f%s  R %.0f bwd %.0f fov %.0f force %.1f  %+.2f/s",
                     n.vis.has_entry ? "" : "-", n.vis.value, n.vis.noticed, n.vis.light, n.vis.seen, n.vis.surv ? " surv" : "",
                     n.vis.range, n.vis.back_range, n.vis.fov * 57.2958f, n.vis.force_range, n.vis.rate);
        add_line(m, IM_COL32(210, 210, 210, 255), ls * 0.85f, "raw %.1f%% %s  shown %.0f%% %s  trend %+.1f%%/s  los %d->%d",
                 v.s.raw_pct, stage_name(v.s.raw_stage), v.s.pct, stage_name(v.s.stage), v.s.trend, n.los, v.s.los);
        if (n.route.ok)
            add_line(m, IM_COL32(210, 210, 210, 255), ls * 0.85f, "task [%s] scr [%s] part [%s] path %d/%d%s  %.1f m/s",
                     n.route.task, n.route.script, n.route.nparts ? part_name(n.route.parts[0]) : "", n.route.n,
                     n.route.total, n.route.final_pt ? " final" : "", v.speed);
    }
}

std::unordered_map<uint16_t, float> g_label_dy;  // label offset below its natural place, last frame (by NPC id)

// Places label blocks in the given order (focus first, then by sticky distance). A block keeps last frame's offset
// while that spot is free and slides back towards its natural place; only a block that would overlap is pushed down
// below the one in its way. So labels move only when they must, and smoothly when they return.
void draw_labels(ImDrawList* dl, std::vector<Marker>& ms, float px)
{
    static std::vector<RectF> placed;
    placed.clear();
    const float slide = ImGui::GetIO().DeltaTime * 150.f * px;  // pixels per frame on the way back
    auto free_at = [&](const RectF& r) {
        for (const RectF& p : placed)
            if (r.overlaps(p))
                return false;
        return true;
    };
    for (Marker& m : ms)
    {
        if (!m.nlines)
            continue;
        float bw = 0, bh = 0;
        for (int i = 0; i < m.nlines; ++i)
        {
            ImVec2 t = text_size(m.lines[i].size, m.lines[i].text);
            bw = fmaxf(bw, t.x);
            bh += t.y;
        }
        const RectF home{m.fx - bw * 0.5f, m.label_top, m.fx + bw * 0.5f, m.label_top + bh};
        RectF r = home;
        if (g_settings.label_declutter)
        {
            auto shifted = [&](float dy) { return RectF{home.x0, home.y0 + dy, home.x1, home.y1 + dy}; };
            auto it = g_label_dy.find(m.v->n.id);
            const float prev = it != g_label_dy.end() ? it->second : 0.f;
            const float back = fmaxf(0.f, prev - slide);
            if (prev > 0.f && free_at(shifted(back)))
                r = shifted(back);
            else if (prev > 0.f && free_at(shifted(prev)))
                r = shifted(prev);
            else if (!free_at(home))
                for (int iter = 0; iter < 12; ++iter)
                {
                    bool moved = false;
                    for (const RectF& p : placed)
                        if (r.overlaps(p))
                        {
                            float dy = p.y1 + 1.f - r.y0;
                            r.y0 += dy;
                            r.y1 += dy;
                            moved = true;
                        }
                    if (!moved)
                        break;
                }
            g_label_dy[m.v->n.id] = r.y0 - home.y0;
            if (r.y0 - m.label_top > 4.f * px)  // leader line back to the box
                dl->AddLine(ImVec2(m.fx, m.b.y), ImVec2(m.fx, r.y0), (m.col & 0x00FFFFFF) | 0x80000000, 1.f);
        }
        placed.push_back(r);
        float y = r.y0;
        for (int i = 0; i < m.nlines; ++i)
        {
            ImVec2 t = text_size(m.lines[i].size, m.lines[i].text);
            text_outlined(dl, m.lines[i].size, ImVec2(m.fx - t.x * 0.5f, y), m.lines[i].col, m.lines[i].text);
            y += t.y;
        }
    }
}

// Arrow on the screen edge pointing towards an NPC that is off screen or behind the camera.
void draw_edge_arrow(ImDrawList* dl, const FrameData& fd, const View& v, float w, float h, float k)
{
    const Npc& n = v.n;
    const float p[3] = {n.pos[0], n.pos[1] + n.height * 0.5f, n.pos[2]};
    const float* vp = fd.vp;
    float cx = p[0] * vp[0] + p[1] * vp[4] + p[2] * vp[8] + vp[12];
    float cy = p[0] * vp[1] + p[1] * vp[5] + p[2] * vp[9] + vp[13];
    float cw = p[0] * vp[3] + p[1] * vp[7] + p[2] * vp[11] + vp[15];
    float aw = fabsf(cw) > 1e-3f ? fabsf(cw) : 1e-3f;
    float dx = cx / aw, dy = -cy / aw;  // screen direction from the centre (y down); keeps left/right when behind
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f)
    {
        dx = 0;
        dy = 1;  // straight behind: point down
    }
    else
    {
        dx /= len;
        dy /= len;
    }
    const float fs = ImGui::GetFontSize();
    const float margin = fs * 2.2f, size = fs * 0.8f;
    float rx = w * 0.5f - margin, ry = h * 0.5f - margin;
    float t = fminf(fabsf(dx) > 1e-4f ? rx / fabsf(dx) : 1e9f, fabsf(dy) > 1e-4f ? ry / fabsf(dy) : 1e9f);
    ImVec2 c(w * 0.5f + dx * t, h * 0.5f + dy * t);
    ImVec2 tip(c.x + dx * size, c.y + dy * size);
    ImVec2 l(c.x - dx * size * 0.6f - dy * size * 0.7f, c.y - dy * size * 0.6f + dx * size * 0.7f);
    ImVec2 r(c.x - dx * size * 0.6f + dy * size * 0.7f, c.y - dy * size * 0.6f - dx * size * 0.7f);
    const ImU32 col = fade(n.health <= 0.f ? g_settings.col_dead : relation_color(n.rel), k);
    dl->AddTriangleFilled(tip, l, r, col);
    dl->AddTriangle(tip, l, r, fade(kOutline, k), 1.5f);
    if (n.targets_player)
        dl->AddCircle(c, size * 1.6f, kTargetsYou, 0, 2.0f);
    char d[32];
    if (n.vis.ok && v.s.threat && v.s.pct > 0.f)
        snprintf(d, sizeof(d), "%.0f m %.0f%%", n.dist, v.s.pct);
    else
        snprintf(d, sizeof(d), "%.0f m", n.dist);
    float ts = fs * 0.85f * g_settings.label_scale;
    ImVec2 tz = text_size(ts, d);
    const ImU32 tc = v.s.threat ? fade(threat_color(v), k) : col;  // the percent / state on the ramp, else relation
    text_outlined(dl, ts, ImVec2(c.x - dx * size * 1.8f - tz.x * 0.5f, c.y - dy * size * 1.8f - tz.y * 0.5f), tc, d);
}

// ---------------------------------------------------------------------------------------------------------------
// Stealth: awareness bar, view cones, alerts.

// Detection bar under the box: exact (value / live seen threshold) with ticks at "noticed" and "light alert";
// NPCs without a vision object get the AI-state estimate, drawn faded.
float draw_awareness(ImDrawList* dl, const Marker& m, float y, float px, float fs)
{
    const View& v = *m.v;
    const Npc& n = v.n;
    if (!g_settings.show_awareness || m.dead || !n.has_brain || m.detail != Detail::Full)
        return 0.f;
    if (g_settings.smart_detail && n.rel == Relation::Friendly)
        return 0.f;
    const float k = m.alpha;
    const float thick = fmaxf(3.f, 3.5f * px), len = fmaxf(m.b.x - m.a.x, fs * 3.f);
    ImVec2 a(m.fx - len * 0.5f, y + px), b(m.fx + len * 0.5f, y + px + thick);
    const float pct = v.s.pct;  // displayed (held) percent: re/STRUCTS.md §6.6
    if (n.vis.ok)
    {
        if (pct <= 0.f && n.mental == 0 && !v.s.threat)
            return 0.f;
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), fade(kOutline, k));
        dl->AddRectFilled(a, ImVec2(a.x + len * pct / 100.f, b.y), fade(fill_color(v), k));
        if (v.s.stage != Stage::Spotted && n.vis.seen > 0.f)
            for (float t : {n.vis.noticed / n.vis.seen, n.vis.light / n.vis.seen})
                if (t > 0.f && t < 1.f)
                    dl->AddLine(ImVec2(a.x + len * t, a.y - px), ImVec2(a.x + len * t, b.y + px),
                                fade(IM_COL32(255, 255, 255, 200), k), fmaxf(1.f, px));
    }
    else
    {
        if (pct <= 0.f)
            return 0.f;
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), fade(kOutline, k));
        dl->AddRectFilled(a, ImVec2(a.x + len * pct / 100.f, b.y),
                          fade(n.targets_player ? kTargetsYou : mood_color(n.mood), 0.55f * k));
    }
    return thick + 2.f * px;
}

// Clip-space point (row-vector VP: w = depth along the camera's forward, metres).
struct ClipPt
{
    float x, y, w;
};

ClipPt to_clip(const float vp[16], const ImVec4& p)
{
    return {p.x * vp[0] + p.y * vp[4] + p.z * vp[8] + vp[12], p.x * vp[1] + p.y * vp[5] + p.z * vp[9] + vp[13],
            p.x * vp[3] + p.y * vp[7] + p.z * vp[11] + vp[15]};
}

ImVec2 clip_to_screen(const ClipPt& c) { return ImVec2((c.x / c.w * 0.5f + 0.5f) * g_vw, (0.5f - c.y / c.w * 0.5f) * g_vh); }

ClipPt clip_lerp(const ClipPt& a, const ClipPt& b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t};
}

// Ground geometry closer to the camera plane than this is cut off rather than projected: a ground line passing next
// to the camera would otherwise project to a screen-crossing line, and pop in and out as its end crosses the plane.
constexpr float kNearCut = 0.3f;

bool clip_near(ClipPt& a, ClipPt& b)
{
    if (a.w < kNearCut && b.w < kNearCut)
        return false;
    if (a.w < kNearCut)
        a = clip_lerp(a, b, (kNearCut - a.w) / (b.w - a.w));
    else if (b.w < kNearCut)
        b = clip_lerp(b, a, (kNearCut - b.w) / (a.w - b.w));
    return true;
}

// Polyline / polygon on the ground, cut at the near plane. The fill is a fan of triangles from the first point, each
// clipped on its own (a clipped convex triangle stays convex), so it never pops on or off as a whole.
void ground_shape(ImDrawList* dl, const FrameData& fd, const std::vector<ImVec4>& wp, bool closed, ImU32 fill,
                  ImU32 line, float thick, bool dashed)
{
    static std::vector<ClipPt> cp;
    cp.resize(wp.size());
    for (size_t i = 0; i < wp.size(); ++i)
        cp[i] = to_clip(fd.vp, wp[i]);
    if ((fill >> 24) && wp.size() >= 3)
    {
        const ImDrawListFlags saved = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // no fringes: neighbouring triangles would show seams
        for (size_t i = 1; i + 1 < wp.size(); ++i)
        {
            const ClipPt tri[3] = {cp[0], cp[i], cp[i + 1]};
            ImVec2 poly[4];
            int n = 0;
            for (int k = 0; k < 3; ++k)  // Sutherland-Hodgman against w >= kNearCut
            {
                const ClipPt &a = tri[k], &b = tri[(k + 1) % 3];
                const bool ina = a.w >= kNearCut, inb = b.w >= kNearCut;
                if (ina)
                    poly[n++] = clip_to_screen(a);
                if (ina != inb)
                    poly[n++] = clip_to_screen(clip_lerp(a, b, (kNearCut - a.w) / (b.w - a.w)));
            }
            if (n >= 3)
                dl->AddConvexPolyFilled(poly, n, fill);
        }
        dl->Flags = saved;
    }
    const size_t nseg = closed ? wp.size() : wp.size() - 1;
    for (size_t i = 0; i < nseg; ++i)
    {
        ClipPt a = cp[i], b = cp[(i + 1) % wp.size()];
        if (!clip_near(a, b))
            continue;
        const ImVec2 sa = clip_to_screen(a), sb = clip_to_screen(b);
        if (dashed)
            dashed_line(dl, sa, sb, line, thick, thick * 4.f);
        else
            dl->AddLine(sa, sb, line, thick);
    }
}

// Ground zones of the NPC's real vision: the cone (range at the centre shrinking to range x k_edge at the FOV edge),
// the all-round zone (back range), the instant zone (force range) and, optionally, the peripheral zone. NPCs without
// a vision object (or with real cones off) get the slider cone.
void draw_cone(ImDrawList* dl, const FrameData& fd, const View& view)
{
    const Npc& n = view.n;
    float fx = n.fwd[0], fz = n.fwd[2], l = sqrtf(fx * fx + fz * fz);
    if (l < 1e-3f)
        return;
    fx /= l;
    fz /= l;
    const float y = n.pos[1] + 0.05f;
    static std::vector<ImVec4> pts;
    auto at = [&](float ang, float r) {  // rotate the forward vector around Y (left-handed, Y up)
        float c = cosf(ang), s = sinf(ang);
        return ImVec4(n.pos[0] + (fx * c + fz * s) * r, y, n.pos[2] + (-fx * s + fz * c) * r, 0.f);
    };
    const Stage st = view.s.stage;
    // Fades in / out with the marker, and over the last 3 m before the cone distance (no pop at the limit).
    const float near_edge = fminf(1.f, fmaxf(0.f, (g_settings.cone_max_dist - n.dist) / 3.f));
    const ImU32 col = fade(threat_color(view), view.k * near_edge);
    const float ka = ((col >> 24) & 0xFF) / 255.f;
    auto with_alpha = [&](ImU32 a) { return fade((col & 0x00FFFFFF) | a, ka); };
    if (!(g_settings.cone_real && n.vis.ok))
    {
        const float fov = (n.vis_fov > 0.f ? n.vis_fov : g_settings.cone_fov) * 3.14159265f / 180.f;
        pts.clear();
        pts.push_back(ImVec4(n.pos[0], y, n.pos[2], 0.f));
        for (int i = 0; i <= 14; ++i)
            pts.push_back(at(-fov * 0.5f + fov * i / 14, g_settings.cone_length));
        ground_shape(dl, fd, pts, true, with_alpha(0x38000000), with_alpha(0xB0000000), 1.5f, true);
        return;
    }
    const Vision& v = n.vis;
    const float half = v.fov * 0.5f, rc = v.range * v.k_centre, re = v.range * v.k_edge;
    pts.clear();
    pts.push_back(ImVec4(n.pos[0], y, n.pos[2], 0.f));
    const int seg = 24;
    for (int i = 0; i <= seg; ++i)
    {
        float a = -half + v.fov * i / seg;
        pts.push_back(at(a, (1.f - fabsf(a) / half) * (rc - re) + re));
    }
    // Fill only where the cone still matters for stealth (noticing / alerted) and you are outside it: in combat, once
    // it has spotted you, or while you stand in it (it would tint half the screen; the bar already says it), an
    // outline is enough. Several overlapping red fills in a fight would wash out the screen.
    const bool fight = st == Stage::Spotted || n.mood == Mood::Combat;
    const VisionPrediction where = vision_predict(n, fd.pv, fd.player_pos);
    const bool inside = where.in_cone || where.in_back || where.in_force;
    const ImU32 fill = fight || inside ? 0 : st != Stage::None || n.mood != Mood::Calm ? 0x1E000000 : 0x0C000000;
    ground_shape(dl, fd, pts, true, with_alpha(fill), with_alpha(0xB0000000), 1.6f, false);
    auto ring = [&](float r, ImU32 c, float t, bool dashed) {
        pts.clear();
        for (int i = 0; i < 40; ++i)
            pts.push_back(at(6.2831853f * i / 40, r));
        ground_shape(dl, fd, pts, true, 0, c, t, dashed);
    };
    if (g_settings.cone_back && v.back_range > 0.f)
        ring(v.back_range, with_alpha(0x80000000), 1.2f, true);
    if (g_settings.cone_back && v.force_range > 0.f)
        ring(v.force_range, fade(IM_COL32(255, 60, 60, 200), ka), 1.5f, false);
    if (g_settings.cone_periph && v.peri_dist > 0.f && v.peri_angle > 0.f)
    {
        pts.clear();
        pts.push_back(ImVec4(n.pos[0], y, n.pos[2], 0.f));
        for (int i = 0; i <= 12; ++i)
            pts.push_back(at(-v.peri_angle * 0.5f + v.peri_angle * i / 12, v.peri_dist));
        ground_shape(dl, fd, pts, true, 0, with_alpha(0x90000000), 1.2f, true);
    }
}

// "Who is detecting me": NPCs with a value for you or an alerted state, most detected first. Then, optionally, the
// "why" panel: your visibility factors and the formula terms of the top NPC.
std::unordered_map<uint16_t, float> g_row_key;  // sticky sort key of the detection list (by NPC id)

void draw_detect_panels(const FrameData& fd, const std::vector<View>& views, float w, float h)
{
    const Settings& s = g_settings;
    if (!fd.ok || (!s.det_list && !s.det_factors))
        return;
    static std::vector<const View*> rows;
    rows.clear();
    for (const View& v : views)  // threats (held: re/STRUCTS.md §6.6), so rows don't blink in and out
        if (v.s.threat && v.n.dist <= s.det_list_dist)
            rows.push_back(&v);
    // Most detected first. The key only moves when it changes by more than 8 points, so two NPCs at similar levels
    // don't swap rows every frame.
    for (const View* v : rows)
    {
        const float k = (v->s.stage == Stage::Spotted ? 1000.f : 0.f) + v->s.pct + v->n.mental * 0.1f;
        auto it = g_row_key.find(v->n.id);
        if (it == g_row_key.end())
            g_row_key[v->n.id] = k;
        else if (fabsf(it->second - k) > 8.f)
            it->second = k;
    }
    std::sort(rows.begin(), rows.end(), [&](const View* a, const View* b) {
        const float ka = g_row_key[a->n.id], kb = g_row_key[b->n.id];
        return ka != kb ? ka > kb : a->n.id < b->n.id;
    });
    const bool list = s.det_list && !(rows.empty() && s.det_list_hide_empty);
    if (!list && !s.det_factors)
        return;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float fs = ImGui::GetFontSize(), pad = fs * 0.45f, row = fs * 1.35f;
    const ImU32 cT = IM_COL32(230, 230, 230, 255), cD = IM_COL32(160, 160, 160, 255);
    struct TextLine
    {
        ImU32 col;
        char text[192];
    };
    static std::vector<TextLine> lines;
    lines.clear();
    auto line = [&](ImU32 c, const char* fmt, ...) {
        TextLine t{c, ""};
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(t.text, sizeof(t.text), fmt, ap);
        va_end(ap);
        lines.push_back(t);
    };
    if (s.det_factors)
    {
        const PlayerVisibility& pv = fd.pv;
        if (!pv.ok)
            line(cD, "Visibility factors unavailable (see log)");
        else
        {
            line(cT, "You: visibility %.2f = light %.2f%s x stance %.1f x speed %.2f x cover %.2f", pv.visibility,
                 fmaxf(pv.light, pv.flash), pv.flash > pv.light ? " (flash)" : "", pv.stance,
                 (pv.speed > pv.speed_min ? pv.speed * pv.speed_k : 0.f) + pv.speed_base, pv.env);
            line(cD, "  %.1f m/s, %s%s", pv.speed, pv.crouched ? "crouched" : "standing",
                 pv.env_mode == 1 ? (pv.cover > 0.01f ? ", in vegetation" : ", no vegetation here")
                 : pv.env_mode == 2 ? ", vegetation unknown (estimate)"
                                    : "");
        }
        const Npc* top = (!rows.empty() && rows[0]->n.vis.ok && rows[0]->n.vis.value > 0.f) ? &rows[0]->n : nullptr;
        if (!top)  // nobody gaining: the hostile NPC that would see you fastest
        {
            float best = 0.f;
            for (const Npc& n : fd.npcs)
                if (n.health > 0.f && n.vis.ok && n.rel == Relation::Hostile && n.dist <= s.det_list_dist)
                {
                    float r = vision_predict(n, fd.pv, fd.player_pos).per_sec / n.vis.seen;
                    if (r > best || (!top && r >= best))
                    {
                        best = r;
                        top = &n;
                    }
                }
        }
        if (top)
        {
            const Vision& v = top->vis;
            const VisionPrediction p = vision_predict(*top, fd.pv, fd.player_pos);
            line(cT, "%s  %.1f m, %.0f deg off its facing: %s", top->display, top->dist, p.angle * 57.2958f,
                 p.in_force  ? "INSTANT zone"
                 : p.in_cone ? "in its view cone"
                 : p.in_back ? "in its all-round zone"
                             : "outside its vision");
            line(cD, "  range here %.1f m, distance term %.2f, light term %.2f, speed x%.2f", p.r_eff, p.falloff,
                 p.light_term, p.speed_mult);
            line(cT, "  predicted %+.1f %%/s while it sees you, observed %+.1f %%/s", 100.f * p.per_sec / v.seen,
                 100.f * v.rate / v.seen);
            line(cD, "  levels: notices %.0f%%, light alert %.0f%%, spotted 100%% (%s)", 100.f * v.noticed / v.seen,
                 100.f * v.light / v.seen, v.seen < 5.f ? "combat set" : v.surv ? "surv set" : "live");
        }
        else if (fd.detect_ok)
            line(cD, "No hostile NPC with vision data within %.0f m", s.det_list_dist);
        else
            line(cD, "Exact detection unavailable (see log): estimates only");
    }
    const int shown = list ? (rows.empty() ? 1 : (static_cast<int>(rows.size()) < s.det_list_rows
                                                       ? static_cast<int>(rows.size())
                                                       : s.det_list_rows))
                           : 0;
    float W = fs * 24.f;
    for (const TextLine& l : lines)
        W = fmaxf(W, ImGui::CalcTextSize(l.text).x + 2 * pad);
    const float H = (list ? row * (shown + 1) : 0.f) + (lines.empty() ? 0.f : fs * 1.25f * lines.size() + pad) +
                    pad * 2;
    const float x0 = w - W - fs, ys[3] = {fs * 3.f, h * 0.38f, h - H - fs * 4.f};
    const float y0 = ys[s.det_list_pos < 0 || s.det_list_pos > 2 ? 0 : s.det_list_pos];
    fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + W, y0 + H), IM_COL32(12, 12, 12, 175), pad);
    float y = y0 + pad;
    if (list)
    {
        fg->AddText(ImVec2(x0 + pad, y), cT, fd.detect_ok ? "Who is detecting you" : "Who is detecting you (estimates)");
        y += row;
        if (rows.empty())
        {
            fg->AddText(ImVec2(x0 + pad, y), cD, "nobody");
            y += row;
        }
        for (int i = 0; i < shown && i < static_cast<int>(rows.size()); ++i)
        {
            const View& v = *rows[i];
            const Npc& n = v.n;
            char status[96], pct[16], txt[192];
            ImU32 sc = threat_color(v);
            if (n.vis.ok)
                snprintf(pct, sizeof(pct), "%3.0f%%", v.s.pct);
            else
                snprintf(pct, sizeof(pct), "~%.0f%%", v.s.pct);
            if (n.targets_player)
                snprintf(status, sizeof(status), "SPOTTED - %s", mental_state_name(n.mental));
            else if (v.s.stage == Stage::Spotted)
                snprintf(status, sizeof(status), "spotted - %s", mental_state_name(n.mental));
            else if (n.vis.ok && v.s.stage != Stage::None)
                snprintf(status, sizeof(status), "%s %+.0f%%/s",
                         v.s.trend_dir > 0 ? "sees you" : v.s.trend_dir < 0 ? "losing sight" : "holding", v.s.trend);
            else if (alerted_not_by_sight(v))
                snprintf(status, sizeof(status), "%s - can't see you", mental_state_name(n.mental));
            else if (n.vis.ok)
                snprintf(status, sizeof(status), "%s - lost sight", mental_state_name(n.mental));
            else
                snprintf(status, sizeof(status), "%s (AI state estimate)", mental_state_name(n.mental));
            const float bx = x0 + pad, bw = fs * 4.f, by = y + row * 0.3f;
            const float f = v.s.pct / 100.f;
            fg->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + row * 0.4f), IM_COL32(60, 60, 60, 220));
            fg->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * f, by + row * 0.4f),
                              n.vis.ok ? fill_color(v) : fade(sc, 0.55f));
            snprintf(txt, sizeof(txt), "%s  %s  %.0f m  %s", pct, n.display, n.dist, status);
            fg->AddText(ImVec2(bx + bw + pad, y), sc, txt);
            y += row;
        }
    }
    if (!lines.empty())
    {
        y += pad * 0.5f;
        fg->AddLine(ImVec2(x0 + pad, y), ImVec2(x0 + W - pad, y), IM_COL32(90, 90, 90, 255));
        y += pad * 0.5f;
        for (const TextLine& l : lines)
        {
            fg->AddText(ImVec2(x0 + pad, y), l.col, l.text);
            y += fs * 1.25f;
        }
    }
}

struct AlertState
{
    bool armed = true;    // the level was below threshold - 10 points (re-arm band), so a crossing may alert again
    bool targets = false;
    ULONGLONG last_alert = 0;
};
struct Alert
{
    uint16_t id;
    char text[128];
    ImU32 col;
    float pos[3];
    ULONGLONG until;
};
std::unordered_map<uint16_t, AlertState> g_alert_state;
std::vector<Alert> g_alerts;
char g_alert_map[64] = "";
ULONGLONG g_last_beep = 0;

// Alerts fire on the displayed level (held, re/STRUCTS.md §6.6) crossing the threshold, and re-arm only after it fell
// 10 points below it: a value hovering at the threshold alerts once, not every 4 s.
void update_alerts(const FrameData& fd, const std::vector<View>& views)
{
    const Settings& s = g_settings;
    const ULONGLONG now = GetTickCount64();
    if (strcmp(g_alert_map, fd.map) != 0)  // new level: forget everything
    {
        strncpy_s(g_alert_map, fd.map, _TRUNCATE);
        g_alert_state.clear();
        g_alerts.clear();
    }
    g_alerts.erase(std::remove_if(g_alerts.begin(), g_alerts.end(), [now](const Alert& a) { return a.until <= now; }),
                   g_alerts.end());
    const float thr = s.alert_threshold;
    for (const View& v : views)
    {
        const Npc& n = v.n;
        const float level = v.s.pct;
        auto it = g_alert_state.find(n.id);
        if (it == g_alert_state.end())  // first sight (overlay just loaded, new level, spawn): baseline only, no alert
        {
            g_alert_state[n.id] = AlertState{level < thr - 10.f, n.targets_player, 0};
            continue;
        }
        AlertState& st = it->second;
        bool rose = st.armed && level >= thr;
        bool target = s.alert_on_target && n.targets_player && !st.targets;
        if (rose)
            st.armed = false;
        else if (level < thr - 10.f)
            st.armed = true;
        st.targets = n.targets_player;
        if (!s.alerts || n.health <= 0.f || !n.has_brain || n.dist > s.alert_max_dist || !(rose || target))
            continue;
        if (s.alert_hostile_only && n.rel != Relation::Hostile)
            continue;
        if (now - st.last_alert < 4000)  // one alert per NPC per 4 s
            continue;
        st.last_alert = now;
        Alert a{};
        a.id = n.id;
        if (n.targets_player)
            snprintf(a.text, sizeof(a.text), "%s  %.0f m  -  SPOTTED you", n.display, n.dist);
        else if (n.vis.ok)
            snprintf(a.text, sizeof(a.text), "%s  %.0f m  -  %.0f%% %s", n.display, n.dist, level,
                     v.s.trend_dir > 0 ? "and rising" : mental_state_name(n.mental));
        else
            snprintf(a.text, sizeof(a.text), "%s  %.0f m  -  ~%.0f%% (%s)", n.display, n.dist, level,
                     mental_state_name(n.mental));
        a.col = threat_color(v);
        memcpy(a.pos, n.pos, sizeof(a.pos));
        a.until = now + 4000;
        g_alerts.erase(std::remove_if(g_alerts.begin(), g_alerts.end(), [&](const Alert& x) { return x.id == a.id; }),
                       g_alerts.end());
        g_alerts.insert(g_alerts.begin(), a);
        if (g_alerts.size() > 4)
            g_alerts.resize(4);
        if (s.alert_sound && now - g_last_beep > 1500)
        {
            MessageBeep(MB_ICONEXCLAMATION);  // asynchronous system sound
            g_last_beep = now;
        }
    }
}

void draw_alerts(const FrameData& fd, float w, float h)
{
    if (g_alerts.empty())
        return;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float fs = ImGui::GetFontSize(), pad = fs * 0.4f, row = fs * 1.5f;
    const float ys[3] = {h * 0.15f, h * 0.40f, h * 0.72f};
    float y = ys[g_settings.alert_pos < 0 || g_settings.alert_pos > 2 ? 0 : g_settings.alert_pos];
    // camera basis on the ground plane (left-handed, Y up: right = up x forward)
    float cfx = fd.cam_fwd[0], cfz = fd.cam_fwd[2], cl = sqrtf(cfx * cfx + cfz * cfz);
    if (cl > 1e-4f)
    {
        cfx /= cl;
        cfz /= cl;
    }
    for (const Alert& a : g_alerts)
    {
        const float* p = a.pos;
        for (const Npc& n : fd.npcs)  // follow the NPC while the alert is shown
            if (n.id == a.id)
                p = n.pos;
        float dx = p[0] - fd.cam_pos[0], dz = p[2] - fd.cam_pos[2];
        float ahead = dx * cfx + dz * cfz, side = dx * cfz - dz * cfx;  // side > 0: to the right
        float ang = atan2f(side, ahead);
        ImVec2 tz = ImGui::CalcTextSize(a.text);
        float bw = tz.x + row + pad * 3;
        ImVec2 b0(w * 0.5f - bw * 0.5f, y), b1(w * 0.5f + bw * 0.5f, y + row);
        fg->AddRectFilled(b0, b1, IM_COL32(15, 15, 15, 190), pad);
        fg->AddRect(b0, b1, a.col, pad, 0, 1.5f);
        // direction arrow (up = in front of you)
        ImVec2 c(b0.x + pad + row * 0.5f, y + row * 0.5f);
        float s = row * 0.36f, ux = sinf(ang), uy = -cosf(ang);
        ImVec2 tip(c.x + ux * s, c.y + uy * s), l(c.x - ux * s * 0.6f - uy * s * 0.7f, c.y - uy * s * 0.6f + ux * s * 0.7f),
            r(c.x - ux * s * 0.6f + uy * s * 0.7f, c.y - uy * s * 0.6f - ux * s * 0.7f);
        fg->AddTriangleFilled(tip, l, r, a.col);
        fg->AddText(ImVec2(b0.x + row + pad * 2, y + (row - tz.y) * 0.5f), a.col, a.text);
        y += row + pad;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Level of detail: which NPCs get full info, cones and edge arrows.

// 0 up to 10 m, 1 at the max distance.
float dist_t(float d)
{
    const float near_d = 10.f, far_d = fmaxf(g_settings.max_dist, near_d + 1.f);
    const float t = (d - near_d) / (far_d - near_d);
    return t < 0.f ? 0.f : t > 1.f ? 1.f : t;
}

float dist_alpha(float d) { return g_settings.dist_fade ? 1.f - dist_t(d) * (1.f - g_settings.fade_min_alpha) : 1.f; }

// The last 5 m before the max distance fade out, so an NPC walking across that line doesn't pop.
float edge_alpha(float d)
{
    const float e = (g_settings.max_dist - d) / 5.f;
    return e < 0.f ? 0.f : e > 1.f ? 1.f : e;
}

constexpr double kAimHold = 0.4;  // the crosshair NPC keeps its focus this long after the crosshair left its box
uint16_t g_aim_id = 0;
double g_aim_until = -1.0;
std::unordered_map<uint16_t, Detail> g_detail;  // last level of detail (by NPC id), for the distance band

// The NPC under the crosshair: the nearest one whose box (plus a small margin) holds the screen centre; held for
// kAimHold after the crosshair left it, unless another NPC is under it.
const View* pick_crosshair(const std::vector<Marker>& ms, float w, float h, double now)
{
    const float cx = w * 0.5f, cy = h * 0.5f, pad = fmaxf(8.f, h * 0.012f);
    for (const Marker& m : ms)  // distance order
        if (m.v->n.health > 0.f && cx >= m.a.x - pad && cx <= m.b.x + pad && cy >= m.a.y - pad && cy <= m.b.y + pad)
        {
            g_aim_id = m.v->n.id;
            g_aim_until = now + kAimHold;
            return m.v;
        }
    if (now < g_aim_until)
        for (const Marker& m : ms)
            if (m.v->n.id == g_aim_id && m.v->n.health > 0.f)
                return m.v;
    return nullptr;
}

void choose_detail(Marker& m, const View* aimed)
{
    const Settings& s = g_settings;
    const View& v = *m.v;
    const Npc& n = v.n;
    m.dead = n.health <= 0.f;
    m.focus = !m.dead && (v.s.threat || &v == aimed);
    Detail far_detail = Detail::Dot, near_detail = m.dead ? Detail::Compact : static_cast<Detail>(s.calm_detail);
    if (!s.smart_detail || m.focus)
        m.detail = Detail::Full;
    else
    {
        // 2 m band around the detail distance: an NPC pacing on the line keeps its current look.
        auto it = g_detail.find(n.id);
        const bool was_near = it != g_detail.end() && it->second != far_detail;
        const float edge = s.detail_dist + (was_near ? 2.f : -2.f);
        m.detail = n.dist > edge ? far_detail : near_detail;
    }
    g_detail[n.id] = m.detail;
    const float t = s.dist_fade && !m.focus ? dist_t(n.dist) : 0.f;
    m.alpha = (1.f - t * (1.f - s.fade_min_alpha)) * v.k * edge_alpha(n.dist);
    m.shrink = 1.f - 0.3f * t;
}

bool wants_cone(const View& v, const View* aimed)
{
    const Settings& s = g_settings;
    const Npc& n = v.n;
    if (n.health <= 0.f || !n.has_brain || n.dist > s.cone_max_dist)
        return false;
    if (&v == aimed || v.s.threat || s.cones_mode == 2)
        return true;
    return s.cones_mode == 1 && n.rel == Relation::Hostile;
}

bool wants_arrow(const View& v)
{
    const Settings& s = g_settings;
    const Npc& n = v.n;
    if (!s.show_arrows)
        return false;
    if (v.s.threat || s.arrows_mode == 2)
        return true;
    return s.arrows_mode == 1 && n.rel == Relation::Hostile && n.health > 0.f && n.dist <= s.arrow_max_dist;
}

// Small diamond at the upper body (calm NPCs far away). Behind a wall in a dashed style: outline only.
void draw_dot(ImDrawList* dl, const Marker& m, float px, bool hollow)
{
    const float r = fmaxf(3.f, 5.f * px * m.shrink);
    const ImVec2 c(m.fx, m.a.y + (m.b.y - m.a.y) * 0.2f);
    const ImVec2 p0(c.x, c.y - r), p1(c.x + r, c.y), p2(c.x, c.y + r), p3(c.x - r, c.y);
    const ImU32 ol = fade(kOutline, ((m.col >> 24) & 0xFF) / 255.f);
    if (hollow)
    {
        dl->AddQuad(p0, p1, p2, p3, ol, 3.f);
        dl->AddQuad(p0, p1, p2, p3, m.col, 1.5f);
        return;
    }
    dl->AddQuadFilled(p0, p1, p2, p3, m.col);
    dl->AddQuad(p0, p1, p2, p3, ol, 1.f);
}

// "!" / "?" above the marker, on the detection ramp (displayed icon: held, re/STRUCTS.md §6.6).
void draw_state_icon(ImDrawList* dl, const Marker& m, float fs, float top)
{
    const char ic = m.v->s.icon;
    if (!g_settings.state_icons || !ic)
        return;
    const char t[2] = {ic, 0};
    const float size = fs * 1.35f;
    const ImVec2 tz = text_size(size, t);
    const ImU32 col = ic == '!' ? kTargetsYou : threat_color(*m.v);
    text_outlined(dl, size, ImVec2(m.fx - tz.x * 0.5f, top - tz.y * 0.9f), fade(col, m.alpha), t);
}

// One marker at its level of detail; the labels are only built here (placed later, focus first).
void draw_marker(ImDrawList* dl, Marker& m, float px, float fs)
{
    const Settings& s = g_settings;
    const View& v = *m.v;
    const Npc& n = v.n;
    m.col = m.dead ? s.col_dead : relation_color(n.rel);
    const bool hidden = los_hidden(v);
    if (hidden && s.los_style != 1)
        m.col = fade(m.col, s.los_alpha);
    m.col = fade(m.col, m.alpha);
    const bool dashed = hidden && s.los_style != 0;
    if (m.detail == Detail::Dot)
    {
        draw_dot(dl, m, px, dashed);
        return;
    }
    const bool full = m.detail == Detail::Full;
    const bool spotted = !m.dead && v.s.stage == Stage::Spotted;
    const float t = s.box_thickness * px * m.shrink * (full ? 1.f : 0.8f) * (spotted ? 1.8f : 1.f);
    draw_box(dl, m.a, m.b, m.col, t, dashed);
    const bool damaged = n.max_health > 0.f && n.health < n.max_health * 0.995f;
    const bool hb = s.show_health && !m.dead &&
                    (s.health_when == 0 || (s.health_when == 1 && (damaged || m.focus)) || (s.health_when == 2 && m.focus));
    float below = hb ? draw_health(dl, m, px, fs) : 0.f;
    below += draw_awareness(dl, m, m.b.y + below, px, fs);
    m.label_top = m.b.y + below + 2.f * px;
    build_labels(m, fs);
    if (full)
    {
        float top = m.a.y - px;
        if (hb && s.hb_pos == 0)
            top -= 2.f * px + fmaxf(3.f, 4.f * px);
        draw_state_icon(dl, m, fs, top);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Per-NPC tracks: engine data -> views (display state, presence). Rules: re/STRUCTS.md §6.6.

struct Track
{
    DisplayTrack d;
    Npc last;            // last known engine data
    double first = 0.0;  // when it appeared (fade-in)
    double seen = 0.0;   // last frame it was in the NPC list
    uint64_t frame = 0;
    bool ghost = false;  // missing from the list, fading out
    // last frame's raw (undelayed) and shown values, for the flicker counters
    Stage raw_stage = Stage::None, stage = Stage::None;
    bool raw_threat = false, threat = false;
    char raw_icon = 0, icon = 0;
    int8_t raw_los = -1, los = -1;
    // routes
    float speed = 0.f;          // m/s over game time, smoothed
    uint32_t speed_gt = 0;      // game time of the last speed sample
    float speed_pos[3]{};
    Route old_route;            // the previous route after a change of plan, fading out
    double old_since = -1.0;
};

// Flicker counters, logged every 30 s: how often each shown quantity changed with the raw engine values (what the
// previous build drew) and as displayed now, plus NPC drop-outs bridged by the fade-out.
struct FlickerStats
{
    int frames = 0, raw_stage = 0, stage = 0, raw_threat = 0, threat = 0, raw_icon = 0, icon = 0, raw_los = 0, los = 0;
    int bridged = 0, removed = 0, appeared = 0;
    double since = -1.0;
} g_flicker;

void flicker_log(double now)
{
    FlickerStats& f = g_flicker;
    if (f.since < 0.0)
        f.since = now;
    if (now - f.since < 30.0)
        return;
    if (f.raw_stage + f.raw_threat + f.raw_icon + f.raw_los + f.bridged + f.removed + f.appeared)
        logf("flicker (%.0f s, %d frames): stage %d -> %d, full detail %d -> %d, icon %d -> %d, line of sight %d -> %d "
             "(raw -> shown changes); drop-outs bridged %d, NPCs gone %d, appeared %d",
             now - f.since, f.frames, f.raw_stage, f.stage, f.raw_threat, f.threat, f.raw_icon, f.icon, f.raw_los, f.los,
             f.bridged, f.removed, f.appeared);
    f = FlickerStats{};
    f.since = now;
}
std::unordered_map<uint16_t, Track> g_tracks;
std::unordered_map<uint16_t, float> g_sticky_dist;  // label order: distance, updated only on a change > 1.5 m
char g_tracks_map[64] = "";
double g_tracks_start = -1.0;
uint64_t g_frame = 0;

constexpr double kFadeIn = 0.15;     // a new NPC fades in over this time
constexpr double kGhost = 0.4;       // an NPC missing from the list fades out over this time (bridges short drop-outs)
constexpr double kStartGrace = 1.0;  // NPCs present in the first second on a map appear without fading in

void forget_npc(uint16_t id)
{
    g_label_dy.erase(id);
    g_row_key.erase(id);
    g_detail.erase(id);
    g_sticky_dist.erase(id);
}

uint32_t g_tracks_gt = 0;  // game time of the last update

// Call only with fd.ok. Starts over on a new level, after a load (ui_draw drops the tracks while the reader has no
// player) and when the game time runs backwards (a quick-load): the held states belong to the old world.
void update_views(const FrameData& fd, std::vector<View>& views, double now)
{
    ++g_frame;
    const bool time_back = fd.game_time && g_tracks_gt && fd.game_time + 1000 < g_tracks_gt;
    g_tracks_gt = fd.game_time;
    if (strcmp(g_tracks_map, fd.map) != 0 || g_tracks_start < 0.0 || time_back)
    {
        strncpy_s(g_tracks_map, fd.map, _TRUNCATE);
        g_tracks.clear();
        g_label_dy.clear();
        g_row_key.clear();
        g_detail.clear();
        g_sticky_dist.clear();
        g_tracks_start = now;
    }
    views.clear();
    for (const Npc& n : fd.npcs)
    {
        auto it = g_tracks.find(n.id);
        if (it != g_tracks.end() && (it->second.last.cls != n.cls || strcmp(it->second.last.name, n.name) != 0))
        {
            forget_npc(n.id);  // the slot holds another entity now
            g_tracks.erase(it);
            it = g_tracks.end();
        }
        const bool fresh = it == g_tracks.end();
        if (fresh)
        {
            it = g_tracks.emplace(n.id, Track{}).first;
            it->second.first = now - g_tracks_start < kStartGrace ? now - kFadeIn : now;
            g_flicker.appeared += now - g_tracks_start >= kStartGrace;
        }
        Track& tr = it->second;
        if (tr.ghost)
        {
            ++g_flicker.bridged;  // back within the fade-out time: the previous build blinked here
            tr.ghost = false;
        }
        // A change of plan (the destination moved more than 1.5 m, or the route ended) keeps the old route, fading.
        const Route& was = tr.last.route;
        if (!fresh && was.moving && was.n > 0)
        {
            const float* a = was.pts[was.n - 1];
            bool changed = !n.route.moving;
            if (!changed)
            {
                const float* b = n.route.pts[n.route.n - 1];
                const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
                changed = dx * dx + dy * dy + dz * dz > 1.5f * 1.5f;
            }
            if (changed)
            {
                tr.old_route = was;
                tr.old_since = now;
            }
        }
        // Speed over game time (sampled every >= 50 ms of game time; 1 s smoothing): only for the arrival estimate.
        const uint32_t gt = fd.game_time;
        if (!tr.speed_gt || gt < tr.speed_gt || gt - tr.speed_gt > 1000)
        {
            tr.speed_gt = gt;
            memcpy(tr.speed_pos, n.pos, sizeof(tr.speed_pos));
        }
        else if (gt - tr.speed_gt >= 50)
        {
            const float dt = (gt - tr.speed_gt) / 1000.f;
            const float dx = n.pos[0] - tr.speed_pos[0], dy = n.pos[1] - tr.speed_pos[1], dz = n.pos[2] - tr.speed_pos[2];
            tr.speed += (sqrtf(dx * dx + dy * dy + dz * dz) / dt - tr.speed) * fminf(1.f, dt / 1.f);
            tr.speed_gt = gt;
            memcpy(tr.speed_pos, n.pos, sizeof(tr.speed_pos));
        }
        tr.last = n;
        tr.seen = now;
        tr.frame = g_frame;
        display_update(tr.d, n, now);
        const Shown& sh = tr.d.s;
        const bool rt = raw_threat(n, sh.raw_stage);
        const char ri = raw_icon(n, sh.raw_stage);
        if (!fresh)
        {
            g_flicker.raw_stage += sh.raw_stage != tr.raw_stage;
            g_flicker.stage += sh.stage != tr.stage;
            g_flicker.raw_threat += rt != tr.raw_threat;
            g_flicker.threat += sh.threat != tr.threat;
            g_flicker.raw_icon += ri != tr.raw_icon;
            g_flicker.icon += sh.icon != tr.icon;
            g_flicker.raw_los += n.los != tr.raw_los;
            g_flicker.los += sh.los != tr.los;
        }
        tr.raw_stage = sh.raw_stage;
        tr.stage = sh.stage;
        tr.raw_threat = rt;
        tr.threat = sh.threat;
        tr.raw_icon = ri;
        tr.icon = sh.icon;
        tr.raw_los = n.los;
        tr.los = sh.los;
        View v;
        v.n = n;
        v.s = tr.d.s;
        v.k = static_cast<float>(fmin(1.0, (now - tr.first) / kFadeIn));
        v.speed = tr.speed;
        v.tr = &tr;  // stable: unordered_map never moves its elements
        views.push_back(v);
    }
    for (auto it = g_tracks.begin(); it != g_tracks.end();)  // NPCs that left the list this frame or recently
    {
        Track& tr = it->second;
        if (tr.frame == g_frame)
        {
            ++it;
            continue;
        }
        const double gone = now - tr.seen;
        if (gone >= kGhost || tr.last.health <= 0.f)
        {
            ++g_flicker.removed;
            forget_npc(it->first);
            it = g_tracks.erase(it);
            continue;
        }
        tr.ghost = true;
        View v;
        v.n = tr.last;
        v.s = tr.d.s;
        v.k = static_cast<float>(1.0 - gone / kGhost);
        v.speed = tr.speed;
        v.tr = &tr;
        views.push_back(v);
        ++it;
    }
    ++g_flicker.frames;
    flicker_log(now);
}

// ---------------------------------------------------------------------------------------------------------------
// Routes and intentions (re/STRUCTS.md §7): the motor's path, the stop point and the facing there, what it will do.

bool project(const FrameData& fd, const float p[3], ImVec2& out)
{
    const ClipPt c = to_clip(fd.vp, ImVec4(p[0], p[1], p[2], 0.f));
    if (c.w < kNearCut)
        return false;
    out = clip_to_screen(c);
    return true;
}

void world_line(ImDrawList* dl, const FrameData& fd, const float a[3], const float b[3], ImU32 col, float t)
{
    ClipPt ca = to_clip(fd.vp, ImVec4(a[0], a[1], a[2], 0.f)), cb = to_clip(fd.vp, ImVec4(b[0], b[1], b[2], 0.f));
    if (clip_near(ca, cb))
        dl->AddLine(clip_to_screen(ca), clip_to_screen(cb), col, t);
}

// Small pictogram for an intent, centred at c (size = half width).
void draw_intent_icon(ImDrawList* dl, Intent i, ImVec2 c, float s, ImU32 col)
{
    const float t = fmaxf(1.5f, s * 0.22f);
    dl->AddCircleFilled(c, s * 1.35f, fade(IM_COL32(15, 15, 15, 255), ((col >> 24) & 0xFF) / 255.f * 0.75f));
    switch (i)
    {
    case Intent::Patrol:  // flag
        dl->AddLine(ImVec2(c.x - s * 0.5f, c.y + s), ImVec2(c.x - s * 0.5f, c.y - s), col, t);
        dl->AddTriangleFilled(ImVec2(c.x - s * 0.5f, c.y - s), ImVec2(c.x + s * 0.8f, c.y - s * 0.55f),
                              ImVec2(c.x - s * 0.5f, c.y - s * 0.1f), col);
        break;
    case Intent::Cover:  // shield
    {
        const ImVec2 p[5] = {{c.x - s * 0.8f, c.y - s * 0.8f}, {c.x + s * 0.8f, c.y - s * 0.8f}, {c.x + s * 0.8f, c.y},
                             {c.x, c.y + s}, {c.x - s * 0.8f, c.y}};
        dl->AddConvexPolyFilled(p, 5, col);
        break;
    }
    case Intent::Search:  // magnifier
        dl->AddCircle(ImVec2(c.x - s * 0.2f, c.y - s * 0.2f), s * 0.55f, col, 0, t);
        dl->AddLine(ImVec2(c.x + s * 0.2f, c.y + s * 0.2f), ImVec2(c.x + s * 0.85f, c.y + s * 0.85f), col, t * 1.3f);
        break;
    case Intent::Body:  // cross
        dl->AddLine(ImVec2(c.x - s * 0.7f, c.y - s * 0.7f), ImVec2(c.x + s * 0.7f, c.y + s * 0.7f), col, t * 1.2f);
        dl->AddLine(ImVec2(c.x + s * 0.7f, c.y - s * 0.7f), ImVec2(c.x - s * 0.7f, c.y + s * 0.7f), col, t * 1.2f);
        break;
    case Intent::Attack:  // crosshair
        dl->AddCircle(c, s * 0.6f, col, 0, t);
        dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x - s * 0.3f, c.y), col, t);
        dl->AddLine(ImVec2(c.x + s * 0.3f, c.y), ImVec2(c.x + s, c.y), col, t);
        dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y - s * 0.3f), col, t);
        dl->AddLine(ImVec2(c.x, c.y + s * 0.3f), ImVec2(c.x, c.y + s), col, t);
        break;
    case Intent::Retreat:  // double chevron back
        for (float o : {-0.35f, 0.35f})
        {
            dl->AddLine(ImVec2(c.x + s * (o + 0.3f), c.y - s * 0.7f), ImVec2(c.x + s * (o - 0.3f), c.y), col, t);
            dl->AddLine(ImVec2(c.x + s * (o - 0.3f), c.y), ImVec2(c.x + s * (o + 0.3f), c.y + s * 0.7f), col, t);
        }
        break;
    case Intent::Follow:  // two dots in a row
        dl->AddCircleFilled(ImVec2(c.x - s * 0.45f, c.y), s * 0.35f, col);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.45f, c.y), s * 0.35f, col);
        break;
    case Intent::Wait:  // pause bars
        dl->AddRectFilled(ImVec2(c.x - s * 0.55f, c.y - s * 0.7f), ImVec2(c.x - s * 0.15f, c.y + s * 0.7f), col);
        dl->AddRectFilled(ImVec2(c.x + s * 0.15f, c.y - s * 0.7f), ImVec2(c.x + s * 0.55f, c.y + s * 0.7f), col);
        break;
    default:  // investigate "?", action "*", idle "z", scripted "S"
    {
        const char* g = i == Intent::Investigate ? "?" : i == Intent::Action ? "*" : i == Intent::Idle ? "z" : "S";
        const float fsz = s * 2.1f;
        const ImVec2 tz = text_size(fsz, g);
        text_outlined(dl, fsz, ImVec2(c.x - tz.x * 0.5f, c.y - tz.y * 0.5f), col, g);
        break;
    }
    }
}

// The route as a smooth ground line through the engine's points (Catmull-Rom, 4 steps per segment), cut at the near
// plane. Returns the path length along the engine's points from `start`. With `details`: chevrons in the walking
// direction every 2.5 m, dots at real corners, the stop ring with an arrow into it and the facing there.
float draw_route_line(ImDrawList* dl, const FrameData& fd, const float start[3], const Route& r, ImU32 col, float px,
                      bool details)
{
    static std::vector<ImVec4> c, sm;
    c.clear();
    sm.clear();
    const float lift = 0.08f;
    c.push_back(ImVec4(start[0], start[1] + lift, start[2], 0.f));
    float length = 0.f;
    for (int i = 0; i < r.n; ++i)
    {
        const ImVec4 p(r.pts[i][0], r.pts[i][1] + lift, r.pts[i][2], 0.f);
        const ImVec4& q = c.back();
        const float d = sqrtf((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
        length += d;
        if (d > 0.15f || i == r.n - 1)
            c.push_back(p);
    }
    if (c.size() < 2)
        return length;
    for (size_t i = 0; i + 1 < c.size(); ++i)
    {
        const ImVec4 &p0 = c[i == 0 ? 0 : i - 1], &p1 = c[i], &p2 = c[i + 1], &p3 = c[i + 2 < c.size() ? i + 2 : i + 1];
        for (int k = 0; k < 4; ++k)
        {
            const float t = k / 4.f, t2 = t * t, t3 = t2 * t;
            auto cr = [&](float a, float b, float cc, float d) {
                return 0.5f * (2.f * b + (-a + cc) * t + (2.f * a - 5.f * b + 4.f * cc - d) * t2 + (-a + 3.f * b - 3.f * cc + d) * t3);
            };
            sm.push_back(ImVec4(cr(p0.x, p1.x, p2.x, p3.x), cr(p0.y, p1.y, p2.y, p3.y), cr(p0.z, p1.z, p2.z, p3.z), 0.f));
        }
    }
    sm.push_back(c.back());
    const float a = ((col >> 24) & 0xFF) / 255.f;
    const float thick = (details ? 2.2f : 1.4f) * px;
    const ImU32 ol = fade(kOutline, a * 0.6f);
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i + 1 < sm.size(); ++i)
        {
            const float pa[3] = {sm[i].x, sm[i].y, sm[i].z}, pb[3] = {sm[i + 1].x, sm[i + 1].y, sm[i + 1].z};
            world_line(dl, fd, pa, pb, pass == 0 ? ol : col, pass == 0 ? thick + 2.f : thick);
        }
    if (!details)
        return length;
    // chevrons along the smoothed line
    float acc = 0.f, next = 1.5f;
    for (size_t i = 0; i + 1 < sm.size(); ++i)
    {
        const ImVec4 &p = sm[i], &q = sm[i + 1];
        const float seg = sqrtf((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.z));
        while (seg > 1e-3f && next <= acc + seg && next < length - 0.8f)
        {
            const float u = (next - acc) / seg;
            const float at[3] = {p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u, p.z + (q.z - p.z) * u};
            const float ahead[3] = {at[0] + (q.x - p.x) / seg * 0.3f, at[1] + (q.y - p.y) / seg * 0.3f, at[2] + (q.z - p.z) / seg * 0.3f};
            ImVec2 s0, s1;
            if (project(fd, at, s0) && project(fd, ahead, s1))
            {
                float dx = s1.x - s0.x, dy = s1.y - s0.y, l = sqrtf(dx * dx + dy * dy);
                if (l > 0.5f)
                {
                    dx /= l;
                    dy /= l;
                    const float k = 5.f * px;
                    const ImVec2 tip(s0.x + dx * k, s0.y + dy * k);
                    dl->AddLine(ImVec2(s0.x - dx * k - dy * k, s0.y - dy * k + dx * k), tip, col, thick);
                    dl->AddLine(ImVec2(s0.x - dx * k + dy * k, s0.y - dy * k - dx * k), tip, col, thick);
                }
            }
            next += 2.5f;
        }
        acc += seg;
    }
    // dots at real corners (the engine's points where the direction turns by more than 25 degrees)
    for (size_t i = 1; i + 1 < c.size(); ++i)
    {
        const float ux = c[i].x - c[i - 1].x, uz = c[i].z - c[i - 1].z, vx = c[i + 1].x - c[i].x, vz = c[i + 1].z - c[i].z;
        const float lu = sqrtf(ux * ux + uz * uz), lv = sqrtf(vx * vx + vz * vz);
        if (lu < 1e-3f || lv < 1e-3f || (ux * vx + uz * vz) / (lu * lv) > 0.906f)
            continue;
        const float p[3] = {c[i].x, c[i].y, c[i].z};
        ImVec2 s;
        if (project(fd, p, s))
        {
            dl->AddCircleFilled(s, 2.8f * px, ol);
            dl->AddCircleFilled(s, 2.0f * px, col);
        }
    }
    // stop point: ring on the ground, an arrowhead into it, the facing there
    const ImVec4& e = c.back();
    static std::vector<ImVec4> ring;
    ring.clear();
    for (int i = 0; i < 20; ++i)
        ring.push_back(ImVec4(e.x + cosf(6.2831853f * i / 20) * 0.4f, e.y, e.z + sinf(6.2831853f * i / 20) * 0.4f, 0.f));
    ground_shape(dl, fd, ring, true, fade(col, 0.25f), col, thick, false);
    const ImVec4& before = sm[sm.size() >= 3 ? sm.size() - 3 : 0];
    const float ep[3] = {e.x, e.y, e.z}, bp[3] = {before.x, before.y, before.z};
    ImVec2 se, sb;
    if (project(fd, ep, se) && project(fd, bp, sb))
    {
        float dx = se.x - sb.x, dy = se.y - sb.y, l = sqrtf(dx * dx + dy * dy);
        if (l > 1.f)
        {
            dx /= l;
            dy /= l;
            const float k = 7.f * px;
            const ImVec2 tip(se.x - dx * 2.f * px, se.y - dy * 2.f * px), base(tip.x - dx * k * 1.6f, tip.y - dy * k * 1.6f);
            dl->AddTriangleFilled(tip, ImVec2(base.x - dy * k, base.y + dx * k), ImVec2(base.x + dy * k, base.y - dx * k), col);
        }
    }
    if (r.has_dir)
    {
        const float l = sqrtf(r.dir[0] * r.dir[0] + r.dir[2] * r.dir[2]);
        const float tip[3] = {e.x + r.dir[0] / l * 1.3f, e.y, e.z + r.dir[2] / l * 1.3f};
        world_line(dl, fd, ep, tip, ol, thick + 2.f);
        world_line(dl, fd, ep, tip, col, thick);
        ImVec2 st, sf;
        if (project(fd, tip, st) && project(fd, ep, sf))
        {
            float dx = st.x - sf.x, dy = st.y - sf.y, ll = sqrtf(dx * dx + dy * dy);
            if (ll > 1.f)
            {
                dx /= ll;
                dy /= ll;
                const float k = 5.f * px;
                dl->AddTriangleFilled(ImVec2(st.x + dx * k, st.y + dy * k), ImVec2(st.x - dy * k, st.y + dx * k),
                                      ImVec2(st.x + dy * k, st.y - dx * k), col);
            }
        }
    }
    return length;
}

// Routes are shown for the NPC under the crosshair and threats, or every NPC nearby (option), within the route range.
bool route_eligible(const View& v, const View* aimed)
{
    const Settings& s = g_settings;
    return s.show_routes && v.n.health > 0.f && v.n.dist <= s.route_max_dist &&
           (&v == aimed || v.s.threat || s.routes_mode == 1);
}

bool wants_route(const View& v, const View* aimed) { return route_eligible(v, aimed) && v.n.route.moving; }

void draw_route(ImDrawList* dl, const FrameData& fd, const View& v, const View* aimed, float px, float fs)
{
    const bool focus = &v == aimed || v.s.threat;
    const float k = v.k * (focus ? 1.f : dist_alpha(v.n.dist)) * edge_alpha(v.n.dist);
    const ImU32 col = fade(threat_color(v), 0.9f * k);
    if (!route_eligible(v, aimed))
        return;
    // the previous plan fades out over 1 s
    if (v.tr && v.tr->old_since >= 0.0)
    {
        const double age = ImGui::GetTime() - v.tr->old_since;
        if (age < 1.0 && v.tr->old_route.n > 0)
            draw_route_line(dl, fd, v.n.pos, v.tr->old_route, fade(col, static_cast<float>(0.6 * (1.0 - age))), px, false);
    }
    if (!v.n.route.moving)
        return;
    const Route& r = v.n.route;
    const float length = draw_route_line(dl, fd, v.n.pos, r, col, px, true);
    if (!g_settings.route_labels)
        return;
    const float* e = r.pts[r.n - 1];
    const float up[3] = {e[0], e[1] + 0.9f, e[2]};
    ImVec2 at;
    if (!project(fd, up, at))
        return;
    const Intent in = route_intent(r);
    char text[96];
    const char* what = in != Intent::None ? intent_label(in) : "stop";
    // Arrival: path length / speed over the last ~1 s, an estimate ("~"); not shown while it stands or crawls (> 60 s).
    if (v.speed > 0.5f && length / v.speed <= 60.f)
        snprintf(text, sizeof(text), "%s  ~%.0f s", what, length / v.speed);
    else
        snprintf(text, sizeof(text), "%s", what);
    const float ts = fs * 0.85f * g_settings.label_scale, is = ts * 0.42f;
    const ImVec2 tz = text_size(ts, text);
    const float w = is * 2.8f + tz.x;
    const ImVec2 ic(at.x - w * 0.5f + is * 1.35f, at.y);
    draw_intent_icon(dl, in, ic, is, col);
    text_outlined(dl, ts, ImVec2(ic.x + is * 1.6f, at.y - tz.y * 0.5f), col, text);
}

void menu_stealth_tab()
{
    Settings& s = g_settings;
    const float iw = ImGui::GetFontSize() * 9.f;
    ImGui::Checkbox("Detection bar + percent", &s.show_awareness);
    ImGui::Checkbox("\"Who is detecting you\" list", &s.det_list);
    ImGui::SameLine();
    ImGui::Checkbox("only while someone is", &s.det_list_hide_empty);
    ImGui::Checkbox("\"Why\" panel: your visibility factors + the formula", &s.det_factors);
    ImGui::Checkbox("Alert banner when an NPC notices / targets you", &s.alerts);
    ImGui::Checkbox("View cones", &s.show_cones);
    ImGui::SameLine();
    const char* cone_who[] = {"Threats only", "+ hostile NPCs", "Every NPC"};
    ImGui::SetNextItemWidth(iw);
    ImGui::Combo("##conewho", &s.cones_mode, cone_who, 3);
    ImGui::Checkbox("Style NPCs behind walls differently", &s.los_enabled);
    ImGui::TextDisabled("Exact %%: each NPC's own vision value for you against its live \"spotted\" threshold\n"
                        "(100%% = the moment it spots you). Ticks on the bar: notices, light alert. \"~\" = estimate\n"
                        "from the AI state (no vision data). \"Alerted, can't see you\": sound, a body, a comrade's\n"
                        "alarm or lost sight.");
    if (section("Detection list layout"))
    {
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("List max distance (m)", &s.det_list_dist, 5.f, 200.f, "%.0f");
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderInt("List rows", &s.det_list_rows, 1, 20);
        const char* lpos[] = {"Top right", "Middle right", "Bottom right"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("List position", &s.det_list_pos, lpos, 3);
    }
    if (section("Alerts"))
    {
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Detection threshold", &s.alert_threshold, 1.f, 100.f, "%.0f%%");
        ImGui::Checkbox("Alert when an NPC starts targeting you", &s.alert_on_target);
        ImGui::Checkbox("Hostile NPCs only", &s.alert_hostile_only);
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Alert max distance (m)", &s.alert_max_dist, 5.f, 500.f, "%.0f");
        const char* pos[] = {"Top", "Middle", "Bottom"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("Banner position", &s.alert_pos, pos, 3);
        ImGui::Checkbox("Sound", &s.alert_sound);
    }
    if (section("View cones"))
    {
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Cone max distance (m)", &s.cone_max_dist, 5.f, 200.f, "%.0f");
        ImGui::Checkbox("From the NPC's own vision data", &s.cone_real);
        ImGui::Checkbox("All-round zone (back range) + instant zone", &s.cone_back);
        ImGui::Checkbox("Peripheral zone (approx.: the game uses the head)", &s.cone_periph);
        ImGui::TextDisabled("Fallback for NPCs without vision data (or real cones off):");
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Cone length (m)", &s.cone_length, 1.f, 50.f, "%.0f");
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Default FOV (deg)", &s.cone_fov, 10.f, 360.f, "%.0f");
    }
    if (section("Line of sight"))
    {
        const char* los_styles[] = {"Faded", "Dashed", "Faded + dashed"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("Hidden style", &s.los_style, los_styles, 3);
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Hidden opacity", &s.los_alpha, 0.05f, 1.f, "%.2f");
        const char* unk[] = {"Draw as visible", "Draw as hidden"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("No data", &s.los_unknown, unk, 2);
        ImGui::TextDisabled("Data: the AI's own sighting of you (brain), not a raycast of ours.");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Zones (Debug tab)

void draw_zone(ImDrawList* dl, const FrameData& fd, const Zone& z, float w, float h)
{
    ImVec2 sp[8];
    bool ok[8];
    for (int i = 0; i < 8; ++i)
    {
        float l[3] = {(i & 1) ? z.bmax[0] : z.bmin[0], (i & 2) ? z.bmax[1] : z.bmin[1], (i & 4) ? z.bmax[2] : z.bmin[2]};
        float p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = z.xf[12 + k] + l[0] * z.xf[k] + l[1] * z.xf[4 + k] + l[2] * z.xf[8 + k];
        ok[i] = world_to_screen(fd.vp, p, w, h, sp[i].x, sp[i].y);
    }
    const ImU32 col = z.player_inside ? IM_COL32(255, 80, 200, 230)
                      : z.trigger    ? IM_COL32(255, 160, 40, 200)
                                     : IM_COL32(80, 190, 255, 170);
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    int drawn = 0;
    for (auto& e : edges)
        if (ok[e[0]] && ok[e[1]])
        {
            dl->AddLine(sp[e[0]], sp[e[1]], col, z.player_inside ? 2.f : 1.2f);
            ++drawn;
        }
    if (g_settings.zone_labels && drawn)
    {
        float c[3] = {0, 0, 0};
        float l[3] = {(z.bmin[0] + z.bmax[0]) * 0.5f, z.bmax[1], (z.bmin[2] + z.bmax[2]) * 0.5f};
        for (int k = 0; k < 3; ++k)
            c[k] = z.xf[12 + k] + l[0] * z.xf[k] + l[1] * z.xf[4 + k] + l[2] * z.xf[8 + k];
        float sx, sy;
        if (world_to_screen(fd.vp, c, w, h, sx, sy))
        {
            char t[96];
            snprintf(t, sizeof(t), "%s  %.0f m", z.name, z.dist);
            float ts = ImGui::GetFontSize() * 0.8f * g_settings.label_scale;
            ImVec2 tz = text_size(ts, t);
            text_outlined(dl, ts, ImVec2(sx - tz.x * 0.5f, sy - tz.y - 2.f), col | 0xFF000000, t);
        }
    }
}

std::unordered_map<uint16_t, bool> g_zone_inside;

void zone_alerts(const FrameData& fd)
{
    for (const Zone& z : fd.zones)
    {
        bool& was = g_zone_inside[z.id];
        if (z.player_inside && !was && g_settings.zone_alerts)
        {
            char t[96];
            snprintf(t, sizeof(t), "Entered zone: %s", z.name);
            ui_toast(t, 3.f);
        }
        was = z.player_inside;
    }
}

void menu_debug_tab(const FrameData& fd)
{
    Settings& s = g_settings;
    const float iw = ImGui::GetFontSize() * 9.f;
    ImGui::Checkbox("Debug labels (raw name, class, id, friend_type, vision values)", &s.debug_labels);
    ImGui::SeparatorText("Trigger / script zones");
    ImGui::Checkbox("Show zones", &s.show_zones);
    ImGui::SameLine();
    ImGui::Checkbox("Triggers only", &s.zone_triggers_only);
    ImGui::SameLine();
    ImGui::Checkbox("Names", &s.zone_labels);
    ImGui::SetNextItemWidth(iw);
    ImGui::SliderFloat("Zone max distance (m)", &s.zone_max_dist, 5.f, 300.f, "%.0f");
    ImGui::Checkbox("Banner when I enter a shown zone", &s.zone_alerts);
    ImGui::TextDisabled("Boxes are the zones' bounding boxes; orange = trigger, blue = other, pink = you are inside.");
    if (s.show_zones)
    {
        int inside = 0;
        for (const Zone& z : fd.zones)
            inside += z.player_inside;
        ImGui::Text("Zones shown: %d   you are inside: %d", static_cast<int>(fd.zones.size()), inside);
        for (const Zone& z : fd.zones)
            if (z.player_inside)
                ImGui::BulletText("%s", z.name);
    }
}

void menu_markers_tab()
{
    Settings& s = g_settings;
    const float iw = ImGui::GetFontSize() * 9.f;
    ImGui::Checkbox("Smart detail: full info only where it matters", &s.smart_detail);
    ImGui::BeginDisabled(!s.smart_detail);
    const char* calm[] = {"Small marker", "Box + name", "Full"};
    ImGui::SetNextItemWidth(iw);
    ImGui::Combo("Calm NPCs nearby", &s.calm_detail, calm, 3);
    ImGui::SetNextItemWidth(iw);
    ImGui::SliderFloat("Nearby = closer than (m)", &s.detail_dist, 5.f, 200.f, "%.0f");
    ImGui::TextDisabled("Always full: NPCs that detect you, are alerted or in combat (not friendly ones).\n"
                        "Calm NPCs further away: small marker, no text.");
    ImGui::EndDisabled();
    ImGui::Checkbox("Full info for the NPC under the crosshair", &s.focus_crosshair);
    ImGui::Checkbox("Fade and shrink calm NPCs with distance", &s.dist_fade);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(iw * 0.8f);
    ImGui::SliderFloat("opacity far away", &s.fade_min_alpha, 0.1f, 1.f, "%.2f");
    ImGui::Checkbox("\"?\" / \"!\" above NPCs that are alerted / have spotted you", &s.state_icons);
    ImGui::Checkbox("Routes: where NPCs are going and what they will do there", &s.show_routes);
    ImGui::SameLine();
    const char* route_who[] = {"Crosshair NPC + threats", "Every NPC nearby"};
    ImGui::SetNextItemWidth(iw);
    ImGui::Combo("##routewho", &s.routes_mode, route_who, 2);
    if (section("Box and health bar"))
    {
        const char* box_styles[] = {"Full box", "Corners only", "None"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("Box style", &s.box_style, box_styles, 3);
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Thickness", &s.box_thickness, 0.5f, 6.f, "%.1f");
        ImGui::Checkbox("Health bar", &s.show_health);
        ImGui::SameLine();
        const char* when[] = {"Always", "Damaged or in focus", "In focus only"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("##hbwhen", &s.health_when, when, 3);
        const char* hb_pos[] = {"Top", "Bottom", "Left", "Right"};
        const char* hb_style[] = {"Bar", "Bar + percent", "Segmented"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("Position", &s.hb_pos, hb_pos, 4);
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("Style", &s.hb_style, hb_style, 3);
    }
    if (section("Labels"))
    {
        ImGui::Checkbox("Name", &s.lbl_name);
        ImGui::SameLine();
        ImGui::Checkbox("Distance", &s.lbl_distance);
        ImGui::SameLine();
        ImGui::Checkbox("Class", &s.lbl_class);
        ImGui::SameLine();
        ImGui::Checkbox("Health %", &s.lbl_health);
        ImGui::Checkbox("AI state + detection % (full detail)", &s.show_state);
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Label size", &s.label_scale, 0.6f, 2.f, "%.2f");
        ImGui::Checkbox("Move overlapping labels apart", &s.label_declutter);
    }
    if (section("Routes"))
    {
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("Route distance (m)", &s.route_max_dist, 5.f, 200.f, "%.0f");
        ImGui::Checkbox("Label at the stop point: what it will do + time of arrival", &s.route_labels);
        ImGui::TextDisabled("Path, waypoints and stop point: the AI's own route (movement motor). Facing arrow at the\n"
                            "stop: the direction the AI will turn to. What it will do: its current task and script.\n"
                            "\"~\" = estimate (time of arrival = path length / current speed). Colour = AI state.");
    }
    if (section("Off-screen arrows"))
    {
        ImGui::Checkbox("Arrows at the screen edge", &s.show_arrows);
        const char* who[] = {"Threats only", "+ hostile NPCs nearby", "Every NPC in range"};
        ImGui::SetNextItemWidth(iw);
        ImGui::Combo("For", &s.arrows_mode, who, 3);
        ImGui::SetNextItemWidth(iw);
        ImGui::SliderFloat("\"Nearby\" for arrows (m)", &s.arrow_max_dist, 5.f, 500.f, "%.0f");
    }
}

void swatch(ImU32 c, const char* label)
{
    const float sz = ImGui::GetFontSize() * 0.75f;
    ImGui::ColorButton(label, ImGui::ColorConvertU32ToFloat4(c | 0xFF000000),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(sz, sz));
    ImGui::SameLine(0.f, sz * 0.4f);
    ImGui::TextUnformatted(label);
}

void legend()
{
    const Settings& s = g_settings;
    ImGui::PushID("legend");
    ImGui::TextDisabled("Marker colour = relation:");
    ImGui::SameLine();
    swatch(s.col_friendly, "friendly");
    ImGui::SameLine();
    swatch(s.col_neutral, "neutral");
    ImGui::SameLine();
    swatch(s.col_hostile, "hostile");
    ImGui::SameLine();
    swatch(s.col_dead, "dead");
    ImGui::TextDisabled("Detection (bar, %%, cones, alerts):");
    ImGui::SameLine();
    swatch(kCalm, "calm");
    ImGui::SameLine();
    swatch(kSees, "sees you");
    ImGui::SameLine();
    swatch(kNoticed, "noticed / alerted");
    ImGui::SameLine();
    swatch(kLightAlert, "light alert");
    ImGui::SameLine();
    swatch(kTargetsYou, "spotted / combat");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kNoticed), "?");
    ImGui::SameLine();
    ImGui::TextUnformatted("alerted or noticing you   ");
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTargetsYou), "!");
    ImGui::SameLine();
    ImGui::TextUnformatted("spotted you / in combat");
    ImGui::TextDisabled("White bar = health.  Faded / dashed = behind a wall.  Small diamond = calm, far away.");
    ImGui::TextDisabled("Route: line with chevrons = where it walks, dots = corners, ring = where it stops, arrow from\n"
                        "the ring = where it will face, icon + label = what it will do there (\"~\" = estimated time).");
    ImGui::PopID();
}

void menu_main_tab()
{
    Settings& s = g_settings;
    ImGui::SeparatorText("Preset");
    const int cur = config_preset(s);
    const char* names[] = {"Minimal", "Stealth", "Full / Debug"};
    for (int p = 1; p <= 3; ++p)
    {
        if (p > 1)
            ImGui::SameLine();
        if (ImGui::RadioButton(names[p - 1], cur == p))
            config_apply_preset(s, p);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled();
    ImGui::RadioButton("Custom", cur == 0);
    ImGui::EndDisabled();
    static const char* desc[] = {
        "Your own mix: any option changed by hand. Pick a preset to go back.",
        "Small markers. Full info only for NPCs that detect you, are alerted or under the crosshair. No cones.",
        "Full info and view cones for threats and the NPC under the crosshair, names for calm NPCs nearby,\n"
        "arrows for threats and nearby hostiles off screen.",
        "Everything for every NPC, with raw engine values. Busy: for checking and debugging."};
    ImGui::TextDisabled("%s", desc[cur]);
    ImGui::SeparatorText("Show");
    ImGui::Checkbox("Humans", &s.show_human);
    ImGui::SameLine();
    ImGui::Checkbox("Mutants", &s.show_mutant);
    ImGui::SameLine();
    ImGui::Checkbox("Fauna", &s.show_fauna);
    ImGui::SameLine(0.f, ImGui::GetFontSize() * 1.5f);
    const Relation rels[] = {Relation::Friendly, Relation::Neutral, Relation::Hostile};
    bool* flags[] = {&s.show_friendly, &s.show_neutral, &s.show_hostile};
    const char* rel_names[] = {"Friendly", "Neutral", "Hostile"};
    for (int i = 0; i < 3; ++i)
    {
        if (i > 0)
            ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, relation_color(rels[i]));
        ImGui::Checkbox(rel_names[i], flags[i]);
        ImGui::PopStyleColor();
    }
    ImGui::Checkbox("Dead NPCs", &s.show_dead);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.f);
    ImGui::SliderFloat("Max distance (m)", &s.max_dist, 10.f, 500.f, "%.0f");
    ImGui::SeparatorText("Legend");
    legend();
}
}  // namespace

void ui_setup_fonts()
{
    // Segoe UI from the Windows font folder (read only; it also covers Cyrillic). Fallback: ImGui's built-in font.
    wchar_t path[MAX_PATH];
    UINT n = GetWindowsDirectoryW(path, MAX_PATH);
    ImGuiIO& io = ImGui::GetIO();
    if (n && n < MAX_PATH - 32)
    {
        wcscat_s(path, L"\\Fonts\\segoeui.ttf");
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (32 << 20))
            {
                void* data = IM_ALLOC(static_cast<size_t>(sz.QuadPart));  // owned (and freed) by the font atlas
                DWORD got = 0;
                if (ReadFile(f, data, static_cast<DWORD>(sz.QuadPart), &got, nullptr) && got == sz.QuadPart &&
                    io.Fonts->AddFontFromMemoryTTF(data, static_cast<int>(got), 16.0f))
                {
                    CloseHandle(f);
                    logf("font: Segoe UI");
                    return;
                }
                IM_FREE(data);
            }
            CloseHandle(f);
        }
    }
    io.Fonts->AddFontDefault();
    logf("font: ImGui default (Segoe UI not available)");
}

void ui_toast(const char* msg, float seconds)
{
    strncpy_s(g_toast, msg, _TRUNCATE);
    g_toast_until = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.f);
}

bool ui_toast_active() { return GetTickCount64() < g_toast_until; }

void ui_menu_request(int tab, bool open_sections)
{
    g_tab_request = tab;
    g_open_sections = open_sections;
}

void ui_draw(const FrameData& fd, float w, float h, bool menu_open)
{
    if (ui_toast_active())
    {
        float left = static_cast<float>(g_toast_until - GetTickCount64()) / 1000.f;
        ImVec2 sz = ImGui::CalcTextSize(g_toast);
        float pad = ImGui::GetFontSize() * 0.6f;
        ImVec2 a(w * 0.5f - sz.x * 0.5f - pad, h * 0.08f), b(w * 0.5f + sz.x * 0.5f + pad, h * 0.08f + sz.y + 2 * pad);
        int alpha = left < 1.0f ? static_cast<int>(255 * left) : 255;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(a, b, IM_COL32(20, 20, 20, alpha * 200 / 255), pad);
        fg->AddText(ImVec2(a.x + pad, a.y + pad), IM_COL32(255, 170, 60, alpha), g_toast);
    }
    int shown = 0, in_range = 0, arrows = 0, after_you = 0;
    g_vw = w;
    g_vh = h;
    static std::vector<View> views;
    const double now = ImGui::GetTime();
    if (fd.ok)
        update_views(fd, views, now);
    else
    {
        views.clear();
        g_tracks_start = -1.0;  // loading / main menu: start over when the world is back
    }
    // Over the game's own screens (Esc menu, photo mode, ...) the world is frozen behind a menu: hide, keep the tracks.
    const bool world = fd.ok && !(fd.paused && !g_settings.show_when_paused);
    if (g_settings.enabled && world)
    {
        const float fs = ImGui::GetFontSize(), px = ui_scale(h);
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(ImVec2(0, 0), ImVec2(w, h), false);  // the game's viewport only
        static std::vector<const View*> order, offscreen;
        order.clear();
        offscreen.clear();
        for (const View& v : views)
            if (v.n.dist <= g_settings.max_dist)
            {
                ++in_range;
                if (passes_filters(v.n))
                    order.push_back(&v);
            }
        std::sort(order.begin(), order.end(), [](const View* a, const View* b) { return a->n.dist < b->n.dist; });
        static std::vector<Marker> markers;
        markers.clear();
        for (const View* v : order)
        {
            if (v->n.targets_player && v->n.health > 0.f)
                ++after_you;
            Marker m{};
            m.v = v;
            if (project_box(fd, v->n, w, h, m.a, m.b, m.fx))
                markers.push_back(m);
            else
                offscreen.push_back(v);
        }
        const View* aimed = g_settings.focus_crosshair ? pick_crosshair(markers, w, h, now) : nullptr;
        for (Marker& m : markers)
            choose_detail(m, aimed);
        if (g_settings.show_cones)
            for (const View* v : order)
                if (wants_cone(*v, aimed))
                    draw_cone(dl, fd, *v);
        if (g_settings.show_routes)
            for (const View* v : order)
                draw_route(dl, fd, *v, aimed, px, fs);
        for (const View* v : offscreen)
            if (wants_arrow(*v))
            {
                // Non-threats also fade over the last 3 m before the arrow distance (mode "hostile NPCs nearby").
                const float arrow_edge = v->s.threat || g_settings.arrows_mode != 1
                                             ? 1.f
                                             : fminf(1.f, fmaxf(0.f, (g_settings.arrow_max_dist - v->n.dist) / 3.f));
                const float k = (v->s.threat ? 1.f : dist_alpha(v->n.dist)) * v->k * edge_alpha(v->n.dist) * arrow_edge;
                draw_edge_arrow(dl, fd, *v, w, h, k);
                ++arrows;
            }
        for (auto it = markers.rbegin(); it != markers.rend(); ++it)  // far first: near markers stay on top
        {
            draw_marker(dl, *it, px, fs);
            ++shown;
        }
        // Label order: NPCs in focus first (theirs keep their place, calm ones move aside), then by a sticky distance
        // that only follows changes over 1.5 m, so two NPCs at similar distances don't keep swapping places.
        for (const Marker& m : markers)
        {
            float& sd = g_sticky_dist.try_emplace(m.v->n.id, m.v->n.dist).first->second;
            if (fabsf(m.v->n.dist - sd) > 1.5f)
                sd = m.v->n.dist;
        }
        std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
            if (a.focus != b.focus)
                return a.focus;
            const float da = g_sticky_dist[a.v->n.id], db = g_sticky_dist[b.v->n.id];
            return da != db ? da < db : a.v->n.id < b.v->n.id;
        });
        draw_labels(dl, markers, px);
        dl->PopClipRect();
    }
    if (world && g_settings.show_zones)
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(ImVec2(0, 0), ImVec2(w, h), false);
        for (const Zone& z : fd.zones)
            draw_zone(dl, fd, z, w, h);
        dl->PopClipRect();
        zone_alerts(fd);
    }
    if (world)
    {
        update_alerts(fd, views);
        draw_alerts(fd, w, h);
        if (g_settings.enabled)
            draw_detect_panels(fd, views, w, h);
    }
    if (!menu_open)
        return;
    // Fixed position: a window dragged outside the game's viewport would draw where the game never clears.
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::Begin(MOD_TITLE " " MOD_VERSION_STR "###overlay_menu", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::Checkbox("Overlay enabled", &g_settings.enabled);
    ImGui::SameLine(0.f, ImGui::GetFontSize() * 1.5f);
    ImGui::Checkbox("Show while the game is paused", &g_settings.show_when_paused);
    if (ImGui::BeginTabBar("tabs"))
    {
        if (ImGui::BeginTabItem("Overlay", nullptr, tab_flags(0)))
        {
            menu_main_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Markers", nullptr, tab_flags(1)))
        {
            menu_markers_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stealth", nullptr, tab_flags(2)))
        {
            menu_stealth_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug", nullptr, tab_flags(3)))
        {
            menu_debug_tab(fd);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings", nullptr, tab_flags(4)))
        {
            ImGui::SeparatorText("Relation colours");
            color_edit("Friendly", g_settings.col_friendly);
            ImGui::SameLine();
            color_edit("Neutral", g_settings.col_neutral);
            ImGui::SameLine();
            color_edit("Hostile", g_settings.col_hostile);
            ImGui::SameLine();
            color_edit("Dead", g_settings.col_dead);
            ImGui::SameLine();
            if (ImGui::Button("Default colours"))
            {
                const Settings d;
                g_settings.col_friendly = d.col_friendly;
                g_settings.col_neutral = d.col_neutral;
                g_settings.col_hostile = d.col_hostile;
                g_settings.col_dead = d.col_dead;
            }
            ImGui::TextDisabled("Yellow and orange mean detection, red hostile and spotted: keep friendly / neutral apart.");
            ImGui::SeparatorText("Keys");
            const float kw = ImGui::GetFontSize() * 8.f;
            ImGui::SetNextItemWidth(kw);
            key_combo("Menu", g_settings.key_menu, g_settings.key_report, g_settings.key_unload);
            ImGui::SetNextItemWidth(kw);
            key_combo("NPC report", g_settings.key_report, g_settings.key_menu, g_settings.key_unload);
            ImGui::SetNextItemWidth(kw);
            key_combo("Unload overlay", g_settings.key_unload, g_settings.key_menu, g_settings.key_report);
            ImGui::SeparatorText("Settings file");
            ImGui::TextDisabled("Saved automatically to metro_npc_overlay.ini next to the DLL.");
            if (ImGui::Button("Reset to defaults"))
                ImGui::OpenPopup("reset?");
            if (ImGui::BeginPopup("reset?"))
            {
                ImGui::TextUnformatted("Reset all overlay settings to defaults?");
                if (ImGui::Button("Reset"))
                {
                    config_reset();
                    ui_toast("Settings reset to defaults", 3.f);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        g_tab_request = -1;
    }
    ImGui::Separator();
    if (fd.ok)
    {
        ImGui::Text("Map: %s   you: friend_type %u (%s)", fd.map, fd.player_friend_type,
                    friend_type_name(fd.player_friend_type));
        ImGui::Text("Markers: %d   arrows: %d   in range: %d   in world: %d", shown, arrows, in_range,
                    static_cast<int>(fd.npcs.size()));
        if (after_you)
            ImGui::TextColored(ImVec4(1.f, 0.25f, 0.25f, 1.f), "%d NPC(s) targeting you", after_you);
        if (!world)
            ImGui::TextDisabled("Hidden: the game is paused (%s)", fd.pause_reason);
    }
    else
        ImGui::TextDisabled("Not drawing: %s", fd.why);
    ImGui::Separator();
    ImGui::TextDisabled("%s: menu   %s: save NPC report   %s: unload overlay", config_key_name(g_settings.key_menu),
                        config_key_name(g_settings.key_report), config_key_name(g_settings.key_unload));
    ImGui::End();
}
