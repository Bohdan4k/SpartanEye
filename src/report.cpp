#include "report.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "display.h"
#include "log.h"

namespace
{
const char* rel_name(Relation r)
{
    return r == Relation::Friendly ? "friendly" : r == Relation::Neutral ? "neutral" : "hostile";
}
const char* cat_name(Category c)
{
    return c == Category::Human ? "human" : c == Category::Mutant ? "mutant" : "fauna";
}
}  // namespace

int report_write(const FrameData& fd, float radius)
{
    if (!fd.ok)
        return -1;
    std::vector<const Npc*> rows;
    for (const Npc& n : fd.npcs)
        if (n.dist <= radius)
            rows.push_back(&n);
    std::sort(rows.begin(), rows.end(), [](const Npc* a, const Npc* b) { return a->dist < b->dist; });

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[512];
    std::string out;
    snprintf(line, sizeof(line),
             "=== report %04u-%02u-%02u %02u:%02u:%02u  map %s  player pos=(%.2f, %.2f, %.2f) friend_type=%u (%s)  "
             "radius %.0f m  npcs %zu\r\n",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, fd.map, fd.player_pos[0], fd.player_pos[1],
             fd.player_pos[2], fd.player_friend_type, friend_type_name(fd.player_friend_type), radius, rows.size());
    out += line;
    const PlayerVisibility& pv = fd.pv;
    snprintf(line, sizeof(line),
             "    your visibility %.3f: light %.3f flash %.3f stance %.2f speed %.2f m/s (min %.2f k %.3f base %.2f) "
             "%s cover %.3f env %.3f%s\r\n",
             pv.visibility, pv.light, pv.flash, pv.stance, pv.speed, pv.speed_min, pv.speed_k, pv.speed_base,
             pv.crouched ? "crouched" : "standing", pv.cover, pv.env, pv.ok ? "" : " (unavailable)");
    out += line;
    out += "   id  class            category relation  ft  friend_type               dist      health     max_hp  "
           "ai_state      target_you  position                      name (display)\r\n";
    for (const Npc* n : rows)
    {
        char pos[64];
        snprintf(pos, sizeof(pos), "(%.2f, %.2f, %.2f)", n->pos[0], n->pos[1], n->pos[2]);
        snprintf(line, sizeof(line), "%5u  %-16s %-8s %-9s %3u  %-23s %6.1f %11.3f %10.3f  %-13s %-10s  %-29s %s (%s)\r\n",
                 n->id, n->cls, cat_name(n->cat), rel_name(n->rel), n->friend_type, friend_type_name(n->friend_type),
                 n->dist, n->health, n->max_health, n->has_brain ? mental_state_name(n->mental) : "-",
                 n->targets_player ? "yes" : "no", pos, n->name, n->display);
        out += line;
        if (n->vis.ok)
        {
            const Vision& v = n->vis;
            snprintf(line, sizeof(line),
                     "        vision: %s%.3f / noticed %.2f light %.2f seen %.2f%s = %.1f%%  range %.1f (x%.2f..%.2f) "
                     "back %.1f fov %.0f force %.1f  %+.2f/s\r\n",
                     v.has_entry ? "" : "no entry ", v.value, v.noticed, v.light, v.seen, v.surv ? " surv" : "",
                     detect_pct(*n), v.range, v.k_edge, v.k_centre, v.back_range, v.fov * 57.2958f, v.force_range,
                     v.rate);
            out += line;
        }
        if (n->route.ok)
        {
            const Route& r = n->route;
            std::string parts;
            for (int i = 0; i < r.nparts && i < 6; ++i)
                parts += std::string(i ? " > " : "") + part_name(r.parts[i]);
            snprintf(line, sizeof(line), "        ai: task [%s] script [%s] parts [%s]  -> %s\r\n", r.task, r.script,
                     parts.c_str(), intent_label(route_intent(r)));
            out += line;
            if (r.moving)
            {
                const float* e = r.pts[r.n - 1];
                snprintf(line, sizeof(line),
                         "        route: %d points (of %d), next (%.2f, %.2f, %.2f) stop (%.2f, %.2f, %.2f)%s facing %s\r\n",
                         r.n, r.total, r.pts[0][0], r.pts[0][1], r.pts[0][2], e[0], e[1], e[2], r.final_pt ? " final" : "",
                         r.has_dir ? "set" : "-");
                out += line;
            }
        }
    }
    out += "\r\n";

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%smetro_npc_reports.log", log_dir());
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return -1;
    DWORD written = 0;
    BOOL ok = WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(f);
    logf("report: %zu NPCs within %.0f m appended to metro_npc_reports.log", rows.size(), radius);
    return ok ? static_cast<int>(rows.size()) : -1;
}
