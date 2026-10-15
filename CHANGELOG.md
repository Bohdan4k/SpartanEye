# Changelog

All notable changes. Versions follow [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-10-15

First public release. Supports Metro Exodus Enhanced Edition, Steam build 24973569 (`MetroExodus.exe` 2.0.0.1).

### Added
- NPC markers through walls: box / corners, name, distance, health bar; colours by relation (friendly / neutral /
  hostile / dead) from the game's own rule.
- Filters: humans / mutants / fauna × friendly / neutral / hostile, max distance, dead NPCs.
- AI state (calm / alerted / combat) with `?` / `!` icons and edge arrows for off-screen threats.
- Exact detection meter from the engine's vision data (value ÷ live "spotted" threshold), "who is detecting you" list,
  alert banner with optional sound, "why" panel (visibility factors and formula terms).
- View cones from each NPC's real vision parameters; line-of-sight styling from the AI's own sighting.
- Routes and intentions: path, stop point, facing, what the NPC will do there, estimated time of arrival.
- Presets Minimal / Stealth / Full-Debug, smart detail, distance fade, label declutter, display rules with hold times
  (no flicker).
- Debug tab: raw values, trigger / script zones, NPC report (HOME).
- Settings in `metro_npc_overlay.ini`, rebindable keys, reset to defaults.
- Launcher `SpartanEye.exe`: starts the game through Steam, waits until it is ready, loads the overlay and reports
  the result; `--inject`, `--unload`, `--shortcut`; optional desktop shortcut.
- Hidden on loading screens and in the pause menu (option to keep it while paused).
- Safety: refuses any other game build with a clear message; all game reads guarded; the overlay disables itself and
  logs instead of crashing the game; frame cost logged every 30 s.
