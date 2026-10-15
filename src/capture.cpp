#include "capture.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Text format, one record per line, fields separated by spaces. Strings have spaces replaced by '\x1F' on write.
//   MNO_FRAME 1
//   view <w> <h> <aspect> <game_time> <player_friend_type> <detect_ok> <map>
//   player <x> <y> <z>
//   cam <px> <py> <pz> <fx> <fy> <fz>
//   vp <16 floats>
//   pv <ok> <light> <flash> <stance> <speed> <speed_min> <speed_k> <speed_base> <crouched> <env_mode> <cover> <env> <visibility>
//   npc <id> <cls> <name> <display> <pos x3> <fwd x3> <vis_fov> <vis_range> <height> <health> <max_health> <dist>
//       <friend_type> <rel> <cat> <has_brain> <mental> <mood> <targets> <awareness> <los>
//       <vis: ok has_entry value noticed light seen range back_range fov k_edge k_centre min_dist lum_offset gain vel_k
//        min_lum far_sub near_dist force_range peri_dist peri_angle period surv rate>
//   zone <id> <name> <xf x16> <bmin x3> <bmax x3> <dist> <trigger> <player_inside>   (readers skip unknown records)
//   partnames <n> <name>...                    (the exe's part name table, so previews can name the parts)
//   route <id> <goal_kind> <moving> <n> <total> <final> <has_dir> <dir x3> <task> <script> <nparts> <part>...
//         <nprev> <part>... <points x3n>
namespace
{
std::string enc(const char* s)
{
    std::string r = s && *s ? s : "-";
    for (char& c : r)
        if (c == ' ')
            c = '\x1F';
    return r;
}
std::string dec(const char* s)
{
    std::string r = strcmp(s, "-") == 0 ? "" : s;
    for (char& c : r)
        if (c == '\x1F')
            c = ' ';
    return r;
}
}  // namespace

bool capture_write(const FrameData& fd, float w, float h, const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return false;
    fprintf(f, "MNO_FRAME 1\n");
    fprintf(f, "view %.1f %.1f %.6f %u %u %d %s\n", w, h, fd.aspect, fd.game_time, fd.player_friend_type,
            fd.detect_ok ? 1 : 0, enc(fd.map).c_str());
    fprintf(f, "player %.4f %.4f %.4f\n", fd.player_pos[0], fd.player_pos[1], fd.player_pos[2]);
    fprintf(f, "cam %.4f %.4f %.4f %.6f %.6f %.6f\n", fd.cam_pos[0], fd.cam_pos[1], fd.cam_pos[2], fd.cam_fwd[0],
            fd.cam_fwd[1], fd.cam_fwd[2]);
    fprintf(f, "vp");
    for (float v : fd.vp)
        fprintf(f, " %.9g", v);
    fprintf(f, "\n");
    const PlayerVisibility& p = fd.pv;
    fprintf(f, "pv %d %.5f %.5f %.4f %.4f %.4f %.4f %.4f %d %d %.5f %.5f %.5f\n", p.ok ? 1 : 0, p.light, p.flash, p.stance,
            p.speed, p.speed_min, p.speed_k, p.speed_base, p.crouched ? 1 : 0, p.env_mode, p.cover, p.env, p.visibility);
    for (const Npc& n : fd.npcs)
    {
        const Vision& v = n.vis;
        fprintf(f,
                "npc %u %s %s %s %.4f %.4f %.4f %.5f %.5f %.5f %.3f %.3f %.3f %.5f %.5f %.4f %u %d %d %d %u %d %d %.4f %d "
                "%d %d %.5f %.4f %.4f %.4f %.4f %.4f %.5f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.5f %u "
                "%d %.5f\n",
                n.id, enc(n.cls).c_str(), enc(n.name).c_str(), enc(n.display).c_str(), n.pos[0], n.pos[1], n.pos[2],
                n.fwd[0], n.fwd[1], n.fwd[2], n.vis_fov, n.vis_range, n.height, n.health, n.max_health, n.dist,
                n.friend_type, static_cast<int>(n.rel), static_cast<int>(n.cat), n.has_brain ? 1 : 0, n.mental,
                static_cast<int>(n.mood), n.targets_player ? 1 : 0, n.awareness, n.los, v.ok ? 1 : 0, v.has_entry ? 1 : 0,
                v.value, v.noticed, v.light, v.seen, v.range, v.back_range, v.fov, v.k_edge, v.k_centre, v.min_dist,
                v.lum_offset, v.gain, v.vel_k, v.min_lum, v.far_sub, v.near_dist, v.force_range, v.peri_dist,
                v.peri_angle, v.period, v.surv ? 1 : 0, v.rate);
    }
    fprintf(f, "partnames");
    int np = 0;
    while (np < 256 && *part_name(static_cast<uint8_t>(np)))
        ++np;
    fprintf(f, " %d", np);
    for (int i = 0; i < np; ++i)
        fprintf(f, " %s", part_name(static_cast<uint8_t>(i)));
    fprintf(f, "\n");
    for (const Npc& n : fd.npcs)
    {
        const Route& r = n.route;
        if (!r.ok)
            continue;
        fprintf(f, "route %u %u %d %d %d %d %d %.5f %.5f %.5f %s %s %u", n.id, r.goal_kind, r.moving ? 1 : 0, r.n, r.total,
                r.final_pt ? 1 : 0, r.has_dir ? 1 : 0, r.dir[0], r.dir[1], r.dir[2], enc(r.task).c_str(),
                enc(r.script).c_str(), r.nparts);
        for (int i = 0; i < r.nparts; ++i)
            fprintf(f, " %u", r.parts[i]);
        fprintf(f, " %u", r.nprev);
        for (int i = 0; i < r.nprev; ++i)
            fprintf(f, " %u", r.prev[i]);
        for (int i = 0; i < r.n; ++i)
            fprintf(f, " %.4f %.4f %.4f", r.pts[i][0], r.pts[i][1], r.pts[i][2]);
        fprintf(f, "\n");
    }
    for (const Zone& z : fd.zones)
    {
        fprintf(f, "zone %u %s", z.id, enc(z.name).c_str());
        for (float x : z.xf)
            fprintf(f, " %.9g", x);
        fprintf(f, " %.5f %.5f %.5f %.5f %.5f %.5f %.4f %d %d\n", z.bmin[0], z.bmin[1], z.bmin[2], z.bmax[0], z.bmax[1],
                z.bmax[2], z.dist, z.trigger ? 1 : 0, z.player_inside ? 1 : 0);
    }
    bool ok = ferror(f) == 0;
    ok = fclose(f) == 0 && ok;
    return ok;
}

bool capture_read(const char* path, FrameData& fd, float& w, float& h, std::vector<std::string>& pool)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;
    fd = FrameData{};
    pool.clear();
    pool.reserve(4096);  // c_str() pointers must stay valid: no reallocation below this
    static char mapbuf[64];
    char line[4096];
    bool header = false;
    while (fgets(line, sizeof(line), f))
    {
        char tag[16] = "";
        int off = 0;
        if (sscanf_s(line, "%15s%n", tag, static_cast<unsigned>(sizeof(tag)), &off) != 1)
            continue;
        const char* r = line + off;
        if (!strcmp(tag, "MNO_FRAME"))
            header = atoi(r) == 1;
        else if (!strcmp(tag, "view"))
        {
            unsigned gt = 0, ft = 0;
            int det = 0;
            char map[64] = "";
            sscanf_s(r, "%f %f %f %u %u %d %63s", &w, &h, &fd.aspect, &gt, &ft, &det, map, static_cast<unsigned>(sizeof(map)));
            fd.game_time = gt;
            fd.player_friend_type = static_cast<uint8_t>(ft);
            fd.detect_ok = det != 0;
            strncpy_s(fd.map, dec(map).c_str(), _TRUNCATE);
            strncpy_s(mapbuf, fd.map, _TRUNCATE);
        }
        else if (!strcmp(tag, "player"))
            sscanf_s(r, "%f %f %f", &fd.player_pos[0], &fd.player_pos[1], &fd.player_pos[2]);
        else if (!strcmp(tag, "cam"))
            sscanf_s(r, "%f %f %f %f %f %f", &fd.cam_pos[0], &fd.cam_pos[1], &fd.cam_pos[2], &fd.cam_fwd[0], &fd.cam_fwd[1],
                     &fd.cam_fwd[2]);
        else if (!strcmp(tag, "vp"))
        {
            const char* p = r;
            for (float& v : fd.vp)
                v = strtof(p, const_cast<char**>(&p));
        }
        else if (!strcmp(tag, "pv"))
        {
            PlayerVisibility& p = fd.pv;
            int ok = 0, cr = 0;
            sscanf_s(r, "%d %f %f %f %f %f %f %f %d %d %f %f %f", &ok, &p.light, &p.flash, &p.stance, &p.speed, &p.speed_min,
                     &p.speed_k, &p.speed_base, &cr, &p.env_mode, &p.cover, &p.env, &p.visibility);
            p.ok = ok != 0;
            p.crouched = cr != 0;
        }
        else if (!strcmp(tag, "npc"))
        {
            Npc n{};
            Vision& v = n.vis;
            unsigned id = 0, ft = 0, mental = 0, period = 0;
            int rel = 0, cat = 0, brain = 0, mood = 0, tgt = 0, los = 0, vok = 0, ent = 0, surv = 0;
            char cls[64] = "", name[64] = "", disp[64] = "";
            int got = sscanf_s(
                r,
                "%u %63s %63s %63s %f %f %f %f %f %f %f %f %f %f %f %f %u %d %d %d %u %d %d %f %d %d %d %f %f %f %f %f %f "
                "%f %f %f %f %f %f %f %f %f %f %f %f %f %u %d %f",
                &id, cls, static_cast<unsigned>(sizeof(cls)), name, static_cast<unsigned>(sizeof(name)), disp,
                static_cast<unsigned>(sizeof(disp)), &n.pos[0], &n.pos[1], &n.pos[2], &n.fwd[0], &n.fwd[1], &n.fwd[2],
                &n.vis_fov, &n.vis_range, &n.height, &n.health, &n.max_health, &n.dist, &ft, &rel, &cat, &brain, &mental,
                &mood, &tgt, &n.awareness, &los, &vok, &ent, &v.value, &v.noticed, &v.light, &v.seen, &v.range,
                &v.back_range, &v.fov, &v.k_edge, &v.k_centre, &v.min_dist, &v.lum_offset, &v.gain, &v.vel_k, &v.min_lum,
                &v.far_sub, &v.near_dist, &v.force_range, &v.peri_dist, &v.peri_angle, &period, &surv, &v.rate);
            if (got < 49)
                continue;
            n.id = static_cast<uint16_t>(id);
            pool.push_back(dec(cls));
            n.cls = pool.back().c_str();
            strncpy_s(n.name, dec(name).c_str(), _TRUNCATE);
            pool.push_back(dec(disp));
            n.display = pool.back().c_str();
            n.friend_type = static_cast<uint8_t>(ft);
            n.rel = static_cast<Relation>(rel);
            n.cat = static_cast<Category>(cat);
            n.has_brain = brain != 0;
            n.mental = static_cast<uint8_t>(mental);
            n.mood = static_cast<Mood>(mood);
            n.targets_player = tgt != 0;
            n.los = static_cast<int8_t>(los);
            v.ok = vok != 0;
            v.has_entry = ent != 0;
            v.period = period;
            v.surv = surv != 0;
            fd.npcs.push_back(n);
        }
        else if (!strcmp(tag, "partnames"))
        {
            std::vector<std::string> names;
            int np = 0, used = 0;
            const char* p = r;
            if (sscanf_s(p, "%d%n", &np, &used) == 1)
                for (p += used; np-- > 0;)
                {
                    char nm[64];
                    if (sscanf_s(p, "%63s%n", nm, static_cast<unsigned>(sizeof(nm)), &used) != 1)
                        break;
                    names.push_back(nm);
                    p += used;
                }
            part_names_offline(names);
        }
        else if (!strcmp(tag, "route"))
        {
            unsigned id = 0, goal = 0, nparts = 0, nprev = 0;
            int moving = 0, nn = 0, total = 0, fin = 0, hasdir = 0, used = 0;
            float d[3];
            char task[64], scr[64];
            const char* p = r;
            if (sscanf_s(p, "%u %u %d %d %d %d %d %f %f %f %63s %63s %u%n", &id, &goal, &moving, &nn, &total, &fin, &hasdir,
                         &d[0], &d[1], &d[2], task, static_cast<unsigned>(sizeof(task)), scr,
                         static_cast<unsigned>(sizeof(scr)), &nparts, &used) != 13)
                continue;
            Npc* n = nullptr;
            for (Npc& x : fd.npcs)
                if (x.id == id)
                    n = &x;
            if (!n || nn < 0 || nn > kRoutePts || nparts > kRouteParts)
                continue;
            Route& rt = n->route;
            rt.ok = true;
            rt.goal_kind = goal;
            rt.moving = moving != 0;
            rt.n = nn;
            rt.total = total;
            rt.final_pt = fin != 0;
            rt.has_dir = hasdir != 0;
            memcpy(rt.dir, d, sizeof(d));
            pool.push_back(dec(task));
            rt.task = pool.back().c_str();
            strncpy_s(rt.script, dec(scr).c_str(), _TRUNCATE);
            p += used;
            char* end = nullptr;
            rt.nparts = static_cast<uint8_t>(nparts);
            for (unsigned i = 0; i < nparts; ++i, p = end)
                rt.parts[i] = static_cast<uint8_t>(strtoul(p, &end, 10));
            nprev = strtoul(p, &end, 10);
            p = end;
            rt.nprev = static_cast<uint8_t>(nprev > 3 ? 3 : nprev);
            for (unsigned i = 0; i < nprev; ++i, p = end)
            {
                const unsigned v = strtoul(p, &end, 10);
                if (i < 3)
                    rt.prev[i] = static_cast<uint8_t>(v);
            }
            for (int i = 0; i < nn; ++i)
                for (int k = 0; k < 3; ++k, p = end)
                    rt.pts[i][k] = strtof(p, &end);
        }
        else if (!strcmp(tag, "zone"))
        {
            Zone z{};
            unsigned id = 0;
            int trig = 0, inside = 0, used = 0;
            char name[64] = "";
            if (sscanf_s(r, "%u %63s%n", &id, name, static_cast<unsigned>(sizeof(name)), &used) != 2)
                continue;
            const char* p = r + used;
            char* end = nullptr;
            float vals[23];
            int got = 0;
            for (; got < 23; ++got, p = end)
            {
                vals[got] = strtof(p, &end);
                if (end == p)
                    break;
            }
            if (got < 23 || sscanf_s(p, "%d %d", &trig, &inside) != 2)
                continue;
            z.id = static_cast<uint16_t>(id);
            strncpy_s(z.name, dec(name).c_str(), _TRUNCATE);
            memcpy(z.xf, vals, sizeof(z.xf));
            memcpy(z.bmin, vals + 16, sizeof(z.bmin));
            memcpy(z.bmax, vals + 19, sizeof(z.bmax));
            z.dist = vals[22];
            z.trigger = trig != 0;
            z.player_inside = inside != 0;
            fd.zones.push_back(z);
        }
    }
    fclose(f);
    fd.ok = header;
    return header;
}
