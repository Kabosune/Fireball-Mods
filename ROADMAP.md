# ROADMAP

The lasting plan and status for Fireball-Mods. Rules and constraints live in `AGENTS.md` and aren't repeated here.

**How to use:** start a session with "read ROADMAP.md and do the next item". Agents tick boxes as they go and fill in **Handoff** notes before stopping (see `AGENTS.md` → Workflow).

Status values: `Done` · `In progress` · `Next` · `Planned` · `Idea`

---

## Done

| Item | Status | Commit | Summary |
| :--- | :--- | :--- | :--- |
| Bootstrap | Done | `f8cdb08` | Repo scaffold, CMake against the Fireball-Core SDK, upgraded `fpv_unlocker` + `time_weather_control`, `stage_mods.py`, native + Python tests |
| FPV Quadcopter Drone (`community.fpv_drone`) | Done | *(this commit)* | Pure C11 runtime `libfpv` ABI v1 loader, RAGE 9 Z-up coordinates, multi-view camera sync, stereo `waveOut` motor audio, opt-in MAX7456 OSD preview, mock `libfpv` + native/Python tests |

---

## Follow-ups & Handoff Notes

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
