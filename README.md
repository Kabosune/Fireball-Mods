# Fireball-Mods

Standalone, strictly offline / single-player mods for *Grand Theft Auto VI* (Project Americas / RAGE 9), built against the frozen pure-C11 SDK in **[Fireball-Core](https://github.com/Kabosune/Fireball-Core)**.

## Scope & Rules
- Strictly offline / single-player gameplay mods and utilities.
- Native plugins (`*.fbplugin.dll`) are pure C11 (`/std:c11 /W4 /WX`) with zero third-party dependencies, compiling solely against `${FIREBALL_CORE_DIR}/include` (`fireball_plugin.h`, `fireball_menu.h`) with full `FB_PLUGIN_ABI_VERSION = 1` / `FB_MENU_ABI_VERSION = 1` compatibility and graceful v1 fallback.
- Never commits compiled binaries (`.dll`, `.exe`, `.fbin`, `.gta6mod`), copyrighted game assets, encryption keys, decrypted executables, or scratch outputs.
- Read-only against your local game install (`--game-dir` / `GTA6_DIR`, default `D:\Games\GTA6`).

## Repository Layout

- `CMakeLists.txt` — MSVC VS 2022 x64 CMake build (`/std:c11 /W4 /WX`, CTest enabled) locating `FIREBALL_CORE_DIR` (cache or env var, default `C:/dev/fireball-core`).
- `mods/fpv_unlocker/` — **On-Foot First-Person View (FPV) Toggle & Unlocker** (`fpv_toggle.fbplugin.dll`, XML/KV/PSO patches, reference Python state machine). See [mods/fpv_unlocker/README.md](mods/fpv_unlocker/README.md).
- `mods/time_weather_control/` — **Time & Weather Control** (`time_weather.fbplugin.dll`, controller-friendly Mod Menu clock/weather controls, opt-in legacy F8 window, reference Python state machine). See [mods/time_weather_control/README.md](mods/time_weather_control/README.md).
- `scripts/stage_mods.py` — Stdlib-only Python helper to validate manifests, stage built `*.fbplugin.dll` + mod manifests into `--out` (default `D:\GTA6-work\fireball-mods-stage`), and optionally invoke `fireball_cli.py` to pack `.gta6mod` archives or compile `fireball_bundle.fbin`.
- `tests/mods_integration_test.c` — Native C11 CTest harness testing both plugins standalone (v1 fallback), mock Menu API sync & zero binding conflicts, and end-to-end inside `fireball_core.dll` (`FIREBALL_HEADLESS=1`).
- `tests/test_mods.py` — Offline Python `unittest` suite validating manifests, `scripts/stage_mods.py`, and reference Python plugin state machines.

## Build & Test Commands

```powershell
# Configure (uses C:/dev/fireball-core by default, or set FIREBALL_CORE_DIR)
cmake -B build -G "Visual Studio 17 2022" -A x64 -DFIREBALL_CORE_DIR=C:/dev/fireball-core

# Build all native plugins and CTest harness
cmake --build build --config Release

# Run native CTest suite (standalone v1 + mock Menu API + headless fireball_core.dll E2E)
ctest --test-dir build -C Release --output-on-failure

# Run offline Python unit tests
python -B -m unittest discover -s tests -v

# Stage built plugins + manifests to D:\GTA6-work\fireball-mods-stage
python -B scripts/stage_mods.py
```

## Default Controls & Chords

All default keyboard and XInput gamepad chord bindings are conflict-free across `fireball.core`, `community.fpv_unlocker`, and `community.time_weather_control`, and can be remapped live in the in-game Mod Menu (`fireball_user_config.json`):

| Mod | Action | Trigger | Default Keyboard | Default Gamepad |
| :--- | :--- | :--- | :--- | :--- |
| `fireball.core` | Toggle Mod Menu | Press | `F1` | `LB + View` |
| `community.fpv_unlocker` | Toggle First-Person (`toggle_fpv`) | Press | `V` | `LB + R3` |
| `community.fpv_unlocker` | Peek First-Person (`peek_fpv`) | Hold | `Alt + V` | `LB + L3` |
| `community.time_weather_control` | Clock +1 Hour (`hour_up`) | Press | `]` | `LB + DPad Up` |
| `community.time_weather_control` | Clock -1 Hour (`hour_down`) | Press | `[` | `LB + DPad Down` |
| `community.time_weather_control` | Pause / Resume Clock (`toggle_pause`) | Press | `\` | `LB + Start` |
| `community.time_weather_control` | Previous Weather (`weather_prev`) | Press | `;` | `LB + DPad Left` |
| `community.time_weather_control` | Next Weather (`weather_next`) | Press | `'` | `LB + DPad Right` |
| `community.time_weather_control` | Lock / Unlock Weather (`toggle_weather_lock`) | Press | `Ctrl + \` | `LB + Y` |
| `community.time_weather_control` | Legacy Control Window (`toggle_window`, opt-in `ui_enabled = true`) | Press | `F8` | Unbound |

## Adding a New Mod

1. Create `mods/<mod_name>/` with:
   - `mod.toml` (`[mod]` metadata with unique `id`, `min_fireball_abi = 1`, `[[patches.*]]` if any, and `[[plugins]]` / `[plugins.config]`).
   - `native/<plugin>.c` compiling cleanly as pure C11 against `${FIREBALL_SDK_INCLUDE_DIR}` (`fireball_plugin.h` and optional `fireball_menu.h`).
   - `plugins/<plugin>.py` (optional reference state machine) and a concise `README.md`.
2. Add a `SHARED` target in `CMakeLists.txt` with `OUTPUT_NAME "<plugin>.fbplugin"`, `PREFIX ""`, `FB_BUILD_DLL`, and `fb_apply_strict_c11(...)`.
3. Choose default keyboard and gamepad chords that do not conflict with existing entries in the table above, and keep runtime state synchronized via `menu->set_setting(..., notify = 0)`.
4. Add native checks to `tests/mods_integration_test.c` and manifest/Python checks to `tests/test_mods.py`.

## License

Dual-licensed under either of:
- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT License ([LICENSE-MIT](LICENSE-MIT) or <https://opensource.org/licenses/MIT>)

at your option. Covers Fireball-Mods source code only; grants no rights to any game or third-party material.
