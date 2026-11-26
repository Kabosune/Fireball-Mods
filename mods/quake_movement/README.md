# Quake Movement (`community.quake_movement`)

Strictly offline / single-player Quake-style on-foot movement mod for *Grand Theft Auto VI* (Project Americas / RAGE 9), built in pure C11 (`/std:c11 /W4 /WX`, zero third-party dependencies) against Fireball-Core's public C11 SDK (`fireball_plugin.h`, `fireball_menu.h`, `fireball_console.h`).

## Presets

Switch live via hotkey (`Ctrl + P` / `RB + Y`), the Fireball Mod Menu (`F1` / `LB + View`), or console (`qm preset <name>`):

| Preset | Key | Core Feel & Mechanics |
| :--- | :--- | :--- |
| **Quake VQ3** | `vq3` / `quake_vq3` | Classic Q3A strafe-jumping (`PM_AirAccelerate`, uncapped air wishspeed), crisp ground acceleration & friction, auto-bhop option. |
| **Quake CPMA** | `cpma` / `quake_cpma` | VQ3 strafe-jumping + snappy ground acceleration + CPMA air-control turning (`W`/`A`/`D`) and air-stop braking. |
| **Half-Life** | `halflife` / `half_life` | GoldSrc / Source low air-wishspeed cap (`0.95 m/s`), high air acceleration (`120.0`) bunnyhopping, lower friction, and crouch-jump boost. |
| **Apex Legends** | `apex` / `apex_legends` | Sprint-to-crouch flat slide boost + cooldown, downhill slope acceleration via surface normal, slide-jump bursts, post-jump tap-strafe air-lurch window, and wall-bounce hook. |
| **Deadlock** | `deadlock` | Directional ground/air dash, 3 stamina pips with timed recharge, timed dash-jump momentum burst, downhill crouch-slide, and wall-bounce hook. |
| **Custom** | `custom` | Full sandbox preset exposing live Mod Menu sliders (`max_speed`, `ground_accel`, `air_accel`, `air_wish_cap`, `friction`, `gravity`, `jump_speed`, `slide_boost`, `dash_impulse`, `auto_bhop`). |

## Architecture & RAGE 9 Coordinates

- **Coordinate Frame:** Right-handed Z-up in meters (`+X` East, `+Y` North, `+Z` Up), matching RAGE 9 (`CClimbTuneMgr` baseline `fGravity = -9.8 m/s²`, shipped sprint `6.0–7.2 m/s`, jump launch `vz = 1.82–4.00 m/s`).
- **Decoupled Solver (`native/qm_physics.h/.c`):** Deterministic pure-C11 physics step taking `(dt_sec, wish_forward, wish_right, cam_yaw_rad, jump/crouch/dash inputs, on_ground, slope_normal, optional wall_probe callback)` with zero Win32 dependencies.
- **Guarded Tier 2 Memory I/O (`native/quake_movement.c`):**
  - Binds to `fb.player_pos`, `fb.player_vel`, `fb.player_flags`, and `fb.camera_basis` (`mat34`, `yaw_f32`, or `forward_vec3`).
  - Live `CPed` RVAs await a VIctory `state_scan` + `state_scan_diff.py ped-move` session (see `C:\dev\gta6-modding\docs\ped_runtime_movement.md`).
  - **Safe degradation:** Never writes to title memory when symbols are unresolved or when `fb.player_flags` indicates the player is dead, ragdolled, or in a vehicle.
  - **Graceful ABI v1 fallback:** Continues working when loaded without `fb_plugin_register_menu` or `fb_plugin_register_console`.

## Default Controls

All default keyboard and gamepad chords are conflict-free across `fireball.core`, `community.fpv_unlocker`, `community.time_weather_control`, and `community.fpv_drone`, and can be remapped live in the Fireball Mod Menu (`F1` / `LB + View`):

| Action | Trigger | Keyboard | Gamepad |
| :--- | :--- | :--- | :--- |
| Toggle Quake Movement (`toggle_movement`) | Press | `F6` | `RB + A` |
| Cycle Movement Preset (`cycle_preset`) | Press | `Ctrl + P` | `RB + Y` |
| Directional Dash (`dash`) | Press | `Shift + Q` | `RB + B` |

## Console Commands (`~`)

- `qm toggle` / `qm on` / `qm off` — toggle or explicitly set movement override.
- `qm preset <vq3|cpma|halflife|apex|deadlock|custom>` — switch active movement preset.
- `qm set <param> <value>` — adjust live physics parameters (`max_speed`, `ground_accel`, `air_accel`, `air_wish_cap`, `air_control`, `friction`, `gravity`, `jump_speed`, `auto_bhop`, `slide_boost`, `dash_impulse`, etc.).
- `qm status` — print enabled state, active preset, horizontal/vertical/peak speed (`m/s`), stamina pips, and Tier 2 guard status.
