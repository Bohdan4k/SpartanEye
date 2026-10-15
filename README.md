# Spartan Eye - NPC & Stealth Overlay

**An NPC and stealth overlay for Metro Exodus Enhanced Edition (Steam).** It shows every NPC through walls, with
distance, health, AI state and, from the game's own vision data, exactly how close each one is to spotting you. It
can also show where NPCs are walking and what they will do when they get there.

*Single-player only. Read-only: it never changes game data, game files or saves.*

[Русская версия](README.ru.md)

![Stealth preset: markers through walls, detection list, view cones](docs/images/stealth.png)

## Features

- **Markers through walls** for humans, mutants and animals: box (or corners), name, distance, health bar.
- **Relation colours** from the game's own friend / enemy rule: friendly green, neutral blue, hostile red, dead grey.
- **AI state**: calm / alerted / combat, `?` when an NPC is alerted or noticing you, `!` when it has spotted you.
- **Exact detection meter**: each NPC's own vision value for you against its live "spotted" threshold.
  100 % is the moment the NPC reacts. Values come from the engine, not from a guess.
- **"Who is detecting you" list**, an alert banner (optional sound) when an NPC notices or targets you, and an optional
  "why" panel. The panel shows your visibility factors (light, stance, speed, vegetation cover) and the formula terms
  for the NPC that sees you best.
- **View cones** drawn from each NPC's real vision data: range, field of view, edge range, all-round zone, instant zone.
- **Line of sight**: NPCs that currently see you are drawn solid, NPCs behind cover faded or dashed.
- **Routes and intentions**: the path an NPC is walking, its stop point, which way it will face there, and what it will
  do there (patrol point, cover, investigate, search, check a body...), with an estimated time of arrival.
- **Edge arrows** for threats that are off screen or behind you.
- **Presets**: *Minimal*, *Stealth* (default) and *Full / Debug*. Smart detail shows full info only where it matters:
  threats and the NPC under your crosshair.
- **Filters**: humans / mutants / fauna × friendly / neutral / hostile, plus a max distance.
- **Debug tab**: raw engine values, trigger / script zones as boxes, and an NPC report written to a text file (HOME).
- Hidden automatically on loading screens and in the pause menu. Every setting is saved in an ini file next to the
  DLL. Keys can be rebound.

| Combat | Routes | Menu |
|---|---|---|
| ![Spotted](docs/images/combat.png) | ![Routes](docs/images/routes.png) | ![Menu](docs/images/menu.png) |

## Requirements

- **Metro Exodus Enhanced Edition on Steam**, game build **24973569** (`MetroExodus.exe` 2.0.0.1). Other builds are
  refused with a clear message, and nothing is loaded. This covers another Steam patch, the Epic or GOG versions, and
  the original 2019 edition. See [After a game update](#after-a-game-update).
- Windows 10 or 11, 64-bit. Any GPU that runs the game (NVIDIA, AMD, Intel), DLSS on or off, any resolution, aspect
  ratio and window mode.
- Nothing else to install. There are no runtimes and no installer, and nothing is copied into the game folder.

## Install and launch

1. Download `SpartanEye-1.0.0.zip` and extract it to any folder you can write to (Documents, Desktop...).
   Not `Program Files`, because the overlay keeps its settings and log next to itself.
2. Run **`SpartanEye.exe`**. It starts the game through Steam and waits for the game window. Then it loads the
   overlay and closes itself. If the game is already running, it only loads the overlay.
3. In game, a short message confirms the overlay is loaded. Press **INSERT** for the menu.

Optional: run `Create desktop shortcut.cmd` once to get a desktop shortcut that does step 2.

Command line: `SpartanEye.exe --inject` (only load into a running game), `--unload`, `--shortcut`.

**Uninstall:** press END in game (or close the game), then delete the folder and the shortcut. Nothing else is
left behind: no registry entries, and no files in the game or save folders.

## Keys

| Key | Action |
|---|---|
| INSERT | Open / close the menu (the game does not receive mouse and keyboard while it is open) |
| HOME | Save an NPC report (every NPC within range, with raw values) to `metro_npc_reports.log` |
| END | Unload the overlay (the game keeps running) |

Rebind them in the menu: *Settings → Keys*.

## What you see

| Element | Meaning |
|---|---|
| Marker colour | Relation to you: green friendly, blue neutral, red hostile, grey dead |
| Yellow → orange → red | Detection / AI state: sees you, noticed / alerted, light alert, spotted / combat |
| `?` / `!` | Alerted or noticing you / has spotted you, in combat |
| Detection bar and % | The NPC's vision value for you ÷ its live "spotted" threshold. Ticks mark the "notices" and "light alert" levels. A `~` means an estimate from the AI state (no vision data for that NPC) |
| "Alerted, can't see you" | Alerted by something else: a sound, a body, a comrade's alarm, or lost sight |
| Faded / dashed marker | Behind cover: the NPC's AI does not currently see you |
| White bar | Health |
| Small diamond | Calm NPC far away (smart detail) |
| Route line with chevrons | Where the NPC walks. Dots are corners, the ring is where it stops, the arrow from the ring is where it will face |
| Icon + label at the stop | What it will do there. `~12 s` = estimated time of arrival |
| Cone on the ground | The NPC's vision. Outlined while it is in combat, has spotted you, or you stand inside it |

The *Overlay* tab of the menu has the same legend.

## Presets

- **Minimal**: small markers. Full info only for NPCs that detect you, are alerted, or are under the crosshair. No cones.
- **Stealth** (default): full info and view cones for threats and the crosshair NPC, names for calm NPCs nearby, arrows
  for threats and nearby hostiles off screen, routes for threats and the crosshair NPC.
- **Full / Debug**: everything for every NPC, with raw engine values. Busy, but meant for checking and debugging.

Changing any option by hand shows *Custom*. Pick a preset to go back.

## Settings file

`metro_npc_overlay.ini` next to the DLL, written by the overlay whenever you change something. You can edit it while
the game is closed. Delete it to reset everything to defaults. *Settings → Reset to defaults* in the menu does the
same.

## After a game update

The overlay supports exactly one game build. When Steam updates the game, the launcher reports *"unsupported game
version"* and loads nothing. Your game is never at risk. Check this page for a new release.

For developers: every game address is found at start-up by byte signatures (each must match uniquely, or several
must agree). A small patch often keeps them valid. To try a new build: set `GAME_BUILD_TIMESTAMP` in `src/version.h`
to the new `MetroExodus.exe` PE timestamp and rebuild. Then check `metro_npc_overlay.log`: it lists every resolved
address, or the signature that failed. `docs/ENGINE_NOTES.md` explains every structure and signature.

## Troubleshooting

- **Log:** `metro_npc_overlay.log` next to the DLL (the previous session's log is `metro_npc_overlay.old.log`). The
  launcher also prints the overlay's own verdict.
- **Antivirus warning:** the launcher loads a DLL into the game with `CreateRemoteThread` + `LoadLibrary`. That is
  how most overlays, trainers and mod loaders work, and some antivirus engines flag it on heuristics. The binaries are
  unsigned. The complete source is here, the release zip has a SHA-256 checksum, and you can build it yourself (see
  below). If your antivirus removed the DLL, the launcher says so.
- **"cannot open the game process":** the game runs as administrator. Run the launcher as administrator too, or
  neither.
- **No markers:** open the menu (INSERT). The bottom line says why nothing is drawn (loading, paused, no player...).
- **Folder not writable:** the launcher warns. Move the folder out of `Program Files`.

## Single-player only

Metro Exodus is a single-player game without anti-cheat. This overlay only reads game memory. It draws by hooking the
DirectX 12 present call, and hooks DirectInput only to keep keys from reaching the game while its menu is open. It
never writes game data, never touches the game folder, saves or online services. Use it in single player only.

## Build from source

Requirements: Visual Studio 2022 or the Build Tools for Visual Studio 2022 (workload *Desktop development with C++*:
MSVC v143 and a Windows 10/11 SDK). CMake 3.25 or newer is included with Visual Studio.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
python tools/package_release.py        # optional: release zip + SHA-256 in dist/
```

The output is `build/Release/SpartanEye.exe` and `metro_npc_overlay.dll`. The CRT is linked statically, so the
build needs no runtime. The repository's GitHub Actions workflow builds the same zip on every push.

Layout: `src/` overlay DLL (game data in `game.cpp`, drawing in `ui.cpp`, display rules in `display.cpp`, D3D12 hook in
`render.cpp`), `launcher/` the launcher, `thirdparty/` vendored libraries (unmodified), `docs/ENGINE_NOTES.md`
reverse-engineering notes, `tools/` release and privacy-scan scripts.

## Known limitations

- Detection covers **vision**. Hearing is not shown.
- Line of sight is the AI's own sighting of you. While an NPC has no reason to look for you, it shows as "unknown"
  (drawn as visible by default).
- The peripheral vision zone is approximate, because the game uses the head direction and the overlay draws it from
  the body.
- Tested in depth on the Moscow / Dead City and Volga levels. Neutral factions that turn hostile, and the later
  levels, are untested.
- Time of arrival on routes is an estimate (path length ÷ recent speed), shown with `~`.

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause), [Dear ImGui](https://github.com/ocornut/imgui)
  (MIT) and stb (MIT). See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
- Reverse engineering with [Ghidra](https://github.com/NationalSecurityAgency/ghidra). Notes on the camera layout of
  the original edition in [TefMeister/metro-exodus-vr](https://github.com/TefMeister/metro-exodus-vr) were a useful
  starting point.
- **AI disclosure:** this mod was developed with Claude Code (Anthropic) as the coding and reverse-engineering agent,
  directed and tested in game by root.
- Not affiliated with or endorsed by 4A Games, Deep Silver or PLAION. Metro Exodus is a trademark of its owners.

## License

[MIT](LICENSE) © 2026 root
