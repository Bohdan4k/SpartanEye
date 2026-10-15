# Metro Exodus EE: verified structures

> Reverse-engineering notes behind the overlay (offsets, signatures, how each value was verified). Addresses are
> for Steam build 24973569 only; the overlay resolves everything by signature at start-up. The research tools
> named below (`tools/*.py`) were used during development and are not part of this repository.


Build: Steam 24973569, `MetroExodus.exe` 2.0.0.1, PE timestamp 1787840176, sha256 `4cd2d254…f572`.
RVAs are relative to the module base (ASLR). The tools and the overlay resolve everything **by signature**; the RVAs
are for this build only. Status tags: `[verified-live]`, `[verified-static]`, `[hypothesis]`.

## 1. Camera (3a)

### Location
| Object | RVA | Notes |
|---|---|---|
| Main camera | `0x27C55F0` | ~112 code references to its base; P/VP carry TAA jitter (row 2) `[verified-live]` |
| Unjittered copy | `0x27C5AC0` (= main + 0x4D0) | same pose as main every frame we sampled; P without jitter. **Use this for the overlay** `[verified-live]` |
| Sibling | `0x27C5120` (= main − 0x4D0) | same pose; jittered P; role unknown (previous frame?) `[hypothesis]` |

Object size 0x4D0. Same layout as the original edition's camera at 0x2111600 (TefMeister VR dossier).

### Layout `[verified-live]`
| Offset | Type | Field |
|---|---|---|
| +0x000 | float4 | position (w = 1) |
| +0x010 | float3 (+pad) | forward |
| +0x020 | float3 (+pad) | up |
| +0x030 | float3 (+pad) | right (= up × forward) |
| +0x040 | float4x4 | V (view) |
| +0x080 | float4x4 | P (projection) |
| +0x0C0 | float4x4 | VP = V·P |
| +0x100 | double4x4 | V in double precision `[verified-static, from dump]` |
| +0x4B4 | float | vertical FOV in degrees (60) |
| +0x4B8 | float | aspect = client width / height (2560/1421 = 1.8015) |

### Conventions `[verified-live]`
- World: **Y up**, left-handed, units ≈ metres (eye ~1.9–2.0 m above the ground under it).
- Matrices: row-major, **row vectors** (`v · M`); translation in row 3.
  - V rows: x column = right, y column = up, z column = forward.
  - `V[3] = −(pos·right, pos·up, pos·forward)`; checked by hand.
- P: `[sx 0 0 0; 0 sy 0 0; jx jy q 1; 0 0 f 0]`, with `sy = 1/tan(vfov/2)` (1.73205 at 60°) and `sx = sy/aspect`.
  **Reversed-Z**: `q = −n/(far−n)`-style, `f ≈ n`. Observed q = −0.00040016, f = 0.10004 → near 0.1, far 250 m
  in this level; the menu camera had 27° / far ≈ 400.
- World → screen (client pixels):
  `clip = [x y z 1]·VP`; skip if `clip.w ≤ 0`; `sx = (clip.x/clip.w·0.5 + 0.5)·W`; `sy = (0.5 − clip.y/clip.w·0.5)·H`.
  W×H = **client area** (2560×1421 windowed here).

### Signatures `[verified-static + verified-live]`
| Name | Pattern | Resolves |
|---|---|---|
| `SIG_CAM_PAIR` (camera static init) | `48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2` (RVA 0xD78858) | main = m+7+i32(m+3); unjit = m+19+i32(m+15); check unjit−main = 0x4D0 |
| `SIG_CAM_MAIN_ALT[0]` | `48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 74` (RVA 0xDBCAAB) | main = m+7+i32(m+3) |
| `SIG_CAM_MAIN_ALT[1]` | `48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 45 0F 57 C9 F3 0F` (RVA 0x389A11) | main = m+7+i32(m+3) |
All three are unique in the exe and agree in the live process (`tools/ee_camera.py`).

### Evidence (2026-10-13, Dead City tunnel)
- `tools/memread.py scanproj` finds P matrices in `.data`. The 0x27C5xxx cluster holds 3 camera objects 0x4D0 apart.
- **Forward test:** `pos + 10·fwd` projects to (1280.0, 710.5) = client centre.
- **Triangulation test** (`tools/cam_logger.py` + `tools/oracle_3a.py`, shots in `re/private/shots/3a_oracle/`):
  - A tire on the ground was seen from three still poses A, B, C.
  - Rays through the tire's measured image centre in A and B (unprojected with each pose's P/basis) meet within
    **2.6 cm** at 4.2 m / 6.0 m. Re-projected, the point lands **4 px** (A) and **3 px** (B) from the measured centres.
  - In C (oblique, hole not visible, left end in shadow) the prediction is 32/33 px (≈0.2 m at 6.8 m) from the
    estimated blob centre. That's within the uncertainty of measuring that blob.
  - With crosshair-only aiming (no image measurement), B's aim was ~0.25 m off the hole centre, which shifted the
    triangulated point ~0.5 m sideways as seen from C. Image-based centres are needed for tests like this.
- All recorded poses: main and unjittered copies give identical pose and VP apart from the sub-pixel jitter (≤ 1 px).

### Open
- Behaviour in cutscenes, photo mode, menus, scopes/ADS (FOV change) and level loads: check in Stage 6.
- Borderless/fullscreen: the client size changes; read W×H from the window (or the swap chain in the overlay).

## 2. Player entity + health (3b)

### Chain `[verified-static + verified-live]`
| Step | Value | Evidence |
|---|---|---|
| level/game global | `[MetroExodus.exe+0x1696CE0]` (`g_level`) | ~1744 code refs; `cmd_new_game_plus` (`fn 0x140098790`) tests it and `+0x28` ("!G NG+ [%s] - player is missing!"); map name id at `+0xF8` (`change_map %s`). The old CE table's name "g_PhysX" is wrong |
| player | `[g_level + 0x28]` | same function; live vtable check below |
| player vtable | `MetroExodus.exe+0x11E7B18` (`vtbl_player`) | `player_dbg_print_stats` (`fn 0x140819490`) compares `*this` with it; live value matches |

The player object is **re-created on every load** (address changed across a quick-load), so re-read the chain each
poll/frame. While loading, `g_level` or `+0x28` may be null or point at a non-player; check the vtable.

### Entity fields
| Offset | Type | Field | Status |
|---|---|---|---|
| +0x000 | ptr | vtable | `[verified-live]` |
| +0x110 | float4x4 | world transform: rows right / up / forward / position (w=1) | `[verified-live, player only]` (to confirm statically and on NPCs in 3c) |
| +0x140 | float3 | position (= transform row 3) = **feet**; main camera is 2.0 m above when standing | `[verified-live, player]` |
| +0x3B8 | float | **health**. Base-entity field: `entity_load_props_base` (`fn 0x1408479f0`) reads property key `"health"` into it and ~30 subclass loaders call that function. Player: 1.0 = full, **−1.0 after death** | `[verified-static + verified-live]` |
| +0x9E0 | float2 | head angles x/y (player; from the stats printer) | `[verified-static]` |
| +0x2A1C | float | stamina (player class; stats printer) — stayed 1.0 during the test, never exercised | `[verified-static]` |

### Signatures (all unique in the exe and in the live .text; `tools/ee_player.py`)
| Name | Pattern | Resolves |
|---|---|---|
| `SIG_LEVEL_PLAYER` (RVA 0x98794) | `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 83 78 ?? 00 74 ?? 48 8B CA FF 15` | g_level = m+7+i32(m+3); player_off = u8(m+15) |
| `SIG_STATS` (RVA 0x8195A8) | `F3 0F 10 9B ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? F3 0F 10 93 ?? ?? ?? ?? 48 8B 0F 0F 5A DB` | stamina_off = i32(m+4); health_off = i32(m+19) |
| `SIG_HEALTH_LOAD` (RVA 0x847AB2) | `48 8B 0E E8 ?? ?? ?? ?? F3 0F 11 83 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 8B 0E 33 D2 E8` | health_off = i32(m+12); check: `lea r8,[key]` at m−0x1B points to `"health"` |
| `SIG_PLAYER_VTBL` (RVA 0x8194DA) | `48 8B 03 48 8D 0D ?? ?? ?? ?? 48 3B C1 0F 29 74 24 50` | vtbl = m+10+i32(m+6) |
The tool refuses to run if the two health offsets disagree or the key string isn't `"health"`.

### Evidence: damage / heal / death oracle (2026-10-13, Dead City tunnel, pid 23224)
`tools/ee_player.py --watch` (50 ms poll, change-only) → `re/private/logs/3b_health.jsonl`. User report: "walked into
fire twice, then used a medkit, then died (from fire again); loaded the quick-save".
| Log time | Health | Interpretation |
|---|---|---|
| 0 s | 1.0000 | full health at the quick-save spot (−703.98, 4.91, 1322.38) |
| 367.8–370.4 s | 1.0 → 0.925 → 0.85 → 0.7862 → … → 0.1150 | fire #1: ticks of 0.0750 / 0.0638 (~4 Hz) |
| 370.44 s | **−1.0000** | death sentinel, 50 ms after the last positive value |
| 377.5 s → 378.1 s | no player → 1.0000 at the quick-save spot | quick-load; new player object |
| 437.6–439.2 s | 1.0 → 0.5088 | fire #2 |
| 439.2–451.9 s | 0.5088 constant (12.7 s) | **no passive regeneration** |
| 451.94–452.40 s | 0.5088 → 0.5453 → … → 0.9560 → **1.0000** | medkit: ramps over ~0.46 s, clamped at exactly 1.0 |
| 675.1–676.2 s | 1.0 → 0.8713 → 0.7213 → 0.5713 → 0.4213 → 0.3078 | fire #3 (0.15/tick: stronger fire) |
| 676.2–688.0 s | 0.3078 constant (11.8 s) | no regeneration again |
| 688.0–688.5 s | 0.2130 → 0.0630 → **−1.0000** | death |
| 722.6 s → 723.3 s | no player → 1.0000 at (−660.23, 6.37, 1317.36) | load (a newer save than the first reload's) |
Note: the log has **two deaths** (after fire #1 and fire #3); the user's account mentions one. Every change in the
log is explained by fire, medkit, death or load; no unexplained writes.

### For the overlay
- Alive test: `health > 0` (dead = −1.0, not 0). For NPCs, check the dead value and max health in 3c (may not be 1.0).
- Health bar: `clamp(health, 0, 1)` for the player; NPC scale TBD (3c).

### Open
- Stamina never changed in the test (no sprinting); live check pending (not needed for the overlay).
- `+0x110` transform: find a static reference and confirm on NPCs (3c).
- CE: `re/metro_ee.CT` Player group uses the same pointer chain but hasn't been opened in Cheat Engine yet.

## 3. Entity registry + NPC enumeration (3c)

### Registry `[verified-static + verified-live]`
| Item | Value | Evidence |
|---|---|---|
| entity server | `[MetroExodus.exe+0x1696C88]` (`g_entities`, ~333 code refs) | `level_load_map_setup` (`fn 0x1406A55C0`, logs `entities_server::map_setup`) loops `i < 0xFFFF` over `[[g_entities]+0x10][i]` calling vtable `+0x1040` on each |
| entity table | `[server+0x10]`, 0xFFFF slots of `entity*`, **indexed by entity ID**, null = free | `entity_link_from_id` (`fn 0x14006DF20`): `[[g_entities]+0x10][id]`; if `server+0x28BB` != 0 only ids < 0x200 resolve (MP client path; 0 in SP) |
| player | slot 0 (= `[g_level+0x28]`) | live |
| live counts (Dead City, 2026-10-13) | 51,999 non-null slots, 93 vtables; 107 creatures (28 in world, 79 hidden) | `tools/ee_reader.py` |
Every non-null slot satisfies `u16 [entity+0x170] == slot` (51,999 / 51,999).

### Entity fields (base entity, all classes)
| Offset | Type | Field | Status |
|---|---|---|---|
| +0x0E0 | ptr | link block (refcounted weak ref; `*link` = this+0xE0, the interface other systems hold) | `[verified-static]` link resolvers; `level_load` uses `iface − 0xE0` |
| +0x0E8 | u32 | xform write lock (owning thread id) | `[verified-static]` `entity_update_xform` |
| +0x110 | float4x4 | world transform (rows right/up/forward/position, w=1) | `[verified-static]` `entity_update_xform` (`fn 0x1408A3260`) writes it under the lock; a parent's xform is read as `*link + 0x30` (= +0x110). `[verified-live]` marker oracle below |
| +0x170 | u16 | entity ID (= table slot) | `[verified-static]` `entity_link_resolve` (fallback constant 0x170 = iface−0xE0+0x170); `[verified-live]` all slots |
| +0x174 | u32 | flags; **0x20 = hidden / not in world** (activation update takes the deactivate path). Also set on the player and on carried items, so never use it for the player | `[verified-static]` `entity_update_activation` (`fn 0x1406513A0`, 40 code sites test it); `[verified-live]` set on all 79 off-map / scripted-later creatures and clear on all 28 creatures present |
| +0x180 / +0x198 | ptr / i16 | parent link / parent bone (attached entities) | `[verified-static]` `entity_update_xform` |
| +0x238 | u32 | **name** (pooled string handle) | `[verified-static]` `ai_set_enemy` (`fn 0x140B17DA8`) prints it with `%s` |
| +0x240 | ptr | class params; **`u32 [params+0]` = clsid** | `[verified-static]` `mov rax,[rax+0x240]; mov ecx,[rax]; cmp ecx,[clsid_*]` (RVA 0x9852F0); `[verified-live]` |
| +0x3B8 | float | health (§2). Scale is per class: NOSALIS 1.0, RAT 0.15, BLIND 3.5, KID 0.1, scripted actors 1001 / 10000; dead = −1.0 | `[verified-static]` loader; `[verified-live]` values + one dead NOSALIS at −1.0 |

### Class ids and strings `[verified-static + verified-live]`
- **clsid = `zlib.crc32(class_name)`** (reflected CRC-32, init/final −1), e.g. `init_clsid_MONSTER` (`fn 0x140038D50`) stores
  crc32("MONSTER") = 0x1B1B38EE in `clsid_MONSTER` (0x1427C317C); live value matches.
- Engine class groups: `clsid_is_creature` (`fn 0x140A30EB0`) lists the creature roster (NPC_FX, LIBRARIAN, NOSALIS,
  NOSALIS_FEMALE, AMEBA, RAT, FLYING_CREATURE, KID, DARKCHILD, WOMAN, WOMAN_COMBAT, SIMPLE, DARK, ANOMALY, HARPY, LURKER,
  WATCHMAN, DOG, BIG_MOTHER, PLAYER, ARAHIND, AQUA_*, GRIZLY, WOMAN_STRIP, RABBIT, HUMANIMAL, DEER, CATFISH, CANNIBAL,
  SNAKE, SEA_CUCUMBER, WORM, BLIND, FROG) after excluding `clsid_in_group_monster_npc_weapons` (`fn 0x140230D20`: MONSTER,
  DUMMY_NPC, SIMPLE_MONSTER, SIMPLE_NPC, NPC, HUMAN, NOSACH + some weapon classes). Human/mutant/fauna split = 3d.
- **Pooled strings:** handle `h` → `[g_string_pool_pages + 8*(h>>24)] + (h & 0xFFFFFF) + 0x14` (NUL-terminated).
  `g_string_pool_pages` = RVA 0x1696410. Same decoding gives the map name (`[g_level+0xF8]` → `m3\13_deadcity`).

### Signatures (`tools/ee_reader.py`; all hits must agree)
| Name | Pattern | Hits | Resolves |
|---|---|---|---|
| `SIG_ENT_SERVER` (RVA 0x6DF37) | `0F B7 0A B8 FF FF 00 00 48 8B 3D ?? ?? ?? ?? BD FF FF FF FF` | 1 | g_entities = m+15+i32(m+11) |
| `SIG_ENT_TABLE` (RVA 0x6DFBB …) | `48 8B 47 ?? 0F B7 C9 48 8B 0C C8 48 85 C9 74 ?? 48 81 C1 ?? ?? ?? ?? 48 8B 19` | 5 (inlined) | table_off = u8(m+3); link_off = i32(m+19) |
| `SIG_NAME` (RVA 0xB17FDC, 0xB180EC) | `8B 8E ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 4C 8B D8 41 B8 FF FF FF 00` | 2 | name_off = i32(m+2); pool = m+13+i32(m+9) |
| `SIG_CLSID` (RVA 0x9852F0) | `48 8B 80 ?? ?? ?? ?? 8B 08 3B 0D ?? ?? ?? ?? 89 4C 24 50` | 1 | params_off = i32(m+3) |
ID (+0x170), flags (+0x174) and xform (+0x110) are constants; the tool checks `table[0]` is the player and that every
row's ID equals its slot, and refuses otherwise.

### Evidence: marker oracle (2026-10-13, `m3\13_deadcity`, game paused)
`tools/ee_reader.py --max-dist 120 --shot re/private/shots/3c/oracle_1.png` → 16 enabled creatures within 120 m
(5 NOSALIS alive + 1 dead, 8 RAT, NPC_FX miller_car_runner_2, KID m2_vault_kirill). Boxes from feet (+0x140) to
+1.8 m projected with the 3a camera:
- NOSALIS id 64514 at 12.9 m, the only creature in direct view: box frames the mutant feet to head.
- NOSALIS id 64258 at 6.0 m (y 6.0, under the train-car floor the player stands on, y 9.6): box at the bottom edge,
  drawn through the car (the overlay's through-walls case).
- NOSALIS/RAT at 30–48 m with y ≈ −0.35 (track level below the platform): boxes behind the platform edge, consistent
  with being hidden from this viewpoint.

### Open
- NPC health was not exercised live (no NPC took damage during 3c); evidence = shared base loader, per-class config
  values and the −1.0 dead NOSALIS. Max health per class (for health bars): unknown, needed for the overlay → use
  the first value seen per entity, or find the config "health" per class.
- Human/mutant/fauna + friendly/neutral/hostile: 3d.
- Reading the whole table externally takes ~1–2 s (52k entities); the overlay reads in-process.

## 4. Relation + category (3d)

### friend_type `[verified-static + verified-live (Dead City)]`
- **`u8 [entity+0x3D9]` = `friend_type`**: the base-entity loader (`entity_load_props_base`) reads property key
  `"friend_type"` with the u8 reader and stores it there (`SIG_FRIEND_TYPE` below).
- Enum **`efriend_type`** (names read live from the game's loaded script enum `_G.types.efriend_type`, table at
  `[[MetroExodus.exe+0x1696CA0]+0xDBD8]`: 15 × {u32 value, u32 pooled-string handle}):
  `0 none, 1 enemy, 2 friend, 3 enemy_for_all, 4 enemy_and_enemy_for_all, 5 deathmatch, 6 any, 7 friend_for_group,
  100 arahinds, 101 watchmans, 102 aquas, 103 lurkers, 104 nosalises, 105 demons, 255 maxbyte`.
- **Engine rule** `entity_is_enemy_base` (`fn 0x14084EC60`, vtable slot +0x398 base): `a` is an enemy of `b` iff both
  types are non-zero and (`b`==5 or `a`==5 or `a`!=`b` or `b`==3); `a`==7 (friend_for_group) goes to a group check
  (`fn 0x1408445A0`). Monsters (`monster_is_enemy`, `fn 0x14094E560`) and humans (`human_is_enemy`, `fn 0x1409819A0`)
  first ask the AI brain (`vtbl+0x5A0` / `+0xF10`, method `+0x8F8`) whether the other is excluded; the base also checks
  the brain's explicit enemy list (`fn 0x14009B7A0`). The overlay models the base rule only.
- Live (Dead City): player 2; Miller (NPC_FX) 2 → friendly; Anna (WOMAN) 2; Dark child 2; rats 7; Kirill (KID) 0;
  dormant nosalis/blind templates 1; **dead creatures read 0** (cleared on death).
- "Neutral" humans in Exodus are an AI action block (`actions/human/neutral`, loader `fn 0x140BF9780`: `neutral_state`
  +0x60, `neutral_flags` +0x70, `neutral_timer` +0x74), not an entity field.
- **User check (2026-10-14, Dead City):** Miller shown green (friendly), Kirill shown yellow (neutral): correct.
- **UNVERIFIED** (no Volga / Caspian / Taiga saves available):
  - neutral humans (warn-then-attack): whether they carry `friend_type` `none` until provoked, or `enemy` with the
    neutral action block holding them back (then the overlay would show them hostile too early);
  - NPCs turning hostile at runtime (friend_type change via `make_friend_type` / AI explicit enemy lists, which the
    overlay does not model);
  - human enemy factions (bandits, slavers, cultists, pirates): expected `enemy` (1) → hostile.
  The HOME report key (overlay) logs class / name / friend_type / relation per NPC for these cases later.

### Signature
| Name | Pattern | Resolves |
|---|---|---|
| `SIG_FRIEND_TYPE` (RVA 0x847C83; 3 matches) | `48 8B 0E E8 ?? ?? ?? ?? 88 83 ?? ?? ?? ?? 4C 8D 05` | the match whose `lea r8,[key]` 31 bytes earlier points to `"friend_type"`: off = i32(m+10) = 0x3D9 |

### Category (class → human / mutant / fauna; overlay + `ee_reader.py`)
- human: NPC, HUMAN, SIMPLE_NPC, DUMMY_NPC, NPC_FX, WOMAN, WOMAN_COMBAT, WOMAN_STRIP, KID, CANNIBAL
- fauna: DOG, DEER, RABBIT, SNAKE, RAT, FROG, WORM, SEA_CUCUMBER, FLYING_CREATURE
- mutant: every other creature class (NOSALIS, WATCHMAN, LURKER, ARAHIND, AQUA_*, GRIZLY, HUMANIMAL, LIBRARIAN,
  BLIND, DARK, DARKCHILD, AMEBA, HARPY, CATFISH, BIG_MOTHER, MONSTER, SIMPLE_MONSTER, NOSALIS_FEMALE, NOSACH)

## 5. AI brain, stealth, zones (feature groups B, 3, 4, 5)

### AI brain `[verified-static + verified-live]`
| Field | Value | Evidence |
|---|---|---|
| brain | `[entity+0xF10]`; `brain+0x08` = owner entity | `monster_is_enemy` (`fn 0x14094E560`) reads `[rcx+0xF10]`; brain debug dump `fn 0x1409423C0` prints the owner's name (+0x238) and position (+0x140); live: owner == entity for all creatures |
| mental state | `u8 [brain+0xA51]`: 0 free, 10 disturbed, other <30 light alert, 30 alert, 40 uber alert, 50 danger | names from `fn 0x1409423C0`; live: calm NPCs 0, nosalis killed in combat frozen at 50 |
| current enemy | link at `brain+0xC70` → `*link` = enemy interface (entity+0xE0) | `fn 0x1409423C0` prints `enemy: %s` via `(*link)+0x158` (= entity+0x238) |
| enemy position | `brain+0xC90` (vec3) | same dump (`enemy_pos`) |
| last strict sighting of the enemy | `i32 [brain+0xA20]` (game time, ms) | brain save `fn 0x14093DC90`: key `_last_enemy_visible_strict_time` → `mov [rbx+0xA20],eax` `[verified-static]` |
| game time | `u32 [MetroExodus.exe+0x1703304]` (ms; ~1884 code refs) | `SIG_GAME_TIME` `44 0F B6 5A 01 48 8B DA 44 8B 0D ?? ?? ?? ?? 45 03 D8` → m+15+i32(m+11) |
Signatures: `SIG_BRAIN` `48 8B F9 41 B8 01 00 00 00 48 8B 89 ?? ?? ?? ?? 48 8B DA 48 8B 01 FF 90` (off = i32(m+12));
`SIG_MENTAL` `41 0F B6 86 ?? ?? ?? ?? 3C 1E 77 ?? 74 ?? 84 C0` (i32(m+4)); `SIG_BRAIN_ENEMY`
`49 8B 86 ?? ?? ?? ?? 33 DB 0F 28 B4 24` (i32(m+3)); `SIG_VIS_STRICT` `48 8B 0F 33 D2 E8 ?? ?? ?? ?? 89 83 ?? ?? ?? ?? 4C 8D 0D`
(off = i32(m+12); key check: `lea r8` at m−7 → `"_last_enemy_visible_strict_time"`).

### Vision (detection) `[verified-static, partial]`
- Human-NPC vision params are loaded by `fn 0x14082EBE0` into the entity: `player_vision_bwd_range` +0xFF4,
  `player_vision_fov` +0xFF8, `decrease_value` +0x1010, `luminocity_factor` +0x1014, `velocity_fac` +0x1018,
  `min_threshold` +0x1028, `threat_threshold` +0x1050, `light_alert_threshold` +0x1054, `vis_threshold` +0x1058,
  `surv_*` +0x105C..+0x1068, `peripheral_*` +0x103C..+0x1048 (`player_vision_range` presumably +0xFF0). Dead City has no
  human NPCs of that class, so these are **unverified live**; other classes leave the block at 0.
- The per-NPC accumulated visibility value (what the thresholds are compared against) was **not found**: only the
  loader touches these offsets; the vision code reads them through a pointer. `_enemy_visible_threshold_timer` lives in
  a script action block (`fn 0x140BEC290`, +0x7C), not in the entity/brain. → needs a live memory-diff session on a level
  with human enemies (stand in view, then hide). The overlay's awareness meter is derived from the mental state.

### Zones `[verified-static, live check pending]`
- Class `O_BASEZONE` (2,641 in Dead City). Loader `fn 0x14071A9D0` reads `shapes` / `shape_%02d` / `shape_type` /
  `shape_box_size` / `shape_sphere_radius` into 0x50-byte records: sphere (type 0 at +0x40) = local centre +0x00,
  radius +0x0C; box (type 1) = 3x4 local matrix (+0x00..+0x2F, rows with translation in column 4) and half size
  +0x30..+0x38 (legacy single-shape path: box size × constant at 0x14147BDD0, presumably 0.5). The list goes to a
  trigger object at `zone+0x178` (created via the global at RVA 0x1696CF0); the zone's bounding box from virtual
  `+0x1090` is stored at `zone+0x150` (min) / `+0x160` (max).

## 6. Detection (vision) system `[verified-static + verified-live (Volga, 2026-10-14)]`

One system for humans AND mutants (every creature with an AI brain): the **vision feel object** = `[brain+0xE0]`
(first of six feel objects at brain+0xE0..+0x108; class vtable RVA 0x132F6A8; `vision+0x08` → brain sub-object whose
`+0x08` = owner entity). Feel #1 (vtable 0x132F5A0, values 0.2/0.4/0.7/0.25) is probably hearing (event based).

### Per-NPC vision parameters (copied by `fn 0x140ABBAF0` from a params block `+0xFF4..+0x106F` into vision+0x10.. and +0x348..)
| vision+ | meaning (from the update formula) | Volga values |
|---|---|---|
| 0x10 | range R (m) | 30 (watchman, raven, cultist), 40/50 crew, Anna 190 |
| 0x14 | backward range (m) | 10 / 3 |
| 0x18 | FOV (radians, full angle) | 2.094 = 120° (Katya 40°) |
| 0x1C / 0x20 | range factor at FOV edge / centre | 0.8 / 1.0 |
| 0x24 | minimum distance for the log falloff | 0.25 |
| 0x28 | update period (uint, ms) used as dt divisor | |
| 0x2C | decay per period when not visible | 500 |
| 0x30 / 0x3C / 0x34 | light offset / gain / velocity factor | |
| 0x40 | min light threshold | |
| 0x44 | subtracted beyond the distance threshold | |
| 0x54 | force-danger range | 2.0 hostile cultists, 0 neutrals |
| 0x58 / 0x5C / 0x60 | peripheral distance / horizontal / vertical angle (head matrix, owner vtbl+0x530) | 8 m / 80° / 50° hostile cultists, 0 (off) others |
| **0x68** | **noticed threshold** (value ≥ → "noticed" memory [vision+0xB8], `fn 0x140F9B600`, notify vtbl+0x50) | 5 hostile humans, 3 neutrals |
| **0x6C / 0x70** | **disturbed / light-alert thresholds** (mental state follows them, live-verified) | 5 / 20 mutants & neutrals, 3 / 15 bandits & cultists |
| **0x74** | **seen threshold** (value ≥ → "enemy" memory [vision+0xB0], `fn 0x140F9B3D0`, notify vtbl+0x58) | 100 |
| 0x78 / 0x7C / 0x80 / 0x84 | the same four in "surv" mode (flag u32 vision+0x88) | 8 / 2.5 / 2.5 / 20 |

**All thresholds are live, the brain rewrites them** (session 1): in light alert the bandits' +0x6C/+0x70 drift from
3/15 towards 5/5 within ~5 s; in combat (danger/uber) the set becomes 5 / 5 / **0.55** and the value is capped at
8.5 (bandits) or 38 (ravens, shrimps), so a lost target is re-acquired almost instantly. Read them every frame.
Per NPC they differ too (Grisha: range 59.7, disturbed 4.98).

### Accumulator (the detection value)
`fn 0x140F878D0` (vision update): list at **`vision+0x90`**, count **u16 `vision+0x9A`**, 16-byte entries
`{u64 target link (→ target interface = entity+0xE0), float value, u8 updated, u8 camera-mode}`. Each update:
`delta = fn 0x140F9B9B0(...)`, `value = clamp(value + delta, 0, max)`. **Detection % = value / seen threshold (+0x74).**
Targets in the per-frame candidate list (vision+0x2E8) are updated through `fn 0x140F9B140` → `fn 0x140F9AFD0`, which
**creates the entry only when the gain is positive** (`fn 0x140F9AE20(..., delta > 0)`): no entry = never gained
sight = 0 %. The cap (out param of fn 0x140F9B9B0) is above 100 when calm (a friendly prisoner reached 505).
A forced path in fn 0x140F878D0 (`fn 0x140F863C0`, via the target's vtbl+0x430) sets the value straight to
max(+0x68, +0x70[, +0x74]) + eps (probably contact / force-danger range; not yet confirmed live).

### Session 1 results (Volga, 2026-10-14, `re/private/logs/detect_session1.*`, 8 Hz logger)
- Raven #24952: 0.4 → 6.9 → **108.3 in the same sample as danger + targeting the player** (100 = reaction ✔).
- Raven #28749: 4.8 → 7.1 = disturbed (threshold 5 ✔). Watchman #5761: disturbed at 8.7 (≥5), light alert at 27.1 (≥20) ✔,
  with up to ~1.5 s brain lag after the crossing.
- Bandit Tolya: light alert at 38 (≥15 ✔). Bandit Dagger: 52.8, danger ~0.3 s game time later at 4 m (crossing 100
  fell into a 1.3 s logger stall; v2 logger samples at 50 Hz without stalls).
- **Non-visual alerts exist**: Dagger went light alert with no vision entry (hearing / bodies); Jager and Tolya went
  danger at the same instant with value 0 (group alert after Dagger). The overlay must show these as "alerted, not by
  sight" from the mental state, not as a vision %.
- Church: friendly/neutral praying cultists never created an entry even at 3 m; three hostile tower cultists were
  taken down from 0.8 m behind at 0 % (never sensed).
- Player stance factor +0x9C4 = 10 in the boat (1 on foot); luminosity 1.4–1.5 in daylight, 0.04–0.4 in shade/indoors.
- Hearing (feel #1, vtable 0x132F5A0): three sets of 0.2/0.4/0.7/0.25 plus 0.1/0.125/0.15/0.1, event based; not RE'd yet.

### Core formula `fn 0x140F9B9B0(vision, target, cam, dt, …, target_pos, out_delta, out_max)`
- dist from the NPC (entity +0x140) to the target; angle vs the NPC **body forward** (xform row 2, +0x130);
  effective range lerps R·k_edge (+0x1C) .. R·k_centre (+0x20) over the half FOV (+0x18 × 0.5).
- within range: `delta = (max(V − [+0x40], floor) + [+0x30]) × [+0x3C] × (speed × [+0x34] + 1) ×
  (ln R_eff − ln d) / (ln R_eff − ln [+0x24]) × dt/period`, minus `[+0x44]` beyond the distance threshold, ≥ 0.
- line of sight: owner vtbl+0x438 raycast; not visible → `delta = −dt/period × [+0x2C]` (decay).
- V = target "visibility" = player vtable +0x4C8 (`fn 0x14081B8A0(player, 0)`; +0x4D8 = camera variant):
  `V = (speed > p[+0x1684] ? speed × p[+0x1680] : 0) + p[+0x167C]) × env × light × stance`, with
  light = max(player[+0x2370] × player[+0x27DC], torch flash p[+0xF64] while game time < player[+0x24E0]),
  stance = player[+0x9C4], env = `fn 0x14088E040` (k/(k+f) from the world object at RVA 0x1696C68, 1.0 when off),
  p = player class params ([player+0x240]). Volga: base 1.0, speed factor 0.075 above 2 m/s.
- Per-target wrapper `fn 0x140F9B140` (records at vision+0x2E8, count u16 +0x2F2, stride 0x70: link +0x30, pos +0x4C,
  flags +0x64) adds the peripheral check from the head matrix.
Tool: `tools/detect_log.py` (session logger).

### Session 2 (chapel cultists, 2026-10-14, `re/private/logs/detect_session2.*`, 50 Hz logger v2) and corrections
- **Thresholds, corrected**: the NPC turns *disturbed* when the value reaches **+0x68** (5 for cultists: boss2 at 5.17,
  5.09, 5.05; stays free at 3.0-3.15), *light alert / alert* at **+0x70**, *spotted / danger* at **+0x74**. +0x68 is
  also the near distance in the formula. +0x6C's role is not known (it drifts 3 -> 5.5 in alert); the overlay does not
  use it. While alerted (uber) the whole set slides within ~10 s to the combat set 5.5 / 5.5 / 5 / **0.55**.
- **100 % = reaction, humans**: cultist 0008's value crossed its falling "seen" threshold (37 -> 64 vs 56 -> 0.55)
  between two samples and it went to danger targeting the player; the others followed by group alarm ~0.5 s later,
  most with no vision entry at all. Cultist 0002 went uber with no entry (player 5.6 m behind it: hearing).
- **Formula verified**: predicted vs measured rise rate over 111 calm rising segments: median ratio **0.99**
  (quartiles 0.85-1.13), one peak (no hidden factor). Units: per update `gain x dt/period`, dt and period (+0x28,
  5 for cultists) in ms -> per second = gain x 1000 / period.
- **Geometry**: the FOV does not cut the gain inside fn 0x140F9B9B0 (the angle is clamped to the edge): the cut is in
  the producer of the visible list (vision+0x220, copied to +0x2E8 by fn 0x14024DEB0; flag bits at record +0x64).
  Measured: outside the 120 deg cone the value rises only within the **back range** (10 m: max 10.2 m at 90-120 deg,
  5.9 m at 150-180 deg) at the edge-range rate (ratios 0.83-1.38) = all-round sight within back range.
  Within **force range** (+0x54, 2 m for hostile cultists) and in view the delta is set to the seen threshold (instant).
  Near floor: within +0x68 metres the light term is at least 0.15 (data 0x14147BCAC). Peripheral zone +0x58/+0x5C/+0x60
  (8 m, 80 deg, 50 deg) uses the head matrix (owner vtbl+0x530).
- **Crouching**: no multiplier of its own (player +0x9C4 stayed 1.0 all session; it is 10 in the boat). The env factor
  `fn 0x14088E040` = 1.0 unless player +0x9C8 != 0 and (dead or movement-state nibble
  `(([[player+0x800]+0x10]+0x34) >> 4) & 0xF == 1` = crouched); then `k / (k + f)`, k = params +0x8F0 (10),
  f = vegetation density `fn 0x140D8A470(world[+0x2500], player pos, min(params +0x8F4, 1.99))`: 3x3 samples of a 96x96
  grid of 2 m cells (+0x29DA70, origin ints +0x487AA0/+0x487AA4, count u16 +0x2804E8), per plant instance
  {pos, scale +0xC, type u16 +0x20}: `f += scale x density(type) x (1 - d^2/size^2)` with size = half the model's
  XZ bbox diagonal x scale + r. Live: dense bushes f 8-10 (env 0.5), sparse grass 0.4-1 (env 0.9+), village 0.
- Overlay: all of the above resolved by signature (game.cpp `detect_init`, layout anchors checked; any shift turns
  exact detection off with a log line). Hearing (feel #1) still not reversed: shown as "alerted, can't see you".

### 6.6 Display rules (overlay, `overlay/src/display.cpp`; 2026-10-15)
What the overlay **shows** is derived from the raw engine values above by these rules; the exact raw values stay
visible in the debug labels ("raw x% stage / shown y% stage") and in the "why" panel. Replayed on both logged sessions
with `overlay_replay` (same code as the DLL, 60 frames/s, values held between log records).

Raw inputs per frame: value v (no entry, 0 or not finite = 0), the live thresholds N = +0x68 noticed, L = +0x70 light
alert, S = +0x74 seen (the "surv" set +0x78/+0x80/+0x84 when vision+0x88 != 0), T = the brain's enemy is the player
(brain +0xC70), M = mental state (brain +0xA51).

1. **Raw percent** p = 100 if T, else min(100, 100·v/S). NPCs without a vision object: the AI-state estimate (M/50,
   1 when T), shown with "~".
2. **Raw stage**: spotted if T or v ≥ S; else light alert if v ≥ L; else noticed if v ≥ N; else **low if p ≥ 1 %**;
   else none. Checked from the top because in the combat set N (5.5) and L (5) lie above S (0.55). Values under 1 % of
   S are glimpses (replay: blips of 0.46 % that appeared and vanished within 0.3-0.8 s) and count as none.
3. **Displayed stage** rises to the raw stage at once. It drops (straight to the current raw stage) only after the raw
   stage has stayed lower for **1 s**, or **2 s** when leaving "spotted"; while the percent is rising again
   (trend > 3 %/s) the drop waits, at most twice the hold.
4. **Displayed percent**: 100 while "spotted" is displayed; otherwise it follows p upwards at once and falls at most
   **40 points per second**. Why: the alert/combat threshold slide (S 100 → 0.55 exponentially over ~8 s, M 40) makes
   p rise without a new sighting (real: the NPC needs less to re-spot you), and the end of combat snaps the calm set
   back in one frame (S 0.55 → 100), which made p jump 100 → 0; a value can also reset to 0 at once.
5. **Bar colour** = where the fill stands against the ticks drawn on it (the live N/S and L/S): yellow below N,
   amber from N, orange from L, red at 100. The label / icon colour follows the displayed stage.
6. **Trend** ("rising" / "falling", %/s in the list): p's change smoothed with a 0.4 s time constant; rising above
   +3 %/s, falling below -3 %/s, a new direction shown after it held 0.5 s (the value moves in steps every 100-300 ms).
7. **Threat** (full detail, cone, edge arrow, list row) = alive, has an AI, not friendly, and (T, or M ≥ 10, or
   displayed stage ≠ none). On at once, off **3 s** after the last reason; a death ends it at once.
8. **Icon**: "!" if displayed stage = spotted or M ≥ 40; "?" if noticed / light alert or M ≥ 10. Up at once, down
   after **1 s**.
9. **Line of sight** (only known while T: strict sighting brain+0xA20 younger than 600 ms): a sighting or the first
   data shows at once; visible → hidden after **1 s** without one; known → unknown after **2 s**.
10. **Presence**: an NPC fades in over 0.15 s (not in the first second on a map); one that leaves the NPC list fades
    out over **0.4 s** (short drop-outs are bridged), a dead one goes at once. Everything starts over on a new map,
    while the reader has no player (loading) and when game time runs backwards (quick-load).
11. Around the edges: calm NPCs switch between box and small marker with a 2 m band around the detail distance; the
    last 5 m before the max distance, 3 m before the cone and arrow distances fade instead of cutting; the crosshair
    NPC keeps its focus 0.4 s after the crosshair left it; labels keep their slot while it is free and slide back
    (150 px/s at 1080p); list rows re-sort only on a key change > 8 points; alerts re-arm 10 points below the
    threshold.

Replay results (`overlay_replay`, previous build's rules vs these), session 2 / session 1:
stage changes back within 1 s **17 → 0** / 4 → 0; full detail on/off back within 1 s **14 → 1** / 3 → 4 (the
remaining ones are real: a 2 % glimpse or an alert-state change 0.1-0.8 s after a 3 s quiet spell); percent drops of
≥ 30 points in one frame **16 → 3** / 8 → 2 (the remaining ones are the player's death / reload at t = 969 s, where
every NPC reset at once). Rises are unchanged (18 / 19 vs 22): they are real events and stay instant.

## 7. Routes and AI task (step 3) `[verified-static + verified-live (Volga, 2026-10-15)]`

### 7.1 Movement motor (inside the brain)
The brain update `fn 0x140946E90` calls the motor update `fn 0x140A75FA0` with `RCX = brain + 0xDA0`: the motor is
**embedded in the brain at +0xDA0** (`motor+0x00` = brain, `motor+0x08` = entity: both checked every read).

| motor+ | meaning | source |
|---|---|---|
| 0x18 | goal entity (when the goal is an object) | fn 0x140A75FA0 arrival check |
| 0x70 | u32 goal kind, 0 = none (7 = move to a position, seen live; 4 = "stopping path", fn 0x140A73C60) | prologue `cmp [rdi+0x70],esi` |
| 0x80 | goal position (= path[0] while moving, live) | fn 0x140A75FA0 |
| 0xA0 | u8 flags; 0x10 = face the linked object's direction at the end | fn 0x140A75500 |
| 0x118 | link → object whose +0x50 is a direction (face object) | fn 0x140A75500 |
| 0x120 | float4 direction to turn to when the path completes ("path completed, but need turn") | fn 0x140A75500 |
| 0x140 / 0x148 / 0x14A | path points (0x70 bytes each) / u16 capacity / u16 count | fn 0x140A75500 / fn 0x140A756C0 |
| 0x1C8 / 0xD0 | movement activity name / previous (pooled string, "move") | fn 0x140A73C60 |
Path point: +0x00 float3 position, +0x20 u8 segment type (0 = walk, else special: fn 0x140A75D00), +0x60 u8 flags
(bit 0 = final point). **The path is a stack**: `[count-1]` is the next waypoint (fn 0x140A756C0 measures the NPC
against `[count-1]` and `[count-2]`; fn 0x140A75500 pops it unless it is final), `[0]` is the destination with the final
flag and equals motor+0x80. Live (Volga, searchers after a fight): count fell 32 → 31 → 28 → 27 → 25 while
`[count-1]` stayed 0-2.5 m from the NPC and `[0]` stayed put; a finished search walk (count 1) was followed by a new
goal 9 m away (change of plan). Calm NPCs standing still: goal kind 0, count 0. Paths are dense (up to 48+ points).

### 7.2 Current task, script and parts
`brain+0xAC0` = current task (list of tasks at +0xAB0, u16 count +0xABA). Getters used by the brain dump
`fn 0x1409423C0`: vtbl+0x148 task type, vtbl+0x150 script name, vtbl+0x158 current part name (vtbl+0x130 = the
scripted part object: `mov rax,rcx` or `xor eax,eax`).
- Type getter = `lea rax,[rip+name]; ret` → static strings: `patrol`, `cover`, `alert: search`, `alert: approach`,
  `alert: base search`, `script`, `do_nothing`, `scary`, `combat`, `light_alert`, `scripted_fire`...
- Script getter `48 8B 41 18 ... 8B 48 04`: name = pooled string handle at `[[task+0x18]+4]` (e.g.
  `scr_search_play_path`, `scr_cover_wait`, `scr_idle_stand`, `scr_human_alert_face_threat`, level scripts such as
  `bridge_blind_babka_idle_01`).
- Part getter (RVA 0x21EC20): script = `[task+0x18]`, parts = `[script+0x08]` (0x90 bytes each, type byte at +0),
  count u16 `[script+0x12]`, current index u32 `[task+0x20]`; name = `[table + type*16]`, table at RVA 0x15B3FB0:
  236 `part_*` names (part_wait, part_get_path_to_cover, part_enter_cover, part_patrol_follow_path,
  part_get_path_to_threat, part_follow_search_path, part_corpse_found, ...), then 6 state names.
The overlay never calls these getters: it recognises them by their code (patterns with the resolved offsets) per vtable.

### 7.3 Intent shown (overlay `display.cpp` `route_intent`)
The first meaningful part from the current one on (what the script does when the walk ends: `enter_cover`,
`patrol_path_completed`, `corpse_found`, `script_action`, `wait`...), else the parts just before the current one
(what set the walk up: `get_path_to_cover`, `follow_search_path`), else the task type. Plain movement / bookkeeping
parts (`walk_path`, `wait_for_movement`, `set_scr`, ...) do not count. Categories: patrol point, take cover,
investigate (`*threat*`: a sound, a sighting or an alarm; the AI does not say which), search, check body
(`corpse_found`), attack, fall back, follow, action / animation, wait, idle (`do_nothing`), scripted.
**Estimates**: only the time of arrival (path length / the NPC's speed over the last ~1 s, shown up to 60 s, "~").

### 7.4 Signatures (overlay `game.cpp`, each unique in build 24973569)
SIG_MOTOR_CALL (brain update, gives +0xDA0 and must call SIG_MOTOR_UPDATE), SIG_MOTOR_UPDATE (+0x70), SIG_PATH
(+0x14A, 0x70, +0x140, +0x60), SIG_END_DIR (+0x118, +0xA0 & 0x10, +0x50, +0x120), SIG_TASK (+0xAC0, +0xABA, +0xAB0),
SIG_TASK_SLOTS (+0x158 / +0x150 / +0x148), SIG_PART_GETTER (+0x18, +0x12, +0x20, +0x08, table), SIG_SCR_GETTER (+0x04).
Tool: `tools/route_probe.py` (read-only; `--watch` prints changes).

## 8. Map state + pause stack (step 4, loading screens) `[verified-static + verified-live (2026-10-15)]`
- **Map state** u32 global, RVA 0x16FE780 (in the engine object at RVA 0x16FE4F0, +0x290). Values from the code:
  16 written at engine start (fn 0x140568200), 1 / 4 dispatched by the main loop (fn 0x140582110), 32 seen live at
  start-up, **2 = "loader_wait_start: mapstate_loaded_and_waiting"** (fn 0x1405CF140 / fn 0x1405CF9D0 / fn 0x1405CFDC0),
  **8 = "loader_wait_finish: mapstate_running"** (fn 0x1405CF0B0 / fn 0x1405CFC80, also fn 0x1405A8E60).
- Live quick-load (tools/mapstate_probe.py): 8 -> 4 (~6 s; the player object is gone only ~1.6 s of it) -> 2 (~4 s,
  the "hold E to continue" screen) -> 8. While 4/2 with the player present, the camera still holds the last frame:
  **the bug** (markers and the detection list drawn over the loading screen). The overlay draws only in state 8,
  250 ms after it starts.
- Signatures (must agree): SIG_MAP_STATE_READ (fn 0x140377720: `mov eax,[state]; cmp eax,8; jz; ...; mov edx,-1;
  call [WaitForSingleObject]`, 1 hit) and SIG_MAP_STATE_RUN (the 2 loader_wait_finish writers of 8).
- **Pause stack**: array of pooled reason-string handles at RVA 0x1F25C48 (ptr +0, u32 count +0xC); pushed / popped
  with "~~~> PUSH/POP PAUSE - REASON [%s]". Reasons seen live: `ui_menu [default]` (Esc menu), `app_inactive`
  (window in the background), `qloader` + `gameload` + `cengine::loader*` (quick-load). SIG_PAUSE_STACK: 3 hits
  (`mov r8d,[count]; mov r9,[array]; mov eax,[reason]`), count == array + 0xC in all. The overlay hides markers while
  any reason other than `app_inactive` is on the stack (option "Show while the game is paused").
