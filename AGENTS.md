# AGENTS.md — shared rules for every AI agent in this repo

This file is the single source of truth for **all** agents (Claude, Antigravity/Gemini, or any other). Read it fully before doing anything, then read `ROADMAP.md`. `CLAUDE.md` just points here.

## Project

Fireball-Mods: standalone offline / single-player *GTA VI* mods (`mods/<name>/`), built only against the public SDK of Fireball-Core. `README.md` has the layout, the default controls table, and the "Adding a New Mod" steps. Follow those steps.

### Related repos

| Repo | Path | Relationship |
| :--- | :--- | :--- |
| fireball-core | `C:\dev\fireball-core` | **Upstream SDK.** We use only its `include/` headers (`FIREBALL_CORE_DIR`, default `C:/dev/fireball-core`) and, for E2E tests, its built `build/Release/fireball_core.dll`. Never edit fireball-core from here. If a mod needs an SDK change, stop and note it in `ROADMAP.md` for fireball-core. |
| gta6-modding | `C:\dev\gta6-modding` | Format research and read-only tools. The source of truth for offsets and layouts. Never invent addresses or offsets. |

## Hard rules
- Strictly offline / single-player. No online/multiplayer, anti-cheat, or DRM-bypass work.
- `D:\Games\GTA6` is **read-only** (`--game-dir` / env `GTA6_DIR`).
- Never commit compiled binaries (`.dll`, `.exe`, `.fbin`, `.gta6mod`), game assets, keys, decrypted executables, user config, logs, or scratch output (see `.gitignore`).
- **Zero third-party dependencies.** Native plugins are pure C11 (`/std:c11 /W4 /WX`). Python is 3.13, stdlib only.
- Plugins compile against `${FIREBALL_CORE_DIR}/include` only, targeting `FB_PLUGIN_ABI_VERSION = 1` / `FB_MENU_ABI_VERSION = 1`, and must keep working when the menu ABI is absent (graceful v1 fallback).
- Default keyboard and gamepad chords must not conflict with any row in the README controls table. Update that table when adding bindings.
- **Do not commit or push unless the user explicitly asks.** Stop with a summary and `git status` for review.

## Paths

| What | Path | Rule |
| :--- | :--- | :--- |
| Game install | `D:\Games\GTA6` | Read-only. |
| Staging output | `D:\GTA6-work\fireball-mods-stage` | Default `--out` of `scripts/stage_mods.py`. Never inside the repo. |
| Fireball-Core | `C:\dev\fireball-core` | Read-only from this repo. Build it there first for E2E tests. |

## Build & test

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DFIREBALL_CORE_DIR=C:/dev/fireball-core
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python -B -m unittest discover -s tests -v
python -B scripts/stage_mods.py
```

If `fireball_core.dll` isn't built in fireball-core, CTest runs only the standalone and mock-menu tests. Say so when reporting results.

Report verification honestly: paste real command output, say what failed, and say what you skipped and why.

## Workflow
1. Read `ROADMAP.md`. Work on the item marked **Next** or **In progress**, unless the user says otherwise.
2. Tick checkboxes as deliverables are done and verified.
3. Before ending a session, update `ROADMAP.md`: the status, the **Handoff** notes (done / left / gotchas / verification), and the next item's spec if one has been agreed. This replaces writing a prompt for the next session.
4. Only edit `AGENTS.md` / `CLAUDE.md` when the user asks.
