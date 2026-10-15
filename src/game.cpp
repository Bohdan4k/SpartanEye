#include "game.h"
#include "version.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "log.h"
#include "sig.h"
#include "state.h"

namespace
{
constexpr uint32_t kBuildTimestamp = GAME_BUILD_TIMESTAMP;  // MetroExodus.exe PE timestamp (version.h)

// Constants with static evidence in re/STRUCTS.md (not exposed by a signature).
constexpr uint32_t kXformOff = 0x110;    // float4x4, row 3 = position (feet)
constexpr uint32_t kIdOff = 0x170;       // u16 entity id == table slot
constexpr uint32_t kFlagsOff = 0x174;    // u32 flags
constexpr uint32_t kFlagHidden = 0x20;   // not placed in the world
constexpr uint32_t kMapNameOff = 0xF8;   // level: pooled string handle of the map name
constexpr uint32_t kCamVpOff = 0xC0;     // camera: VP matrix
constexpr uint32_t kCamAspectOff = 0x4B8;  // camera: float aspect
constexpr uint32_t kCamPosOff = 0x00, kCamFwdOff = 0x10;
// Human-NPC vision params (loader fn 0x14082EBE0: "player_vision_bwd_range" -> +0xFF4, "player_vision_fov" -> +0xFF8;
// "player_vision_range" is the store right before, +0xFF0 [hypothesis]). Other classes leave this block at 0.
constexpr uint32_t kVisRangeOff = 0xFF0, kVisFovOff = 0xFF8;
constexpr uint32_t kCamSize = 0x4D0;     // camera object size (main -> unjittered copy)
constexpr uint32_t kSlots = 0xFFFF;
constexpr uint32_t kSweepPerFrame = 4096;
constexpr uint8_t kZoneCls = 0xFE;        // g_slot_cls marker for zones
constexpr uint32_t kZoneBoxOff = 0x150;  // zone: local bounding box min (float4) / max (float4)  // table slots classified per frame (full sweep every 16 frames)

// Signatures (same as tools/ee_player.py, tools/ee_reader.py, tools/ee_camera.py).
constexpr const char* SIG_LEVEL_PLAYER = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 83 78 ?? 00 74 ?? 48 8B CA FF 15";
constexpr const char* SIG_STATS = "F3 0F 10 9B ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? F3 0F 10 93 ?? ?? ?? ?? 48 8B 0F 0F 5A DB";
constexpr const char* SIG_HEALTH_LOAD = "48 8B 0E E8 ?? ?? ?? ?? F3 0F 11 83 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 8B 0E 33 D2 E8";
constexpr const char* SIG_PLAYER_VTBL = "48 8B 03 48 8D 0D ?? ?? ?? ?? 48 3B C1 0F 29 74 24 50";
constexpr const char* SIG_ENT_SERVER = "0F B7 0A B8 FF FF 00 00 48 8B 3D ?? ?? ?? ?? BD FF FF FF FF";
constexpr const char* SIG_ENT_TABLE = "48 8B 47 ?? 0F B7 C9 48 8B 0C C8 48 85 C9 74 ?? 48 81 C1 ?? ?? ?? ?? 48 8B 19";
constexpr const char* SIG_NAME = "8B 8E ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 4C 8B D8 41 B8 FF FF FF 00";
constexpr const char* SIG_CLSID = "48 8B 80 ?? ?? ?? ?? 8B 08 3B 0D ?? ?? ?? ?? 89 4C 24 50";
// base-entity loader: read u8 "friend_type" -> mov [rbx+off],al. 3 matches; the right one has lea r8,["friend_type"]
// 31 bytes earlier.
constexpr const char* SIG_FRIEND_TYPE = "48 8B 0E E8 ?? ?? ?? ?? 88 83 ?? ?? ?? ?? 4C 8D 05";
constexpr int kFriendTypeKeyBack = 31;
// monster_is_enemy: mov rdi,rcx; mov r8d,1; mov rcx,[rcx+brain]; mov rbx,rdx; mov rax,[rcx]; call [rax+...]
constexpr const char* SIG_BRAIN = "48 8B F9 41 B8 01 00 00 00 48 8B 89 ?? ?? ?? ?? 48 8B DA 48 8B 01 FF 90";
// brain debug dump: movzx eax,byte [r14+mental]; cmp al,0x1E (alert); ja; je; test al,al
constexpr const char* SIG_MENTAL = "41 0F B6 86 ?? ?? ?? ?? 3C 1E 77 ?? 74 ?? 84 C0";
// brain debug dump: mov rax,[r14+enemy_link]; xor ebx,ebx; movaps xmm6,[rsp+...]
constexpr const char* SIG_BRAIN_ENEMY = "49 8B 86 ?? ?? ?? ?? 33 DB 0F 28 B4 24";
constexpr uint32_t kBrainOwnerOff = 0x08;  // brain -> owner entity (fn 0x1409423C0)
// game time (ms): helper fn 0x14006E4A0 reads it
constexpr const char* SIG_GAME_TIME = "44 0F B6 5A 01 48 8B DA 44 8B 0D ?? ?? ?? ?? 45 03 D8";
// brain save: lea r8,["_last_enemy_visible_strict_time"]; mov rcx,[rdi]; xor edx,edx; call; mov [rbx+off],eax
constexpr const char* SIG_VIS_STRICT = "48 8B 0F 33 D2 E8 ?? ?? ?? ?? 89 83 ?? ?? ?? ?? 4C 8D 0D";
constexpr uint32_t kSeenRecentlyMs = 600;  // strict sighting within this window = line of sight
constexpr const char* SIG_CAM_PAIR = "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2";
// Map state (u32 global; re/STRUCTS.md §8): 8 = "mapstate_running", 2 = "mapstate_loaded_and_waiting" (loading screen
// with the new level already loaded), 1 / 4 / 32 while loading. Two signatures that must agree:
// fn 0x140377720: mov eax,[state]; cmp eax,8; jz; mov rcx,[..]; mov edx,-1; call [WaitForSingleObject]
constexpr const char* SIG_MAP_STATE_READ = "8B 05 ?? ?? ?? ?? 83 F8 08 74 14 48 8B 0D ?? ?? ?? ?? BA FF FF FF FF FF 15";
// loader_wait_finish (2 copies): mov rcx,[..]; lea rdx,[..]; xor r9d,r9d; mov dword [state],8; lea r8d,[r9+0x10]
constexpr const char* SIG_MAP_STATE_RUN =
    "48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 45 33 C9 C7 05 ?? ?? ?? ?? 08 00 00 00 45 8D 41 10";
constexpr uint32_t kMapRunning = 8;
// Pause stack (array of pooled reason strings, count at +0xC): PUSH / POP PAUSE code, 3 matches that must agree:
// mov r8d,[count]; mov r9,[array]; mov eax,[reason]
constexpr const char* SIG_PAUSE_STACK = "44 8B 05 ?? ?? ?? ?? 4C 8B 0D ?? ?? ?? ?? 8B 05";
constexpr uint32_t kPauseCountOff = 0xC;
constexpr uint64_t kMapSettleMs = 250;  // after the map starts running, wait this long (first frames still show loading)

// ---- detection (re/STRUCTS.md §6) ----
// vision formula fn 0x140F9B9B0: mov r11,rsp; push rbx; sub rsp,..; movss xmm5,[rcx+range]; xorps; mov [r11+8],rbp
constexpr const char* SIG_VIS_FORMULA = "4C 8B DC 53 48 81 EC ?? ?? ?? ?? F3 0F 10 69 ?? 0F 57 C0 49 89 6B 08";
constexpr const char* SIG_VIS_KEDGE = "F3 44 0F 59 71 ??";                                 // mulss xmm14,[rcx+k_edge]
constexpr const char* SIG_VIS_PERIOD = "8B 41 ?? F3 48 0F 2A C0 48 8B 41 ?? 48 8B 48 ??";  // period; owner chain
constexpr const char* SIG_VIS_POS = "0F 5C A1 ?? ?? ?? ??";                                // subps xmm4,[rcx+pos]
// vision update fn 0x140F878D0: mov r11,[rdi+list]; movzx r8d,word [rdi+count]
constexpr const char* SIG_VIS_LIST = "4C 8B 9F ?? ?? ?? ?? 44 0F B7 87 ?? ?? ?? ??";
// devirtualised calls compare an object's vtable with the vision class vtable (2 sites, same vtable)
constexpr const char* SIG_VIS_VTBL =
    "48 8B 13 48 8D 3D ?? ?? ?? ?? 48 3B D7 0F 85 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ??";
// player visibility fn 0x14081B8A0 (2 identical copies): light, camera light, flash end time, light k, params, flash,
// stance, then the call of the environment factor
constexpr const char* SIG_PLAYER_VIS =
    "F3 0F 10 81 ?? ?? ?? ?? EB 08 F3 0F 10 81 ?? ?? ?? ?? 8B 81 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? F3 0F 59 81 ?? ?? ?? ?? "
    "72 05 0F 57 F6 EB 0F 48 8B 81 ?? ?? ?? ?? F3 0F 10 B0 ?? ?? ?? ?? F3 0F 5F F0 F3 0F 59 B1 ?? ?? ?? ?? E8";
constexpr const char* SIG_PLAYER_SPEED_MIN = "0F 2F 90 ?? ?? ?? ?? 76 ?? 48 8B 03 48 8D 54 24 ?? 48 8B CB FF 90";
constexpr const char* SIG_PLAYER_SPEED_K = "F3 0F 59 90 ?? ?? ?? ?? 0F 28 74 24 ?? F3 0F 58 90 ?? ?? ?? ??";
// environment factor fn 0x14088E040 (through the call above): flag + movement state; crouch value; world, vegetation
constexpr const char* SIG_ENV_A = "83 B9 ?? ?? ?? ?? 00 48 8B D9 0F 84 ?? ?? ?? ?? 48 8B 81 ?? ?? ?? ??";
constexpr const char* SIG_ENV_B = "48 8B 40 ?? 8B 78 ?? 48 8B 03 C1 EF ?? 40 80 E7 ??";
constexpr const char* SIG_ENV_CMP = "74 ?? 40 80 FF ?? 75 ??";
constexpr const char* SIG_ENV_C =
    "48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 8B BB ?? ?? ?? ?? 48 8D 93 ?? ?? ?? ?? 48 8B 01 F3 0F 10 97 ?? ?? ?? "
    "?? 49 3B C0 75 ?? 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 D0 F3 0F 10 87 ?? ?? ?? ??";
// vegetation density fn 0x140D8A470 (through the call above)
constexpr const char* SIG_VEG_COUNT = "66 44 3B A7 ?? ?? ?? ??";
constexpr const char* SIG_VEG_MAXR = "F3 0F 5D 35 ?? ?? ?? ??";
constexpr const char* SIG_VEG_OX = "44 8B BF ?? ?? ?? ?? 0F 1F 00 41 BA 9E 00 00 00";
constexpr const char* SIG_VEG_OZ = "8B AF ?? ?? ?? ?? 49 63 F3";
constexpr const char* SIG_VEG_CELLS = "4C 8B 8C CF ?? ?? ?? ?? 41 F6 41 ?? 03";
constexpr const char* SIG_VEG_INST = "0F B7 5A ?? F3 0F 10 6A ??";
constexpr const char* SIG_VEG_MODELS = "0F B7 84 DF ?? ?? ?? ?? 48 8D 0C 80 48 8B 84 CF ?? ?? ?? ??";
constexpr const char* SIG_VEG_BBOX = "0F 10 40 ?? 0F 10 50 ?? 0F 28 CA";
constexpr const char* SIG_VEG_DENS =
    "48 8B 84 DF ?? ?? ?? ?? 0F 57 DB F3 0F 5E E1 C7 84 24 ?? ?? ?? ?? ?? ?? ?? ?? 44 89 64 24 ?? 48 8B 48 ?? F3 0F 59 "
    "69 ??";
constexpr const char* SIG_VEG_CELLN = "44 0F B7 50 ?? 45 85 D2";

// Routes and AI task (re/STRUCTS.md §7). Every one matches exactly once in build 24973569.
// brain update fn 0x140946e90: mov edx,r15d; lea rcx,[r14+motor]; call motor_update; cmp byte [r14+..],0
constexpr const char* SIG_MOTOR_CALL = "41 8B D7 49 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? 41 80 BE ?? ?? ?? ?? 00";
// motor_update fn 0x140A75FA0 prologue, ends with cmp [rdi+goal],esi (goal set)
constexpr const char* SIG_MOTOR_UPDATE =
    "40 53 55 56 57 41 54 41 56 48 83 EC ?? 48 8B F9 0F 29 74 24 ?? 48 8B 09 33 F6 8B EA 8B 81 ?? ?? ?? ?? 83 E0 01 39 77 ??";
// fn 0x140A75500 (update waypoint): movzx eax,[rbx+count]; imul eax,eax,stride; sub eax,stride; add rax,[rbx+points];
// jz; test byte [rax+flags],1 (final point)
constexpr const char* SIG_PATH = "0F B7 83 ?? ?? ?? ?? 6B C0 ?? 83 E8 ?? 48 03 83 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? F6 40 ?? 01";
// same function: facing at the end = [[rbx+look]]+dir_in_obj if byte [rbx+flags] & mask, else [rbx+dir]
constexpr const char* SIG_END_DIR = "48 8B 8B ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 09 48 85 C9 74 ?? F6 83 ?? ?? ?? ?? ?? 74 ?? 48 85 C9 "
                                    "48 8D 41 ?? BA ?? ?? ?? ?? 48 0F 44 C2 0F 10 00 EB ?? 0F 10 83 ?? ?? ?? ??";
// brain dump fn 0x1409423C0: mov r8,[r14+task]; test; jz; movzx edx,word [r14+ntasks]; ...; mov r9,[r14+tasks]
constexpr const char* SIG_TASK = "4D 8B 86 ?? ?? ?? ?? 4D 85 C0 74 ?? 41 0F B7 96 ?? ?? ?? ?? 33 F6 85 D2 74 ?? 4D 8B 8E ?? ?? ?? ??";
// same function: task vtable getters, part name / script name / type name
constexpr const char* SIG_TASK_SLOTS = "49 8B 07 49 8B CF FF 90 ?? ?? ?? ?? 4D 8B 07 49 8B CF 48 8B F8 41 FF 90 ?? ?? ?? ?? "
                                       "4D 8B 07 49 8B CF 48 8B D8 41 FF 90 ?? ?? ?? ??";
// part name getter (task vtbl part slot): script = [task+s]; n = word [script+n]; idx = [task+i];
// type = byte [[script+a] + idx*0x90]; name = [table + type*16]
constexpr const char* SIG_PART_GETTER = "4C 8B 41 ?? 4D 85 C0 74 ?? 41 0F B7 40 ?? 39 41 ?? 73 ?? 8B 41 ?? 8D 0C C0 49 8B 40 ?? "
                                        "C1 E1 04 0F B6 04 01 48 8D 0D ?? ?? ?? ?? 48 03 C0 48 8B 04 C1 C3";
constexpr size_t kPartGetterLen = 51;
// script name getter (task vtbl script slot): string handle at [[task+s]+h]
constexpr const char* SIG_SCR_GETTER = "48 8B 41 ?? 48 85 C0 74 ?? 8B 48 ?? 85 C9 74 ??";
constexpr uint32_t kPartStride = 0x90;  // lea ecx,[rax+rax*8]; shl ecx,4 in the part getter (part of its pattern)

// Vision object layout (fn 0x140F878D0 / fn 0x140F9B9B0, live-verified on Volga). Accepted only when the signature-read
// anchors (range, k_edge, period, list, count) sit exactly here; any shift disables exact detection.
namespace VO
{
constexpr uint32_t owner = 0x08;  // -> brain sub-object, whose +0x08 is the owner entity
constexpr uint32_t range = 0x10, back = 0x14, fov = 0x18, k_edge = 0x1C, k_centre = 0x20, min_dist = 0x24;
constexpr uint32_t period = 0x28, lum_off = 0x30, vel_k = 0x34, gain = 0x3C, min_lum = 0x40, far_sub = 0x44;
constexpr uint32_t force = 0x54, peri_dist = 0x58, peri_angle = 0x5C;
constexpr uint32_t noticed = 0x68, light = 0x70, seen = 0x74;               // and the near distance (= noticed)
constexpr uint32_t s_noticed = 0x78, s_light = 0x80, s_seen = 0x84, surv = 0x88;
constexpr uint32_t list = 0x90, count = 0x9A;                               // 16-byte {link, float value, ..}
constexpr uint32_t block = 0x10, block_size = 0x90;                         // one read: +0x10 .. +0x9F
}  // namespace VO
constexpr uint32_t kBrainVisionDefault = 0xE0;  // brain -> first feel object (vision); re-derived if a patch moves it
constexpr float kNearFloor = 0.15f;              // fn 0x140F9B9B0 constant (data 0x14147BCAC): light floor near the NPC

struct Detect
{
    bool vis_ok = false;           // vision layout verified, vtable known
    uintptr_t vis_vtbl = 0;
    uint32_t brain_vis_off = kBrainVisionDefault;
    bool pv_ok = false;            // player visibility offsets
    uint32_t lum = 0, flash_until = 0, lum_k = 0, stance = 0;    // player
    uint32_t flash = 0, speed_min = 0, speed_k = 0, speed_base = 0;  // player class params ([player + params_off])
    bool env_ok = false;           // environment factor (crouch state + vegetation hook-up)
    uint32_t env_flag = 0, env_anim = 0;
    uint8_t env_anim2 = 0, env_state = 0, env_shift = 0, env_mask = 0, env_crouch = 0;
    uintptr_t world_global = 0, world_vtbl = 0;
    uint32_t world_veg = 0, env_radius = 0, env_k = 0;
    bool veg_ok = false;           // vegetation grid
    uint32_t veg_count = 0, veg_ox = 0, veg_oz = 0, veg_cells = 0, veg_types = 0, veg_models = 0, veg_dens = 0;
    uint8_t cell_flags = 0, cell_n = 0, inst_type = 0, inst_scale = 0, bbox_a = 0, bbox_b = 0, dens_p = 0, dens_f = 0;
    float veg_maxr = 0;
} D;

struct Resolved
{
    uintptr_t level_global = 0;     // &g_level
    uint32_t player_off = 0;        // 0x28
    uintptr_t player_vtbl = 0;
    uint32_t health_off = 0;        // 0x3B8
    uintptr_t entities_global = 0;  // &g_entities
    uint32_t table_off = 0;         // 0x10
    uint32_t name_off = 0;          // 0x238
    uintptr_t string_pool = 0;      // &g_string_pool_pages
    uint32_t params_off = 0;        // 0x240
    uint32_t friend_off = 0;        // 0x3D9
    uint32_t link_off = 0;          // 0xE0: the interface entity links point at
    uint32_t brain_off = 0;         // 0xF10 (0 = NPC state off)
    uint32_t mental_off = 0;        // 0xA51 in the brain
    uint32_t enemy_off = 0;         // 0xC70 in the brain (0 = "targets you" off)
    uint32_t vis_strict_off = 0;    // 0xA20 in the brain: last strict sighting of the enemy (game ms), 0 = off
    uintptr_t game_time = 0;        // &game time (ms), 0 = off
    uintptr_t cam_unjit = 0;        // unjittered camera object
    uintptr_t map_state = 0;        // &map state (u32), 0 = loading-screen check off
    uintptr_t pause_stack = 0;      // &pause stack {ptr, .., u32 count at +0xC}, 0 = pause detection off
} R;
bool g_ready = false;

// Creature classes (tools/ee_reader.py). Heights only size the marker; they are rough guesses, not game data.
using C = Category;
struct ClassInfo
{
    const char* name;
    float height;
    Category cat;
    uint32_t clsid;
};
ClassInfo kClasses[] = {
    // humans
    {"NPC", 1.8f, C::Human, 0}, {"HUMAN", 1.8f, C::Human, 0}, {"SIMPLE_NPC", 1.8f, C::Human, 0},
    {"DUMMY_NPC", 1.8f, C::Human, 0}, {"NPC_FX", 1.8f, C::Human, 0}, {"WOMAN", 1.75f, C::Human, 0},
    {"WOMAN_COMBAT", 1.75f, C::Human, 0}, {"WOMAN_STRIP", 1.75f, C::Human, 0}, {"KID", 1.3f, C::Human, 0},
    {"CANNIBAL", 1.8f, C::Human, 0},
    // mutants
    {"MONSTER", 1.8f, C::Mutant, 0}, {"SIMPLE_MONSTER", 1.8f, C::Mutant, 0}, {"NOSALIS", 1.6f, C::Mutant, 0},
    {"NOSALIS_FEMALE", 1.8f, C::Mutant, 0}, {"NOSACH", 1.6f, C::Mutant, 0}, {"LURKER", 1.2f, C::Mutant, 0},
    {"WATCHMAN", 1.4f, C::Mutant, 0}, {"BIG_MOTHER", 2.5f, C::Mutant, 0}, {"ARAHIND", 1.0f, C::Mutant, 0},
    {"AQUA_MALE_BIG", 2.0f, C::Mutant, 0}, {"AQUA_MALE_SMALL", 1.4f, C::Mutant, 0}, {"AQUA_FEMALE", 3.0f, C::Mutant, 0},
    {"GRIZLY", 2.2f, C::Mutant, 0}, {"HUMANIMAL", 2.0f, C::Mutant, 0}, {"LIBRARIAN", 2.2f, C::Mutant, 0},
    {"DARK", 1.8f, C::Mutant, 0}, {"DARKCHILD", 1.3f, C::Mutant, 0}, {"AMEBA", 1.0f, C::Mutant, 0},
    {"HARPY", 1.5f, C::Mutant, 0}, {"CATFISH", 3.0f, C::Mutant, 0}, {"BLIND", 1.8f, C::Mutant, 0},
    // fauna
    {"DOG", 0.9f, C::Fauna, 0}, {"DEER", 1.5f, C::Fauna, 0}, {"RABBIT", 0.4f, C::Fauna, 0}, {"SNAKE", 0.4f, C::Fauna, 0},
    {"RAT", 0.3f, C::Fauna, 0}, {"FROG", 0.3f, C::Fauna, 0}, {"WORM", 0.5f, C::Fauna, 0},
    {"SEA_CUCUMBER", 0.5f, C::Fauna, 0}, {"FLYING_CREATURE", 1.0f, C::Fauna, 0},
};
constexpr size_t kNumClasses = sizeof(kClasses) / sizeof(kClasses[0]);
static_assert(kNumClasses < 255, "class index must fit in uint8_t");

uint32_t crc32(const char* s)  // reflected CRC-32 (zlib), the engine's clsid hash
{
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; ++s)
    {
        c ^= static_cast<uint8_t>(*s);
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// ---- guarded reads (the game frees entities on other threads; a stale pointer must not crash us) ----
bool rd(uintptr_t src, void* dst, size_t n)
{
    if (src < 0x10000 || src > 0x7FFFFFFEFFFF)
        return false;
    __try
    {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
template <class T>
bool rdv(uintptr_t a, T& v) { return rd(a, &v, sizeof(T)); }

bool read_string(uint32_t handle, char* out, size_t cap)
{
    out[0] = 0;
    if (!handle)
        return false;
    uintptr_t page = 0;
    if (!rdv(R.string_pool + 8ull * (handle >> 24), page) || !page)
        return false;
    char tmp[64];
    if (!rd(page + (handle & 0xFFFFFF) + 0x14, tmp, sizeof(tmp)))
        return false;
    tmp[sizeof(tmp) - 1] = 0;
    strncpy_s(out, cap, tmp, _TRUNCATE);
    return true;
}

// ---- per-slot tracking ----
uintptr_t g_table = 0;                 // table address the state below belongs to
uintptr_t g_slot_ent[kSlots + 1];      // entity pointer last classified per slot
uint8_t g_slot_cls[kSlots + 1];        // 0 = not a creature, else 1 + index into kClasses
char g_slot_name[kSlots + 1][32];      // cached entity name (truncated)
std::vector<uint16_t> g_creatures;     // slots with a creature class (rebuilt each sweep)
std::vector<uint16_t> g_zones;         // slots with a zone (rebuilt each sweep)
uint32_t g_zone_clsid = 0;
uint32_t g_sweep = 0;
std::unordered_map<uintptr_t, float> g_max_hp;  // entity pointer -> max health seen
uintptr_t g_map_level = 0;
char g_map_name[64] = "";
struct RateState
{
    float value = 0;
    uint32_t t = 0;
    float rate = 0;
};
std::unordered_map<uintptr_t, RateState> g_rates;  // entity -> observed rate of its value for the player

void reset_tracking(uintptr_t table)
{
    g_table = table;
    memset(g_slot_ent, 0, sizeof(g_slot_ent));
    memset(g_slot_cls, 0, sizeof(g_slot_cls));
    g_creatures.clear();
    g_zones.clear();
    g_sweep = 0;
    g_max_hp.clear();
    g_rates.clear();
}

uint8_t classify(uintptr_t e, uint16_t slot)
{
    uintptr_t params = 0;
    uint32_t clsid = 0;
    uint16_t id = 0;
    if (!rdv(e + R.params_off, params) || !rdv(params, clsid) || !rdv(e + kIdOff, id) || id != slot)
        return 0;
    for (size_t i = 0; i < kNumClasses; ++i)
        if (kClasses[i].clsid == clsid)
            return static_cast<uint8_t>(i + 1);
    return clsid == g_zone_clsid ? kZoneCls : 0;
}

void sweep(uintptr_t table)
{
    static uintptr_t buf[kSweepPerFrame];  // render thread only; keep 32 KB off the game thread's stack
    uint32_t start = g_sweep, n = (kSlots - start < kSweepPerFrame) ? kSlots - start : kSweepPerFrame;
    if (!rd(table + 8ull * start, buf, 8ull * n))
        return;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t slot = static_cast<uint16_t>(start + i);
        uintptr_t e = buf[i];
        if (e == g_slot_ent[slot])
            continue;
        if (g_slot_ent[slot])
            g_max_hp.erase(g_slot_ent[slot]);
        g_slot_ent[slot] = e;
        uint8_t c = e ? classify(e, slot) : 0;
        if (c && !g_slot_cls[slot])
            (c == kZoneCls ? g_zones : g_creatures).push_back(slot);
        g_slot_cls[slot] = c;
        if (c)
        {
            uint32_t h = 0;
            char nm[48];
            if (rdv(e + R.name_off, h) && read_string(h, nm, sizeof(nm)))
                strncpy_s(g_slot_name[slot], nm, _TRUNCATE);
            else
                g_slot_name[slot][0] = 0;
        }
    }
    g_sweep = start + n;
    if (g_sweep >= kSlots)
    {
        g_sweep = 0;
        g_creatures.clear();
        g_zones.clear();
        for (uint32_t s = 0; s < kSlots; ++s)
            if (g_slot_cls[s])
                (g_slot_cls[s] == kZoneCls ? g_zones : g_creatures).push_back(static_cast<uint16_t>(s));
    }
}

// The engine's base rule (entity_is_enemy_base): enemies iff both types are set and (either is deathmatch, or the
// player's is enemy_for_all, or they differ). Per-NPC AI overrides (explicit enemy lists) are not modelled.
Relation relation(uint8_t npc, uint8_t player)
{
    if (npc == 0 || player == 0)
        return Relation::Neutral;
    if (npc == 5 || player == 5 || player == 3 || npc != player)
        return Relation::Hostile;
    return Relation::Friendly;
}

Mood mood_of(uint8_t m)
{
    if (m == 0)
        return Mood::Calm;
    return m >= 40 ? Mood::Combat : Mood::Alert;  // 10 disturbed, <30 light alert, 30 alert | 40 uber alert, 50 danger
}

// Story characters, matched as substrings of the (lower-cased) entity name. Everyone else gets a class name.
struct NameKey
{
    const char* key;
    const char* display;
};
const NameKey kCharacters[] = {
    {"miller", "Miller"}, {"kirill", "Kirill"}, {"kiril", "Kirill"}, {"anna", "Anna"}, {"ann_", "Anna"},
    {"katya", "Katya"}, {"krest", "Krest"}, {"stepan", "Stepan"}, {"idiot", "Idiot"}, {"tokarev", "Tokarev"},
    {"damir", "Damir"}, {"alyosha", "Alyosha"}, {"alesha", "Alyosha"}, {"duke", "Duke"}, {"yermak", "Yermak"},
    {"nastya", "Nastya"}, {"olga", "Olga"}, {"giul", "Giul"}, {"baron", "Baron"}, {"sam_", "Sam"},
    {"silantiy", "Silantiy"}, {"tikhon", "Tikhon"}, {"oksana", "Oksana"}, {"krasnov", "Krasnov"},
};
struct ClassName
{
    const char* cls;
    const char* display;
};
const ClassName kClassNames[] = {
    {"NPC", "Human"}, {"HUMAN", "Human"}, {"SIMPLE_NPC", "Human"}, {"DUMMY_NPC", "Human"}, {"NPC_FX", "Human"},
    {"WOMAN", "Woman"}, {"WOMAN_COMBAT", "Woman"}, {"WOMAN_STRIP", "Woman"}, {"KID", "Child"},
    {"CANNIBAL", "Cannibal"}, {"MONSTER", "Mutant"}, {"SIMPLE_MONSTER", "Mutant"}, {"NOSALIS", "Nosalis"},
    {"NOSALIS_FEMALE", "Nosalis female"}, {"NOSACH", "Nosach"}, {"LURKER", "Lurker"}, {"WATCHMAN", "Watchman"},
    {"DOG", "Dog"}, {"BIG_MOTHER", "Big mother"}, {"ARAHIND", "Spider"}, {"AQUA_MALE_BIG", "Shrimp (big)"},
    {"AQUA_MALE_SMALL", "Shrimp"}, {"AQUA_FEMALE", "Shrimp queen"}, {"GRIZLY", "Bear"}, {"HUMANIMAL", "Humanimal"},
    {"LIBRARIAN", "Librarian"}, {"DARK", "Dark one"}, {"DARKCHILD", "Dark one (child)"}, {"AMEBA", "Ameba"},
    {"HARPY", "Demon"}, {"CATFISH", "Tsar fish"}, {"BLIND", "Blind one"}, {"DEER", "Deer"}, {"RABBIT", "Rabbit"},
    {"SNAKE", "Worm"}, {"RAT", "Rat"}, {"FROG", "Frog"}, {"WORM", "Worm"}, {"SEA_CUCUMBER", "Sea cucumber"},
    {"FLYING_CREATURE", "Bird"},
};

const char* display_name(const char* cls, Category cat, const char* entity_name)
{
    if (cat == Category::Human)
    {
        char low[48];
        size_t i = 0;
        for (; entity_name[i] && i + 1 < sizeof(low); ++i)
            low[i] = static_cast<char>(tolower(static_cast<unsigned char>(entity_name[i])));
        low[i] = 0;
        for (const auto& k : kCharacters)
            if (strstr(low, k.key))
                return k.display;
    }
    for (const auto& n : kClassNames)
        if (!strcmp(n.cls, cls))
            return n.display;
    return cls;
}

bool finite3(const float* p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && fabsf(p[0]) < 1e5f &&
           fabsf(p[1]) < 1e5f && fabsf(p[2]) < 1e5f;
}

// ---- detection: resolve (optional feature; markers work without it) ----

// Exactly one distinct value from all matches of `sig` in [start, start + len), read by `get`; 0 matches -> false.
template <class T, class F>
bool agree_in(const uint8_t* start, size_t len, const char* sig, F get, T& out, const char* what)
{
    auto hits = sig_find_in(start, len, sig);
    if (hits.empty())
    {
        logf("detection: %s not found", what);
        return false;
    }
    out = get(hits[0]);
    for (auto h : hits)
        if (get(h) != out)
        {
            logf("detection: %s matches disagree", what);
            return false;
        }
    return true;
}

void detect_init(const GameModule& m)
{
    const uint8_t* T = m.text;
    const size_t TN = m.text_size;
    auto u8at = [](int o) { return [o](uint8_t* p) { return p[o]; }; };
    auto i32at = [](int o) { return [o](uint8_t* p) { return static_cast<uint32_t>(rd_i32(p + o)); }; };
    // RIP-relative target of the instruction at p + insn (disp32 at p + disp, length len)
    auto ripat = [](int insn, int disp, int len) {
        return [=](uint8_t* p) { return reinterpret_cast<uintptr_t>(p + insn + len + rd_i32(p + disp)); };
    };
    auto ptrat = [](int o) { return [o](uint8_t* p) { return p + o; }; };

    // vision object: layout anchors + class vtable
    uint8_t* fn = nullptr;
    uint8_t rng = 0, kedge = 0, per = 0;
    uint32_t pos = 0, list = 0, count = 0;
    bool ok = agree_in(T, TN, SIG_VIS_FORMULA, ptrat(0), fn, "vision formula") &&
              agree_in(fn, 0x100, SIG_VIS_FORMULA, u8at(15), rng, "vision range") &&
              agree_in(fn, 0x100, SIG_VIS_KEDGE, u8at(5), kedge, "vision edge factor") &&
              agree_in(fn, 0x100, SIG_VIS_PERIOD, u8at(2), per, "vision period") &&
              agree_in(fn, 0x100, SIG_VIS_POS, i32at(3), pos, "vision owner position") &&
              agree_in(T, TN, SIG_VIS_LIST, i32at(3), list, "vision list") &&
              agree_in(T, TN, SIG_VIS_LIST, i32at(11), count, "vision list count") &&
              agree_in(T, TN, SIG_VIS_VTBL, ripat(3, 6, 7), D.vis_vtbl, "vision vtable");
    if (ok && (rng != VO::range || kedge != VO::k_edge || per != VO::period || list != VO::list || count != VO::count ||
               pos != kXformOff + 0x30))
    {
        logf("detection: vision layout changed (range +0x%X, edge +0x%X, period +0x%X, list +0x%X/+0x%X, pos +0x%X); "
             "exact detection off", rng, kedge, per, list, count, pos);
        ok = false;
    }
    D.vis_ok = ok;

    // player visibility (light, flash, stance, speed terms)
    uintptr_t env_fn = 0;  // two identical copies of the function: they must agree on everything, incl. the call
    uint32_t params = 0;
    D.pv_ok = agree_in(T, TN, SIG_PLAYER_VIS, ripat(72, 73, 5), env_fn, "player visibility") &&
              agree_in(T, TN, SIG_PLAYER_VIS, i32at(4), D.lum, "player light") &&
              agree_in(T, TN, SIG_PLAYER_VIS, i32at(20), D.flash_until, "player flash time") &&
              agree_in(T, TN, SIG_PLAYER_VIS, i32at(34), D.lum_k, "player light factor") &&
              agree_in(T, TN, SIG_PLAYER_VIS, i32at(48), params, "player params") &&
              agree_in(T, TN, SIG_PLAYER_VIS, i32at(56), D.flash, "player flash") &&
              agree_in(T, TN, SIG_PLAYER_VIS, i32at(68), D.stance, "player stance") &&
              agree_in(T, TN, SIG_PLAYER_SPEED_MIN, i32at(3), D.speed_min, "player speed min") &&
              agree_in(T, TN, SIG_PLAYER_SPEED_K, i32at(4), D.speed_k, "player speed factor") &&
              agree_in(T, TN, SIG_PLAYER_SPEED_K, i32at(17), D.speed_base, "player speed base");
    if (D.pv_ok && params != R.params_off)
    {
        logf("detection: player params offset 0x%X != 0x%X; visibility factors off", params, R.params_off);
        D.pv_ok = false;
    }

    // environment factor: crouch state and the vegetation density it uses
    const uint8_t* env = D.pv_ok ? reinterpret_cast<const uint8_t*>(env_fn) : nullptr;
    uintptr_t veg_fn = 0;
    uint32_t env_pos = 0;
    if (env && env >= T && env < T + TN - 0x140)
    {
        D.env_ok = agree_in(env, 0x40, SIG_ENV_A, i32at(2), D.env_flag, "env flag") &&
                   agree_in(env, 0x40, SIG_ENV_A, i32at(19), D.env_anim, "env movement state") &&
                   agree_in(env, 0x80, SIG_ENV_B, u8at(3), D.env_anim2, "env state 2") &&
                   agree_in(env, 0x80, SIG_ENV_B, u8at(6), D.env_state, "env state word") &&
                   agree_in(env, 0x80, SIG_ENV_B, u8at(12), D.env_shift, "env state shift") &&
                   agree_in(env, 0x80, SIG_ENV_B, u8at(16), D.env_mask, "env state mask") &&
                   agree_in(env, 0x80, SIG_ENV_CMP, u8at(5), D.env_crouch, "env crouch value") &&
                   agree_in(env, 0x100, SIG_ENV_C, ripat(0, 3, 7), D.world_global, "env world") &&
                   agree_in(env, 0x100, SIG_ENV_C, ripat(7, 10, 7), D.world_vtbl, "env world class") &&
                   agree_in(env, 0x100, SIG_ENV_C, i32at(24), env_pos, "env position") &&
                   agree_in(env, 0x100, SIG_ENV_C, i32at(35), D.env_radius, "env radius") &&
                   agree_in(env, 0x100, SIG_ENV_C, i32at(47), D.world_veg, "env vegetation") &&
                   agree_in(env, 0x100, SIG_ENV_C, ripat(51, 52, 5), veg_fn, "env vegetation function") &&
                   agree_in(env, 0x100, SIG_ENV_C, i32at(63), D.env_k, "env k");
        if (D.env_ok && env_pos != kXformOff + 0x30)
            D.env_ok = false;
    }
    const uint8_t* veg = reinterpret_cast<const uint8_t*>(veg_fn);
    if (D.env_ok && veg >= T && veg < T + TN - 0x500)
    {
        uintptr_t maxr = 0;
        D.veg_ok = agree_in(veg, 0x500, SIG_VEG_COUNT, i32at(4), D.veg_count, "veg count") &&
                   agree_in(veg, 0x500, SIG_VEG_MAXR, ripat(0, 4, 8), maxr, "veg max radius") &&
                   agree_in(veg, 0x500, SIG_VEG_OX, i32at(3), D.veg_ox, "veg origin x") &&
                   agree_in(veg, 0x500, SIG_VEG_OZ, i32at(2), D.veg_oz, "veg origin z") &&
                   agree_in(veg, 0x500, SIG_VEG_CELLS, i32at(4), D.veg_cells, "veg cells") &&
                   agree_in(veg, 0x500, SIG_VEG_CELLS, u8at(12), D.cell_flags, "veg cell flags") &&
                   agree_in(veg, 0x500, SIG_VEG_CELLN, u8at(4), D.cell_n, "veg cell count") &&
                   agree_in(veg, 0x500, SIG_VEG_INST, u8at(3), D.inst_type, "veg instance type") &&
                   agree_in(veg, 0x500, SIG_VEG_INST, u8at(8), D.inst_scale, "veg instance scale") &&
                   agree_in(veg, 0x500, SIG_VEG_MODELS, i32at(4), D.veg_types, "veg types") &&
                   agree_in(veg, 0x500, SIG_VEG_MODELS, i32at(16), D.veg_models, "veg models") &&
                   agree_in(veg, 0x500, SIG_VEG_BBOX, u8at(3), D.bbox_a, "veg bbox a") &&
                   agree_in(veg, 0x500, SIG_VEG_BBOX, u8at(7), D.bbox_b, "veg bbox b") &&
                   agree_in(veg, 0x500, SIG_VEG_DENS, i32at(4), D.veg_dens, "veg density table") &&
                   agree_in(veg, 0x500, SIG_VEG_DENS, u8at(34), D.dens_p, "veg density ptr") &&
                   agree_in(veg, 0x500, SIG_VEG_DENS, u8at(39), D.dens_f, "veg density field");
        if (D.veg_ok)
            memcpy(&D.veg_maxr, reinterpret_cast<const void*>(maxr), 4);  // .rdata constant of our own process
    }
    logf("detection: vision %s (vtable +%llX, brain +0x%X), player factors %s (light +0x%X x +0x%X, stance +0x%X, "
         "flash +0x%X), crouch %s (state nibble == %u), vegetation %s (max radius %.2f)",
         D.vis_ok ? "ok" : "OFF", static_cast<unsigned long long>(D.vis_vtbl - reinterpret_cast<uintptr_t>(m.base)),
         D.brain_vis_off, D.pv_ok ? "ok" : "OFF", D.lum, D.lum_k, D.stance, D.flash, D.env_ok ? "ok" : "OFF",
         D.env_crouch, D.veg_ok ? "ok" : "OFF", D.veg_maxr);
}

// ---- detection: per frame ----

ULONGLONG g_last_vis_scan = 0;

bool vision_object(uintptr_t brain, uintptr_t e, uintptr_t& vis)
{
    uintptr_t vt = 0, sub = 0, owner = 0;
    return rdv(brain + D.brain_vis_off, vis) && vis && rdv(vis, vt) && vt == D.vis_vtbl &&
           rdv(vis + VO::owner, sub) && sub && rdv(sub + 8, owner) && owner == e;
}

// A patch could move the vision feel inside the brain: look for the vtable once in a while and adopt the new slot.
void rescan_vision_slot(uintptr_t brain, uintptr_t e)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_last_vis_scan < 2000)
        return;
    g_last_vis_scan = now;
    uintptr_t slots[0x80];
    if (!rd(brain + 0x40, slots, sizeof(slots)))
        return;
    for (int i = 0; i < 0x80; ++i)
    {
        uintptr_t vt = 0, sub = 0, owner = 0;
        if (slots[i] && rdv(slots[i], vt) && vt == D.vis_vtbl && rdv(slots[i] + VO::owner, sub) && sub &&
            rdv(sub + 8, owner) && owner == e)
        {
            uint32_t off = 0x40 + 8u * static_cast<uint32_t>(i);
            if (off != D.brain_vis_off)
            {
                logf("detection: vision object found at brain +0x%X (was +0x%X)", off, D.brain_vis_off);
                D.brain_vis_off = off;
            }
            return;
        }
    }
}

bool read_vision(uintptr_t e, uintptr_t brain, uintptr_t player, uint32_t gt, Vision& v)
{
    v = Vision{};
    uintptr_t vis = 0;
    if (!vision_object(brain, e, vis))
    {
        rescan_vision_slot(brain, e);
        if (!vision_object(brain, e, vis))
            return false;
    }
    uint8_t raw[VO::block_size];
    if (!rd(vis + VO::block, raw, sizeof(raw)))
        return false;
    auto F = [&](uint32_t o) {
        float f;
        memcpy(&f, raw + (o - VO::block), 4);
        return f;
    };
    auto U = [&](uint32_t o) {
        uint32_t u;
        memcpy(&u, raw + (o - VO::block), 4);
        return u;
    };
    v.surv = U(VO::surv) != 0;
    v.range = F(VO::range);
    v.back_range = F(VO::back);
    v.fov = F(VO::fov);
    v.k_edge = F(VO::k_edge);
    v.k_centre = F(VO::k_centre);
    v.min_dist = F(VO::min_dist);
    v.period = U(VO::period);
    v.lum_offset = F(VO::lum_off);
    v.vel_k = F(VO::vel_k);
    v.gain = F(VO::gain);
    v.min_lum = F(VO::min_lum);
    v.far_sub = F(VO::far_sub);
    v.force_range = F(VO::force);
    v.peri_dist = F(VO::peri_dist);
    v.peri_angle = F(VO::peri_angle);
    v.noticed = v.near_dist = F(v.surv ? VO::s_noticed : VO::noticed);
    v.light = F(v.surv ? VO::s_light : VO::light);
    v.seen = F(v.surv ? VO::s_seen : VO::seen);
    const float chk[] = {v.range, v.back_range, v.fov, v.k_edge, v.k_centre, v.min_dist, v.gain, v.noticed, v.light, v.seen};
    for (float f : chk)
        if (!std::isfinite(f) || f < 0.f || f > 1e6f)
            return false;
    if (v.range <= 0.f || v.fov <= 0.f || v.fov > 6.3f || v.seen <= 0.f || v.period == 0)
        return false;
    uintptr_t arr = 0;
    uint16_t n = 0;
    memcpy(&arr, raw + (VO::list - VO::block), 8);
    memcpy(&n, raw + (VO::count - VO::block), 2);
    if (n && arr)
    {
        struct Entry
        {
            uintptr_t link;
            float value;
            uint8_t updated, cam, pad[2];
        } ents[64];
        uint16_t k = n < 64 ? n : 64;
        if (rd(arr, ents, sizeof(Entry) * k))
            for (uint16_t i = 0; i < k; ++i)
            {
                uintptr_t iface = 0;
                if (ents[i].link && rdv(ents[i].link, iface) && iface && iface - R.link_off == player &&
                    std::isfinite(ents[i].value))
                {
                    v.has_entry = true;
                    v.value = ents[i].value;
                    break;
                }
            }
    }
    v.ok = true;
    RateState& rs = g_rates[e];  // observed change per second, smoothed over ~0.25 s of game time
    if (gt && rs.t && gt > rs.t && gt - rs.t < 1000)
    {
        float dt = (gt - rs.t) / 1000.f, inst = (v.value - rs.value) / dt, k = fminf(1.f, dt / 0.25f);
        rs.rate += (inst - rs.rate) * k;
    }
    else if (!gt || gt != rs.t)
        rs.rate = 0.f;
    if (gt != rs.t)
    {
        rs.value = v.value;
        rs.t = gt;
    }
    v.rate = rs.rate;
    return true;
}

// Vegetation density around a point, as the engine's fn 0x140D8A470 computes it (read-only replica: the engine takes a
// lock first, we only read with guarded reads; a torn read shows as a one-frame glitch at worst).
bool veg_density(uintptr_t vw, const float p[3], float r, float& out)
{
    out = 0.f;
    uint16_t cnt = 0;
    int32_t ox = 0, oz = 0;
    if (!rdv(vw + D.veg_count, cnt) || !rdv(vw + D.veg_ox, ox) || !rdv(vw + D.veg_oz, oz))
        return false;
    if (!cnt)
        return true;
    if (r > D.veg_maxr)
        r = D.veg_maxr;
    uintptr_t cells[9];
    int nc = 0;
    for (int ix = -1; ix <= 1; ++ix)
    {
        int xi = static_cast<int>(floorf((p[0] + ix * r) * 0.5f)) - ox;
        if (xi + 0x30 < 0 || xi + 0x30 >= 0x60)
            continue;
        for (int iz = -1; iz <= 1; ++iz)
        {
            int zi = static_cast<int>(floorf((p[2] + iz * r) * 0.5f)) - oz + 0x30;
            if (zi < 0 || zi >= 0x60)
                continue;
            uintptr_t c = 0;
            uint8_t fl = 0;
            int64_t idx = static_cast<int64_t>(0x5F - zi) * 0x60 + xi;
            if (!rdv(vw + D.veg_cells + idx * 8, c) || !c || !rdv(c + D.cell_flags, fl) || (fl & 3))
                continue;
            bool dup = false;
            for (int k = 0; k < nc; ++k)
                dup = dup || cells[k] == c;
            if (!dup && nc < 9)
                cells[nc++] = c;
        }
    }
    float f = 0.f;
    for (int k = 0; k < nc; ++k)
    {
        uint16_t n = 0;
        uintptr_t arr = 0;
        if (!rdv(cells[k] + D.cell_n, n) || !rdv(cells[k], arr) || !arr || n > 4096)
            continue;
        for (uint16_t i = 0; i < n; ++i)
        {
            uintptr_t ins = 0, mdl = 0, dt = 0, dp = 0;
            float q[4], a[4], b[4], dens = 0.f;
            uint16_t type = 0, mi = 0;
            if (!rdv(arr + 8ull * i, ins) || !ins || !rd(ins, q, 12) || !rdv(ins + D.inst_scale, q[3]) ||
                !rdv(ins + D.inst_type, type))
                continue;
            float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2], d2 = dx * dx + dy * dy + dz * dz;
            if (!rdv(vw + D.veg_types + 16ull * type, mi) || !rdv(vw + D.veg_models + 0x28ull * mi, mdl) || !mdl ||
                !rd(mdl + D.bbox_b, a, 16) || !rd(mdl + D.bbox_a, b, 16))
                continue;
            float sx = fmaxf(a[0], b[0]) - fminf(a[0], b[0]), sz = fmaxf(a[2], b[2]) - fminf(a[2], b[2]);
            float size = sqrtf(sx * sx + sz * sz) * q[3] * 0.5f + r;
            size *= size;
            if (d2 > size || !rdv(vw + D.veg_dens + 16ull * type, dt) || !dt || !rdv(dt + D.dens_p, dp) || !dp ||
                !rdv(dp + D.dens_f, dens) || !std::isfinite(dens))
                continue;
            float t = fminf(1.f, fmaxf(0.f, d2 / size));
            f += q[3] * dens * (1.f - t);
        }
    }
    out = std::isfinite(f) ? f : 0.f;
    return true;
}

void read_player_vis(uintptr_t player, uint32_t gt, const float pos[3], PlayerVisibility& pv)
{
    pv = PlayerVisibility{};
    if (!D.pv_ok)
        return;
    uintptr_t params = 0;
    uint32_t until = 0;
    float light = 0, lk = 0, stance = 0, hp = 0;
    if (!rdv(player + D.lum, light) || !rdv(player + D.lum_k, lk) || !rdv(player + D.stance, stance) ||
        !rdv(player + D.flash_until, until) || !rdv(player + R.params_off, params) || !params ||
        !rdv(params + D.speed_min, pv.speed_min) || !rdv(params + D.speed_k, pv.speed_k) ||
        !rdv(params + D.speed_base, pv.speed_base))
        return;
    pv.light = light * lk;
    if (gt < until)
        rdv(params + D.flash, pv.flash);
    pv.stance = stance;
    rdv(player + R.health_off, hp);

    // speed from the position over game time (the engine reads the velocity vector; same thing while walking)
    static float last[3] = {0, 0, 0}, speed = 0.f;
    static uint32_t last_t = 0;
    static uintptr_t last_player = 0;
    if (player != last_player || !last_t || gt < last_t || gt - last_t > 1000)
    {
        memcpy(last, pos, sizeof(last));
        last_t = gt;
        last_player = player;
        speed = 0.f;
    }
    else if (gt - last_t >= 50)
    {
        float dx = pos[0] - last[0], dy = pos[1] - last[1], dz = pos[2] - last[2];
        float inst = sqrtf(dx * dx + dy * dy + dz * dz) / ((gt - last_t) / 1000.f);
        speed += (inst - speed) * 0.5f;
        memcpy(last, pos, sizeof(last));
        last_t = gt;
    }
    pv.speed = speed;

    // environment factor: 1 unless crouched (or dead); then k / (k + vegetation density)
    pv.env = 1.f;
    pv.env_mode = 0;
    uint32_t flag = 0;
    uintptr_t a1 = 0, a2 = 0;
    uint32_t st = 0;
    if (D.env_ok && rdv(player + D.env_flag, flag) && rdv(player + D.env_anim, a1) && a1 && rdv(a1 + D.env_anim2, a2) &&
        a2 && rdv(a2 + D.env_state, st))
    {
        pv.crouched = ((st >> D.env_shift) & D.env_mask) == D.env_crouch;
        if (flag && (pv.crouched || hp <= 0.f))
        {
            pv.env_mode = 2;
            uintptr_t world = 0, wvt = 0, vw = 0;
            float radius = 0, k = 0, f = 0;
            if (D.veg_ok && rdv(D.world_global, world) && world && rdv(world, wvt) && wvt == D.world_vtbl &&
                rdv(world + D.world_veg, vw) && vw && rdv(params + D.env_radius, radius) && rdv(params + D.env_k, k) &&
                k > 0.f && veg_density(vw, pos, radius, f))
            {
                pv.env_mode = 1;
                pv.cover = f;
                pv.env = k / (k + f);
            }
        }
    }
    const float spd = pv.speed > pv.speed_min ? pv.speed * pv.speed_k : 0.f;
    pv.visibility = (spd + pv.speed_base) * pv.env * fmaxf(pv.light, pv.flash) * pv.stance;
    pv.ok = std::isfinite(pv.visibility);
}
// ---- routes and AI task (re/STRUCTS.md §7) ----

struct RouteLayout
{
    bool ok = false;       // motor + path
    bool task_ok = false;  // current task, getter slots, part table
    uint32_t motor = 0, goal = 0, count = 0, points = 0, stride = 0, flags = 0;
    uint32_t look = 0, look_flags = 0, look_mask = 0, obj_dir = 0, dir = 0;
    uint32_t task = 0, part_slot = 0, scr_slot = 0, type_slot = 0;
    uint32_t t_scr = 0, t_idx = 0, s_n = 0, s_arr = 0, s_name = 0;
    const char* part_names[256] = {};
    int nparts = 0;
    uintptr_t mod = 0, mod_end = 0, text = 0, text_end = 0;
} RL;

struct TaskInfo
{
    const char* type;  // static string in the exe, "" unknown
    bool parts;        // the part getter is the scripted one (decoded offsets match)
    bool scr;          // the script name getter is the scripted one
};
std::unordered_map<uintptr_t, TaskInfo> g_task_info;  // by task vtable (render thread only)

// Exactly one match of `sig`, else logs and returns nullptr.
const uint8_t* find_one(const GameModule& m, const char* sig, const char* what)
{
    auto hits = sig_find_all(m, sig);
    if (hits.size() != 1)
    {
        logf("routes: %s %s", what, hits.empty() ? "not found" : "matched more than once");
        return nullptr;
    }
    return hits[0];
}

void route_init(const GameModule& m)
{
    RL = RouteLayout{};
    RL.mod = reinterpret_cast<uintptr_t>(m.base);
    RL.mod_end = RL.mod + m.size;
    RL.text = reinterpret_cast<uintptr_t>(m.text);
    RL.text_end = RL.text + m.text_size;
    const uint8_t* call = find_one(m, SIG_MOTOR_CALL, "motor call");
    const uint8_t* upd = find_one(m, SIG_MOTOR_UPDATE, "motor update");
    const uint8_t* path = find_one(m, SIG_PATH, "path layout");
    const uint8_t* end = find_one(m, SIG_END_DIR, "end facing");
    if (call && upd && path && end && rip_target(const_cast<uint8_t*>(call) + 10, 1, 5) == upd &&
        path[9] == path[12] && path[28] < 0x70)
    {
        RL.motor = static_cast<uint32_t>(rd_i32(call + 6));
        RL.goal = upd[39];
        RL.count = static_cast<uint32_t>(rd_i32(path + 3));
        RL.stride = path[9];
        RL.points = static_cast<uint32_t>(rd_i32(path + 16));
        RL.flags = path[28];
        RL.look = static_cast<uint32_t>(rd_i32(end + 3));
        RL.look_flags = static_cast<uint32_t>(rd_i32(end + 22));
        RL.look_mask = end[26];
        RL.obj_dir = end[35];
        RL.dir = static_cast<uint32_t>(rd_i32(end + 53));
        RL.ok = RL.motor > 0 && RL.motor < 0x4000 && RL.stride >= 16 && RL.points < 0x1000 && RL.count < 0x1000;
    }
    else if (call && upd)
        logf("routes: the motor call does not reach the motor update (or the path layout is off)");
    const uint8_t* task = find_one(m, SIG_TASK, "task");
    const uint8_t* slots = find_one(m, SIG_TASK_SLOTS, "task getters");
    const uint8_t* getter = find_one(m, SIG_PART_GETTER, "part getter");
    const uint8_t* sgetter = find_one(m, SIG_SCR_GETTER, "script getter");
    if (task && slots && getter && sgetter && getter[3] == sgetter[3])
    {
        RL.task = static_cast<uint32_t>(rd_i32(task + 3));
        RL.part_slot = static_cast<uint32_t>(rd_i32(slots + 8));
        RL.scr_slot = static_cast<uint32_t>(rd_i32(slots + 24));
        RL.type_slot = static_cast<uint32_t>(rd_i32(slots + 40));
        RL.t_scr = getter[3];
        RL.s_n = getter[13];
        RL.t_idx = getter[16];
        RL.s_arr = getter[28];
        RL.s_name = sgetter[11];
        // Part name table: 16-byte entries {char* name, ...}; the names that start with "part_" are the parts.
        const uintptr_t table = reinterpret_cast<uintptr_t>(rip_target(const_cast<uint8_t*>(getter) + 36, 3, 7));
        for (int i = 0; i < 256; ++i)
        {
            uintptr_t p = 0;
            char head[6] = {};
            if (!rdv(table + 16ull * i, p) || p < RL.mod || p >= RL.mod_end || !rd(p, head, 5) || memcmp(head, "part_", 5))
                break;
            RL.part_names[i] = reinterpret_cast<const char*>(p);
            RL.nparts = i + 1;
        }
        RL.task_ok = RL.nparts > 100 && RL.task < 0x4000 && RL.part_slot < 0x800 && RL.scr_slot < 0x800 && RL.type_slot < 0x800;
    }
    logf("routes: %s (motor brain+0x%X, goal +0x%X, points +0x%X x %u, count +0x%X, final flag +0x%X, end facing +0x%X / "
         "[+0x%X]+0x%X if [+0x%X]&0x%X); task: %s (brain+0x%X, getters +0x%X/+0x%X/+0x%X, %d parts)",
         RL.ok ? "ok" : "OFF", RL.motor, RL.goal, RL.points, RL.stride, RL.count, RL.flags, RL.dir, RL.look, RL.obj_dir,
         RL.look_flags, RL.look_mask, RL.task_ok ? "ok" : "OFF", RL.task, RL.part_slot, RL.scr_slot, RL.type_slot,
         RL.nparts);
}

bool in_text(uintptr_t f, size_t n) { return f >= RL.text && f + n <= RL.text_end; }

// What a task's vtable offers, decided from the getters' own code (never called): the type name getter is
// `lea rax,[rip+name]; ret`; the scripted part / script getters match the patterns with the resolved offsets.
TaskInfo task_info(uintptr_t vt)
{
    auto it = g_task_info.find(vt);
    if (it != g_task_info.end())
        return it->second;
    if (g_task_info.size() > 512)
        g_task_info.clear();
    TaskInfo ti{"", false, false};
    uintptr_t fn = 0;
    uint8_t b[8];
    if (rdv(vt + RL.type_slot, fn) && in_text(fn, 8) && rd(fn, b, 8) && b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x05 &&
        b[7] == 0xC3)
    {
        const uintptr_t s = fn + 7 + static_cast<intptr_t>(rd_i32(b + 3));
        char probe[48];
        if (s >= RL.mod && s + sizeof(probe) <= RL.mod_end && rd(s, probe, sizeof(probe)) && memchr(probe, 0, sizeof(probe)))
            ti.type = reinterpret_cast<const char*>(s);
    }
    if (rdv(vt + RL.part_slot, fn) && in_text(fn, kPartGetterLen))
    {
        const uint8_t* f = reinterpret_cast<const uint8_t*>(fn);
        auto h = sig_find_in(f, kPartGetterLen, SIG_PART_GETTER);
        ti.parts = !h.empty() && h[0] == f && f[3] == RL.t_scr && f[13] == RL.s_n && f[16] == RL.t_idx && f[28] == RL.s_arr;
    }
    if (rdv(vt + RL.scr_slot, fn) && in_text(fn, 16))
    {
        const uint8_t* f = reinterpret_cast<const uint8_t*>(fn);
        auto h = sig_find_in(f, 16, SIG_SCR_GETTER);
        ti.scr = !h.empty() && h[0] == f && f[3] == RL.t_scr && f[11] == RL.s_name;
    }
    g_task_info[vt] = ti;
    return ti;
}

// The motor's path (stack in the engine: [count-1] = next waypoint ... [0] = destination; returned in walking order),
// the facing at the end, and the brain's task with its script parts. Guarded reads only.
void read_route(uintptr_t e, uintptr_t brain, const float npc_pos[3], Route& r)
{
    r.ok = r.moving = r.final_pt = r.has_dir = false;
    r.n = r.total = 0;
    r.goal_kind = 0;
    r.task = "";
    r.script[0] = 0;
    r.nparts = r.nprev = 0;
    if (!RL.ok)
        return;
    const uintptr_t m = brain + RL.motor;
    uintptr_t back = 0, ent = 0;
    if (!rdv(m, back) || back != brain || !rdv(m + 8, ent) || ent != e)
        return;
    r.ok = true;
    rdv(m + RL.goal, r.goal_kind);
    uintptr_t arr = 0;
    uint16_t cnt = 0, cap = 0;
    if (r.goal_kind && rdv(m + RL.points, arr) && arr && rdv(m + RL.count, cnt) && rdv(m + RL.count - 2, cap) && cnt &&
        cnt <= cap && cnt < 4096)
    {
        const int take = cnt < kRoutePts ? cnt : kRoutePts;
        bool good = true;
        for (int k = 0; k < take && good; ++k)
        {
            // walking order; when capped, an even pick that keeps the next waypoint and the destination
            const int idx = take == 1 ? 0 : cnt - 1 - static_cast<int>(static_cast<int64_t>(k) * (cnt - 1) / (take - 1));
            float* p = r.pts[k];
            good = rd(arr + static_cast<uint64_t>(idx) * RL.stride, p, 12) && finite3(p) &&
                   fabsf(p[0] - npc_pos[0]) < 1000.f && fabsf(p[1] - npc_pos[1]) < 1000.f && fabsf(p[2] - npc_pos[2]) < 1000.f;
        }
        uint8_t fl = 0;
        if (good && rdv(arr + RL.flags, fl))
        {
            r.n = take;
            r.total = cnt;
            r.final_pt = (fl & 1) != 0;
            r.moving = true;
        }
    }
    if (r.moving)
    {
        uint32_t lf = 0;
        uintptr_t link = 0, obj = 0;
        float d[3];
        const bool use_obj = rdv(m + RL.look_flags, lf) && (lf & RL.look_mask) && rdv(m + RL.look, link) && link &&
                             rdv(link, obj) && obj;
        if ((use_obj ? rd(obj + RL.obj_dir, d, 12) : rd(m + RL.dir, d, 12)) && finite3(d) && d[0] * d[0] + d[2] * d[2] > 0.01f)
        {
            memcpy(r.dir, d, sizeof(d));
            r.has_dir = true;
        }
    }
    if (!RL.task_ok)
        return;
    uintptr_t task = 0, vt = 0;
    if (!rdv(brain + RL.task, task) || !task || !rdv(task, vt) || !vt)
        return;
    const TaskInfo ti = task_info(vt);
    r.task = ti.type;
    uintptr_t scr = 0;
    if (!(ti.parts || ti.scr) || !rdv(task + RL.t_scr, scr) || !scr)
        return;
    uint32_t h = 0;
    if (ti.scr && rdv(scr + RL.s_name, h))
        read_string(h, r.script, sizeof(r.script));
    uint16_t n = 0;
    uintptr_t parts = 0;
    uint32_t idx = 0;
    if (!ti.parts || !rdv(scr + RL.s_n, n) || !rdv(scr + RL.s_arr, parts) || !parts || !rdv(task + RL.t_idx, idx) ||
        idx >= n || n > 1024)
        return;
    for (uint32_t i = idx; i < n && r.nparts < kRouteParts; ++i)
        if (!rdv(parts + static_cast<uint64_t>(i) * kPartStride, r.parts[r.nparts++]))
        {
            --r.nparts;
            break;
        }
    for (uint32_t k = 1; k <= 3 && k <= idx; ++k)
        if (!rdv(parts + static_cast<uint64_t>(idx - k) * kPartStride, r.prev[r.nprev++]))
        {
            --r.nprev;
            break;
        }
}
}  // namespace

static std::vector<std::string> g_offline_parts;  // part names from a captured frame (offline preview only)

void part_names_offline(const std::vector<std::string>& names) { g_offline_parts = names; }

const char* part_name(uint8_t type)
{
    if (type < RL.nparts && RL.part_names[type])
        return RL.part_names[type];
    return type < g_offline_parts.size() ? g_offline_parts[type].c_str() : "";
}

VisionPrediction vision_predict(const Npc& n, const PlayerVisibility& pv, const float pp[3])
{
    VisionPrediction r{};
    const Vision& v = n.vis;
    if (!v.ok || !pv.ok)
        return r;
    float dx = pp[0] - n.pos[0], dy = pp[1] - n.pos[1], dz = pp[2] - n.pos[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    float fl = sqrtf(n.fwd[0] * n.fwd[0] + n.fwd[1] * n.fwd[1] + n.fwd[2] * n.fwd[2]);
    if (d < 1e-3f || fl < 1e-4f)
        return r;
    float c = (dx * n.fwd[0] + dy * n.fwd[1] + dz * n.fwd[2]) / (d * fl);
    r.angle = acosf(fminf(1.f, fmaxf(-1.f, c)));
    const float half = v.fov * 0.5f;
    const float rc = fmaxf(v.range * v.k_centre, 0.001f), re = v.range * v.k_edge;
    const float a = fminf(r.angle, half);
    r.r_eff = (1.f - a / half) * (rc - re) + re;
    r.in_cone = r.angle <= half && d < r.r_eff;
    r.in_back = d <= v.back_range;
    r.in_force = d < v.force_range && (r.in_cone || r.in_back);
    r.light_term = fmaxf(pv.visibility - v.min_lum, d < v.near_dist ? kNearFloor : 0.f) + v.lum_offset;
    r.speed_mult = 1.f + pv.speed * v.vel_k;
    if (d >= r.r_eff)
        return r;
    r.falloff = logf(r.r_eff / d) / fmaxf(logf(r.r_eff / fmaxf(v.min_dist, 1e-3f)), 0.001f);
    float g = r.light_term * v.gain * r.speed_mult * r.falloff;
    if (d >= v.near_dist)
        g -= v.far_sub;
    r.per_sec = fmaxf(g, 0.f) * 1000.f / static_cast<float>(v.period);
    return r;
}

GameInit game_init(char* err, size_t errlen)
{
    GameModule m;
    if (!game_module(m))
    {
        snprintf(err, errlen, "MetroExodus.exe module/.text not found");
        return GameInit::WrongBuild;
    }
    if (m.timestamp != kBuildTimestamp)
    {
        snprintf(err, errlen, "build mismatch: PE timestamp %u, expected %u (Steam build 24973569)", m.timestamp,
                 kBuildTimestamp);
        return GameInit::WrongBuild;
    }
    auto one = [&](const char* sig, const char* what) -> uint8_t* {
        auto h = sig_find_all(m, sig);
        if (h.size() != 1)
        {
            snprintf(err, errlen, "%s: %zu matches (expected 1)", what, h.size());
            return nullptr;
        }
        return h[0];
    };
    uint8_t* p;
    if (!(p = one(SIG_LEVEL_PLAYER, "SIG_LEVEL_PLAYER")))
        return GameInit::MissingData;
    R.level_global = reinterpret_cast<uintptr_t>(rip_target(p, 3, 7));
    R.player_off = p[15];
    if (!(p = one(SIG_STATS, "SIG_STATS")))
        return GameInit::MissingData;
    R.health_off = static_cast<uint32_t>(rd_i32(p + 19));
    if (!(p = one(SIG_HEALTH_LOAD, "SIG_HEALTH_LOAD")))
        return GameInit::MissingData;
    uint8_t* lea = p - 0x1B;  // lea r8,["health"]
    if (lea[0] != 0x4C || lea[1] != 0x8D || lea[2] != 0x05 || static_cast<uint32_t>(rd_i32(p + 12)) != R.health_off ||
        memcmp(rip_target(lea, 3, 7), "health", 7) != 0)
    {
        snprintf(err, errlen, "health offset cross-check failed");
        return GameInit::MissingData;
    }
    if (!(p = one(SIG_PLAYER_VTBL, "SIG_PLAYER_VTBL")))
        return GameInit::MissingData;
    R.player_vtbl = reinterpret_cast<uintptr_t>(rip_target(p + 3, 3, 7));
    if (!(p = one(SIG_ENT_SERVER, "SIG_ENT_SERVER")))
        return GameInit::MissingData;
    R.entities_global = reinterpret_cast<uintptr_t>(rip_target(p + 8, 3, 7));
    {
        auto hits = sig_find_all(m, SIG_ENT_TABLE);
        if (hits.empty())
        {
            snprintf(err, errlen, "SIG_ENT_TABLE: no match");
            return GameInit::MissingData;
        }
        R.table_off = hits[0][3];
        R.link_off = static_cast<uint32_t>(rd_i32(hits[0] + 19));
        for (auto h : hits)
            if (h[3] != R.table_off || static_cast<uint32_t>(rd_i32(h + 19)) != R.link_off)
            {
                snprintf(err, errlen, "SIG_ENT_TABLE: matches disagree");
                return GameInit::MissingData;
            }
    }
    {
        auto hits = sig_find_all(m, SIG_NAME);
        if (hits.empty())
        {
            snprintf(err, errlen, "SIG_NAME: no match");
            return GameInit::MissingData;
        }
        R.name_off = static_cast<uint32_t>(rd_i32(hits[0] + 2));
        R.string_pool = reinterpret_cast<uintptr_t>(rip_target(hits[0] + 6, 3, 7));
        for (auto h : hits)
            if (static_cast<uint32_t>(rd_i32(h + 2)) != R.name_off ||
                reinterpret_cast<uintptr_t>(rip_target(h + 6, 3, 7)) != R.string_pool)
            {
                snprintf(err, errlen, "SIG_NAME: matches disagree");
                return GameInit::MissingData;
            }
    }
    if (!(p = one(SIG_CLSID, "SIG_CLSID")))
        return GameInit::MissingData;
    R.params_off = static_cast<uint32_t>(rd_i32(p + 3));
    {
        R.friend_off = 0;
        for (auto h : sig_find_all(m, SIG_FRIEND_TYPE))
        {
            uint8_t* key_lea = h - kFriendTypeKeyBack;  // lea r8,[key]
            if (key_lea[0] == 0x4C && key_lea[1] == 0x8D && key_lea[2] == 0x05 &&
                memcmp(rip_target(key_lea, 3, 7), "friend_type", 12) == 0)
            {
                if (R.friend_off)
                {
                    snprintf(err, errlen, "SIG_FRIEND_TYPE: more than one friend_type store");
                    return GameInit::MissingData;
                }
                R.friend_off = static_cast<uint32_t>(rd_i32(h + 10));
            }
        }
        if (!R.friend_off)
        {
            snprintf(err, errlen, "SIG_FRIEND_TYPE: not found");
            return GameInit::MissingData;
        }
    }
    {  // AI brain: optional, markers work without it
        auto b = sig_find_all(m, SIG_BRAIN), ms = sig_find_all(m, SIG_MENTAL), en = sig_find_all(m, SIG_BRAIN_ENEMY);
        if (b.size() == 1 && ms.size() == 1)
        {
            R.brain_off = static_cast<uint32_t>(rd_i32(b[0] + 12));
            R.mental_off = static_cast<uint32_t>(rd_i32(ms[0] + 4));
            if (en.size() == 1)
                R.enemy_off = static_cast<uint32_t>(rd_i32(en[0] + 3));
        }
        else
            logf("AI brain signatures: %zu/%zu matches; NPC state disabled", b.size(), ms.size());
        auto gt = sig_find_all(m, SIG_GAME_TIME);
        auto vs = sig_find_all(m, SIG_VIS_STRICT);
        if (gt.size() == 1)
            R.game_time = reinterpret_cast<uintptr_t>(rip_target(gt[0] + 8, 3, 7));
        if (vs.size() == 1)
        {
            uint8_t* key_lea = vs[0] - 7;
            if (key_lea[0] == 0x4C && key_lea[1] == 0x8D && key_lea[2] == 0x05 &&
                memcmp(rip_target(key_lea, 3, 7), "_last_enemy_visible_strict_time", 32) == 0)
                R.vis_strict_off = static_cast<uint32_t>(rd_i32(vs[0] + 12));
        }
        if (!R.game_time || !R.vis_strict_off)
            logf("line-of-sight signatures: time %s, strict sighting %s", R.game_time ? "ok" : "missing",
                 R.vis_strict_off ? "ok" : "missing");
    }
    if (!(p = one(SIG_CAM_PAIR, "SIG_CAM_PAIR")))
        return GameInit::MissingData;
    uintptr_t cam_main = reinterpret_cast<uintptr_t>(rip_target(p, 3, 7));
    R.cam_unjit = reinterpret_cast<uintptr_t>(rip_target(p + 12, 3, 7));
    if (R.cam_unjit - cam_main != kCamSize)
    {
        snprintf(err, errlen, "camera pair signature inconsistent");
        return GameInit::MissingData;
    }
    {  // map state: without it the markers would show over loading screens, so a disagreement disables the overlay
        auto rd_hits = sig_find_all(m, SIG_MAP_STATE_READ), run_hits = sig_find_all(m, SIG_MAP_STATE_RUN);
        uintptr_t st = 0;
        bool agree = !rd_hits.empty() && !run_hits.empty();
        for (auto h : rd_hits)
        {
            uintptr_t a = reinterpret_cast<uintptr_t>(h) + 6 + rd_i32(h + 2);
            agree = agree && (!st || a == st);
            st = a;
        }
        for (auto h : run_hits)
        {
            uintptr_t a = reinterpret_cast<uintptr_t>(h) + 27 + rd_i32(h + 19);
            agree = agree && a == st;
        }
        if (!agree)
        {
            snprintf(err, errlen, "map state signatures: %zu/%zu matches or targets disagree", rd_hits.size(),
                     run_hits.size());
            return GameInit::MissingData;
        }
        R.map_state = st;
    }
    {  // pause stack: optional (without it markers also show over the Esc menu)
        uintptr_t ps = 0;
        bool agree = true;
        auto hits = sig_find_all(m, SIG_PAUSE_STACK);
        for (auto h : hits)
        {
            uintptr_t cnt = reinterpret_cast<uintptr_t>(h) + 7 + rd_i32(h + 3);
            uintptr_t arr = reinterpret_cast<uintptr_t>(h) + 14 + rd_i32(h + 10);
            agree = agree && cnt == arr + kPauseCountOff && (!ps || arr == ps);
            ps = arr;
        }
        if (agree && ps)
            R.pause_stack = ps;
        else
            logf("pause stack signature: %zu matches%s; markers also show over the pause menu", hits.size(),
                 agree ? "" : " (disagree)");
    }
    detect_init(m);
    route_init(m);
    for (auto& c : kClasses)
        c.clsid = crc32(c.name);
    g_zone_clsid = crc32("O_BASEZONE");
    auto rva = [&](uintptr_t a) { return static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m.base)); };
    logf("resolved: g_level=+%llX player_off=0x%X player_vtbl=+%llX health_off=0x%X g_entities=+%llX "
         "table_off=0x%X name_off=0x%X string_pool=+%llX params_off=0x%X friend_off=0x%X brain_off=0x%X "
         "mental_off=0x%X enemy_off=0x%X camera_unjit=+%llX map_state=+%llX pause_stack=+%llX",
         rva(R.level_global), R.player_off, rva(R.player_vtbl), R.health_off, rva(R.entities_global), R.table_off,
         R.name_off, rva(R.string_pool), R.params_off, R.friend_off, R.brain_off, R.mental_off, R.enemy_off,
         rva(R.cam_unjit), rva(R.map_state), R.pause_stack ? rva(R.pause_stack) : 0ull);
    g_ready = true;
    return GameInit::Ok;
}

static void game_update_inner(FrameData& out);

// The game's pause stack: "ui_menu [default]" (Esc menu), "cengine::loader*" (loading), "app_inactive" (window in the
// background), ... Every reason but app_inactive means a screen covers the world, so the UI hides the markers.
static void read_pause(FrameData& out)
{
    out.paused = false;
    out.pause_reason[0] = 0;
    uintptr_t arr = 0;
    uint32_t count = 0;
    if (!R.pause_stack || !rdv(R.pause_stack, arr) || !rdv(R.pause_stack + kPauseCountOff, count) || !count ||
        count > 64 || !arr)
        return;
    uint32_t ids[8];
    const uint32_t n = count < 8 ? count : 8;
    if (!rd(arr, ids, 4ull * n))
        return;
    for (uint32_t i = n; i-- > 0;)  // top of the stack first
    {
        char s[40];
        if (!read_string(ids[i], s, sizeof(s)) || strcmp(s, "app_inactive") == 0)
            continue;
        out.paused = true;
        strncpy_s(out.pause_reason, s, _TRUNCATE);
        return;
    }
}

// Logs the reason once whenever the reader's state changes (ok <-> a failed check), so a silent stop is diagnosable.
void game_update(FrameData& out)
{
    static const char* last = nullptr;
    out.why = "";
    game_update_inner(out);
    const char* now = out.ok ? "ok" : out.why;
    if (now != last)
    {
        logf("reader: %s", out.ok ? "ok (drawing)" : now);
        last = now;
    }
    static char last_pause[40] = "";
    if (out.ok && strcmp(out.pause_reason, last_pause) != 0)
    {
        logf(out.paused ? "game paused: %s" : "game resumed", out.pause_reason);
        strncpy_s(last_pause, out.pause_reason, _TRUNCATE);
    }
}

static void game_update_inner(FrameData& out)
{
    out.ok = false;
    out.paused = false;
    out.pause_reason[0] = 0;
    out.npcs.clear();
    out.tracked = 0;
    if (!g_ready)
    {
        out.why = "game data unavailable (signature check failed at load)";
        return;
    }
    // Loading screens: the new level's player exists (and the camera still holds the last frame) well before the map
    // runs, e.g. ~8 s of "hold E to continue" after a quick-load. Draw only while the map runs, after a short settle.
    static uint64_t running_since = 0;
    uint32_t map_state = 0;
    if (!rdv(R.map_state, map_state) || map_state != kMapRunning)
    {
        running_since = 0;
        out.why = "loading (map not running)";
        return;
    }
    const uint64_t now_ms = GetTickCount64();
    if (!running_since)
        running_since = now_ms;
    if (now_ms - running_since < kMapSettleMs)
    {
        out.why = "map just started running (settling)";
        return;
    }
    read_pause(out);
    uintptr_t level = 0, player = 0, vt = 0;
    if (!rdv(R.level_global, level) || !level || !rdv(level + R.player_off, player) || !player || !rdv(player, vt) ||
        vt != R.player_vtbl)
    {
        out.why = "no player (main menu or loading)";
        return;
    }
    float xf[16];
    if (!rd(player + kXformOff, xf, sizeof(xf)) || !finite3(xf + 12) || !rd(R.cam_unjit + kCamVpOff, out.vp, 64))
    {
        out.why = "player position or camera unreadable";
        return;
    }
    memcpy(out.player_pos, xf + 12, sizeof(out.player_pos));
    rdv(R.cam_unjit + kCamAspectOff, out.aspect);
    rd(R.cam_unjit + kCamPosOff, out.cam_pos, 12);
    rd(R.cam_unjit + kCamFwdOff, out.cam_fwd, 12);
    out.game_time = 0;
    if (R.game_time)
        rdv(R.game_time, out.game_time);
    rdv(player + R.friend_off, out.player_friend_type);
    read_player_vis(player, out.game_time, out.player_pos, out.pv);
    out.detect_ok = D.vis_ok;
    out.route_ok = RL.ok;

    if (level != g_map_level)
    {
        uint32_t h = 0;
        g_map_level = level;
        if (!rdv(level + kMapNameOff, h) || !read_string(h, g_map_name, sizeof(g_map_name)))
            g_map_name[0] = 0;
    }
    strncpy_s(out.map, g_map_name, _TRUNCATE);

    uintptr_t server = 0, table = 0, pslot = 0;
    if (!rdv(R.entities_global, server) || !server || !rdv(server + R.table_off, table) || !table)
    {
        out.why = "entity table unavailable";
        return;
    }
    if (table != g_table)
        reset_tracking(table);
    // Layout self-check: the table slot named by the player's own ID holds the player. (Not slot 0: after a death /
    // reload the game can hand the player another ID, e.g. 4042 on Volga, with slots 0..511 empty.)
    uint16_t pid = 0;
    if (!rdv(player + kIdOff, pid) || !rdv(table + 8ull * pid, pslot) || pslot != player)
    {
        out.why = "entity table layout check failed (player not in its own slot)";
        return;
    }
    out.ok = true;
    sweep(table);
    out.tracked = static_cast<int>(g_creatures.size());

    out.zones.clear();
    if (g_settings.show_zones)
    {
        const float maxd = g_settings.zone_max_dist;
        for (uint16_t slot : g_zones)
        {
            uintptr_t e = 0;
            float p[3];
            if (g_slot_cls[slot] != kZoneCls || !rdv(table + 8ull * slot, e) || e != g_slot_ent[slot] ||
                !rd(e + kXformOff + 48, p, 12) || !finite3(p))
                continue;
            float dx = p[0] - out.player_pos[0], dy = p[1] - out.player_pos[1], dz = p[2] - out.player_pos[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > maxd)
                continue;
            Zone z{};
            z.id = slot;
            strncpy_s(z.name, g_slot_name[slot], _TRUNCATE);
            z.trigger = _strnicmp(z.name, "trigger", 7) == 0;
            if (g_settings.zone_triggers_only && !z.trigger)
                continue;
            float bb[8];
            if (!rd(e + kXformOff, z.xf, sizeof(z.xf)) || !rd(e + kZoneBoxOff, bb, sizeof(bb)))
                continue;
            bool ok = true;
            for (int i = 0; i < 3; ++i)
            {
                z.bmin[i] = bb[i];
                z.bmax[i] = bb[4 + i];
                ok = ok && std::isfinite(bb[i]) && std::isfinite(bb[4 + i]) && bb[i] <= bb[4 + i] &&
                     bb[4 + i] - bb[i] < 2000.f;
            }
            if (!ok)
                continue;
            z.dist = d;
            // player inside? local = projection on the (possibly scaled) axes
            float rel[3] = {out.player_pos[0] - z.xf[12], out.player_pos[1] - z.xf[13], out.player_pos[2] - z.xf[14]};
            bool inside = true;
            for (int i = 0; i < 3 && inside; ++i)
            {
                const float* ax = z.xf + 4 * i;
                float l2 = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
                float loc = l2 > 1e-8f ? (rel[0] * ax[0] + rel[1] * ax[1] + rel[2] * ax[2]) / l2 : 0.f;
                inside = loc >= z.bmin[i] && loc <= z.bmax[i];
            }
            z.player_inside = inside;
            out.zones.push_back(z);
        }
    }

    for (uint16_t slot : g_creatures)
    {
        uintptr_t e = 0;
        if (!g_slot_cls[slot] || !rdv(table + 8ull * slot, e))
            continue;
        if (e != g_slot_ent[slot])  // replaced since the sweep passed: classify now instead of skipping ~16 frames
        {
            const uint8_t nc = e ? classify(e, slot) : 0;
            if (nc == kZoneCls)
            {
                g_slot_cls[slot] = 0;  // leave g_slot_ent alone: the sweep files it under zones
                continue;
            }
            g_max_hp.erase(g_slot_ent[slot]);
            g_slot_ent[slot] = e;
            g_slot_cls[slot] = nc;
            uint32_t hn = 0;
            if (!nc || !rdv(e + R.name_off, hn) || !read_string(hn, g_slot_name[slot], sizeof(g_slot_name[slot])))
                g_slot_name[slot][0] = 0;
        }
        const uint8_t c = g_slot_cls[slot];
        if (!c)
            continue;
        uint32_t flags = 0;
        float hp = 0;
        if (!rdv(e + kFlagsOff, flags) || (flags & kFlagHidden) || !rdv(e + R.health_off, hp) ||
            !rd(e + kXformOff, xf, sizeof(xf)) || !finite3(xf + 12) || !std::isfinite(hp))
            continue;
        Npc n{};
        n.id = slot;
        n.cls = kClasses[c - 1].name;
        n.height = kClasses[c - 1].height;
        strncpy_s(n.name, g_slot_name[slot], _TRUNCATE);
        memcpy(n.pos, xf + 12, sizeof(n.pos));
        memcpy(n.fwd, xf + 8, sizeof(n.fwd));
        n.vis_fov = n.vis_range = 0.f;
        if (kClasses[c - 1].cat == Category::Human)
        {
            float vr = 0.f, vf = 0.f;
            if (rdv(e + kVisRangeOff, vr) && rdv(e + kVisFovOff, vf) && std::isfinite(vr) && std::isfinite(vf) &&
                vr > 1.f && vr < 300.f && vf > 5.f && vf <= 360.f)
            {
                n.vis_range = vr;
                n.vis_fov = vf;
            }
        }
        n.health = hp;
        float& mx = g_max_hp[e];
        if (hp > mx)
            mx = hp;  // first positive value seen, raised if exceeded (e.g. a heal)
        n.max_health = mx;
        rdv(e + R.friend_off, n.friend_type);
        n.rel = relation(n.friend_type, out.player_friend_type);
        n.cat = kClasses[c - 1].cat;
        n.display = display_name(n.cls, n.cat, n.name);
        n.has_brain = false;
        n.mental = 0;
        n.mood = Mood::Unknown;
        n.targets_player = false;
        uintptr_t brain = 0, owner = 0;
        n.awareness = 0.f;
        n.los = -1;
        if (R.brain_off && rdv(e + R.brain_off, brain) && brain && rdv(brain + kBrainOwnerOff, owner) && owner == e &&
            rdv(brain + R.mental_off, n.mental))
        {
            n.has_brain = true;
            n.mood = mood_of(n.mental);
            uintptr_t link = 0, iface = 0;
            if (R.enemy_off && rdv(brain + R.enemy_off, link) && link && rdv(link, iface) && iface)
                n.targets_player = iface - R.link_off == player;
            n.awareness = n.targets_player ? 1.f : (n.mental >= 50 ? 1.f : n.mental / 50.f);
            // Line of sight from the AI's own strict (raycast) sighting of its enemy: only known when that is us.
            int32_t seen = 0;
            if (n.targets_player && R.vis_strict_off && out.game_time && rdv(brain + R.vis_strict_off, seen) && seen > 0)
                n.los = (out.game_time - static_cast<uint32_t>(seen)) < kSeenRecentlyMs ? 1 : 0;
            if (D.vis_ok)
                read_vision(e, brain, player, out.game_time, n.vis);
        }
        float dx = n.pos[0] - out.player_pos[0], dy = n.pos[1] - out.player_pos[1], dz = n.pos[2] - out.player_pos[2];
        n.dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (n.has_brain && g_settings.show_routes && n.dist <= g_settings.route_max_dist && n.health > 0.f)
            read_route(e, brain, n.pos, n.route);
        out.npcs.push_back(n);
    }
}

const char* friend_type_name(uint8_t t)
{
    switch (t)
    {
    case 0: return "none";
    case 1: return "enemy";
    case 2: return "friend";
    case 3: return "enemy_for_all";
    case 4: return "enemy_and_enemy_for_all";
    case 5: return "deathmatch";
    case 6: return "any";
    case 7: return "friend_for_group";
    case 100: return "arahinds";
    case 101: return "watchmans";
    case 102: return "aquas";
    case 103: return "lurkers";
    case 104: return "nosalises";
    case 105: return "demons";
    default: return "?";
    }
}

const char* mental_state_name(uint8_t m)
{
    switch (m)
    {
    case 0: return "free";
    case 10: return "disturbed";
    case 30: return "alert";
    case 40: return "uber alert";
    case 50: return "danger";
    default: return m < 30 ? "light alert" : "invalid";
    }
}

bool world_to_screen(const float vp[16], const float p[3], float w, float h, float& sx, float& sy)
{
    float cx = p[0] * vp[0] + p[1] * vp[4] + p[2] * vp[8] + vp[12];
    float cy = p[0] * vp[1] + p[1] * vp[5] + p[2] * vp[9] + vp[13];
    float cw = p[0] * vp[3] + p[1] * vp[7] + p[2] * vp[11] + vp[15];
    if (cw < 0.05f)
        return false;
    sx = (cx / cw * 0.5f + 0.5f) * w;
    sy = (0.5f - cy / cw * 0.5f) * h;
    return true;
}
