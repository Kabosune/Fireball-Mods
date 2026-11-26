# FPV Quadcopter Drone (`community.fpv_drone`)

Strictly offline / single-player Betaflight-in-the-loop FPV quadcopter mod for *Grand Theft Auto VI* (Project Americas / RAGE 9), powered by **[hammerill/libfpv](https://github.com/hammerill/libfpv)** loaded dynamically at runtime (`libfpv.dll`, ABI v1).

## Why `libfpv`?

Instead of approximating quadcopter physics with an arcade flight loop, `libfpv` runs **real Betaflight 2025.12 firmware** (via SimITL) on a dedicated 1 kHz simulation thread:
- Real PID loops, rates, filters, arming checks, Angle / Horizon / Acro / Air Mode / Turtle Mode.
- Live **Betaflight Configurator** connection over WebSocket (`ws://localhost:5761`) and `diff all` import from real quads.
- Real R/C radios (EdgeTX / OpenTX / ELRS / Crossfire USB HID), gamepads, and keyboard fallback via libfpv's embedded SDL3.
- Shared per-user config folder (`%APPDATA%\libfpv`), so your radio calibration, presets, and Betaflight EEPROM follow you across games.
- Physical quad presets, battery sag/drain, prop/motor damage, wind/gusts, GPS home arrow, MAX7456 OSD, Dear ImGui overlay, and stereo synthesized motor/beeper audio.

## Architecture & Zero-Dependency Build

- `fpv_drone.fbplugin.dll` is **pure C11 (`/std:c11 /W4 /WX`)** with zero third-party build-time dependencies and compiles solely against Fireball-Core's public C11 SDK (`fireball_plugin.h`, `fireball_menu.h`).
- At runtime, it loads x64 `libfpv.dll` via `LoadLibraryW` + `GetProcAddress` (checking `libfpv_dll_path`, `LIBFPV_DLL`, `<plugin_dir>\libfpv.dll`, `<plugin_dir>\libfpv\libfpv.dll`, then `PATH`) and verifies `fpv_abi_version() == 1`.
- Supports both **monolithic** `libfpv.dll` and **split-process** builds (`libfpv.dll` + `libfpv-server.exe` + `libfpv-fonts/` + `libfpv-presets/`).
- If `libfpv.dll` is not yet installed, the plugin loads cleanly in standby mode, reports diagnostic status in the Fireball Mod Menu, and exposes a **Load / Retry `libfpv.dll`** button.

### Installing `libfpv.dll` (x64 Windows)

`libfpv` builds on Linux/WSL with MinGW-w64 (`cmake/toolchains/mingw-w64-x86_64.cmake`, see [hammerill/libfpv](https://github.com/hammerill/libfpv#windows)):

```sh
git clone --recursive https://github.com/hammerill/libfpv.git
cd libfpv
# Split build (MIT client DLL + GPL libfpv-server.exe):
cmake -S . -B build/win64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
  -DLIBFPV_SPLIT_PROCESS=ON -DLIBFPV_BUILD_TESTS=OFF -DLIBFPV_BUILD_EXAMPLES=OFF
cmake --build build/win64 -j
```

Copy the resulting `libfpv.dll` (and for split builds, `libfpv-server.exe`, `libfpv-fonts/`, `libfpv-presets/`) next to `fpv_drone.fbplugin.dll` in your staging/mods folder (never commit binaries into this repo).

## RAGE 9 Integration & Title Symbols

- **Coordinate System:** RAGE 9 Z-up meters (`+X` East/Right, `+Z` Up, `+Y` North/Forward, `1.0 unit/m`).
- **50 ms Post-Spawn Settle Guard:** Skips camera/pose writes during the first 50 ms of `sim_time` after `fpv_drone_create()` to avoid single-frame `(0,0,0)` camera jumps.
- **Placeholder Symbols:** Until live VIctory memory scans in `gta6-modding` confirm the exact RVAs, configure the symbols below in `victory.toml` under `[fireball.symbols]` (plugin also runs cleanly on Tier-0-only hosts without them):
  - `fb.drone_camera` — camera output (`mat34_fov`, `pos_quat_fov`, or RAGE `Rz(yaw)Rx(pitch)Ry(roll)` `pos_euler_fov`).
  - `fb.drone_camera_override` — `u32` flag (`1` while drone camera owns the viewport, `0` in Pilot view / recalled / shutdown).
  - `fb.pilot_pos` — `float[3]` pilot feet position + optional `float heading_rad` (`+0x0C`).
  - `fb.streaming_focus` — `float[3]` position + `float[3]` velocity so map/LOD streaming follows a distant drone.
  - `fb.drone_pose` — `float[3]` pos + `float[4]` quat + `float[4]` motor RPM + `u32` flags for an entity/prop proxy.
  - `fb.wind` — `float[3]` world wind velocity (m/s) fed into `fpv_wind_set()`.
- **Audio & OSD:**
  - Stereo 44.1 kHz Win32 `waveOut` worker (`fpv_drone_audio.c`) pulls `fpv_audio_read()` directly.
  - Opt-in Win32 GDI live Betaflight MAX7456 OSD preview window (`osd_window_enabled = false` by default) renders `fpv_osd_render_rgba()` + live telemetry until direct D3D12 command-queue overlay hooks are exposed by Fireball-Core.

## Default Controls

All default keyboard and gamepad chords are conflict-free across `fireball.core`, `community.fpv_unlocker`, and `community.time_weather_control`, and can be remapped live in the Fireball Mod Menu (`F1` / `LB + View`):

| Action | Trigger | Keyboard | Gamepad |
| :--- | :--- | :--- | :--- |
| Launch / Recall Drone (`toggle_drone`) | Press | `F7` | `LB + X` |
| Cycle Camera View (`cycle_view`) | Press | `Shift + V` | `LB + B` |
| Repair & Respawn (`respawn_drone`) | Press | `Ctrl + R` | `LB + A` |
| Toggle Pilot View (`toggle_pilot_view`) | Press | `Ctrl + F` | `RB + View` |
| Toggle `libfpv` Overlay (`toggle_overlay`) | Press | `F9` | `RB + Start` |

When loaded without `fb_plugin_register_menu` (v1 fallback), all five keyboard bindings work directly through `FB_EVENT_INPUT_EVENT`.

## Licensing Note

This plugin's C11 source code (`mods/fpv_drone/`) is dual-licensed under **MIT OR Apache-2.0** and defines only the MIT-compatible `LIBFPV_ABI_VERSION = 1` runtime interface. If you distribute a bundled copy of `libfpv.dll` alongside it, follow `hammerill/libfpv`'s `docs/PUBLISHING.md#licensing` (GPL-3.0-or-later for monolithic builds, or MIT client DLL + GPL-3.0 `libfpv-server.exe`/assets for split builds).
