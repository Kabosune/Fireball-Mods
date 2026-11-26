# ROADMAP

The lasting plan and status for Fireball-Mods. Rules and constraints live in `AGENTS.md` and aren't repeated here.

**How to use:** start a session with "read ROADMAP.md and do the next item". Agents tick boxes as they go and fill in **Handoff** notes before stopping (see `AGENTS.md` → Workflow).

Status values: `Done` · `In progress` · `Next` · `Planned` · `Idea`

---

## Done

| Item | Status | Commit | Summary |
| :--- | :--- | :--- | :--- |
| Bootstrap | Done | `f8cdb08` | Repo scaffold, CMake against the Fireball-Core SDK, upgraded `fpv_unlocker` + `time_weather_control`, `stage_mods.py`, native + Python tests |
| FPV Quadcopter Drone (`community.fpv_drone`) | Done | `10dde1a` | Pure C11 runtime `libfpv` ABI v1 loader, RAGE 9 Z-up coordinates, multi-view camera sync, stereo `waveOut` motor audio, opt-in MAX7456 OSD preview, mock `libfpv` + native/Python tests |
| Quake Movement (`community.quake_movement`) | Done | *(this commit)* | Pure C11 Z-up movement physics engine (`VQ3` / `CPMA` / `Half-Life` / `Apex Legends` / `Deadlock` / `Custom`), Mod Menu sliders & speed readout, console commands (`qm`), guarded Tier 2 `CPed` I/O, unit + E2E + Python tests |

---

## Follow-ups & Handoff Notes

### Quake Movement (`community.quake_movement`)
- **Left (live game integration):** Populate live `CPed` RVAs (`fb.player_pos`, `fb.player_vel`, `fb.player_flags`, `fb.camera_basis`) in `victory.toml` once a live VIctory `state_scan` + `state_scan_diff.py ped-move` session confirms them (see `C:\dev\gta6-modding\docs\ped_runtime_movement.md`), and wire `qm_input_t.wall_probe` / `slope_normal` to `T049` static collision triangles or a native RAGE 9 raycast when confirmed.
- **Gotchas / Fireball-Core SDK follow-ups:**
  - Plugin degrades to safe no-op when `fb.player_vel` or `fb.player_flags` is unresolved, and skips velocity writes whenever `dead`, `ragdoll`, or `in_vehicle` flags are set.
  - `FB_EVENT_INPUT_EVENT` observes `W/A/S/D/Space/Ctrl/C` passively (`return 0`) so native animations/gameplay input stay intact; only bound action chords (`F6`, `Ctrl+P`, `Shift+Q` and gamepad `RB+A/Y/B`) are consumed.
  - Analog gamepad left-stick / trigger axes and mouse-wheel scroll events are not yet exposed on `fb_input_event_v1_t`; controller locomotion & scroll-wheel tap-strafe will need an additive Fireball-Core input event extension.

### FPV Quadcopter Drone (`community.fpv_drone`)
- **Left (live game integration):** Replace placeholder title symbols (`fb.drone_camera`, `fb.drone_camera_override`, `fb.pilot_pos`, `fb.streaming_focus`, `fb.drone_pose`, `fb.wind`) once live VIctory scans in `C:\dev\gta6-modding` confirm RVAs; install a MinGW-built x64 `libfpv.dll` (and `libfpv-server.exe` + assets for split builds) into the staging folder next to `fpv_drone.fbplugin.dll`.
- **Gotchas / Fireball-Core SDK follow-ups:**
  - `FB_EVENT_FRAME_PRESENT` currently passes only `void* d3d12_swapchain_or_null`. To draw `fpv_osd_draw_data()` / `fpv_overlay_render()` textured triangles and the fisheye/goggle video feed shader directly onto the D3D12 back buffer (via D3D12 or D3D11On12 as in `rdr2-fpv`), Fireball-Core will need a future additive SDK extension exposing the `ID3D12CommandQueue*` alongside the swapchain.
  - Real world wall/building/vehicle collision requires a RAGE 9 physics line-probe hook (`fpv_world_set_raycast_callback_ex` currently uses a configurable ground-plane fallback until a native RAGE 9 raycast symbol is confirmed in `gta6-modding`).
  - `mods/forT3Code/` (untracked RDR2 reference folder placed by the user) is left untouched and ignored by `scripts/stage_mods.py` (no `mod.toml`).

---

## Ideas / open questions
_(Candidates, not decisions.)_
- `time_weather_control`: replace the placeholder clock/weather addresses once a live scan confirms them (research lives in `C:\dev\gta6-modding`).
- Once Fireball-Core Phase 3 ships `fireball_cli.py stage` (load-order resolver + conflict report), switch `scripts/stage_mods.py` to use it instead of its own staging logic.
- Fireball-Core `examples/` has older copies of both mods. Decide which copy is canonical.
- CI for this repo (Fireball-Core Phase 3 adds one there). It would need to check out and build Fireball-Core first.
