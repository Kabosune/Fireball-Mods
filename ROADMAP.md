# ROADMAP

The lasting plan and status for Fireball-Mods. Rules and constraints live in `AGENTS.md` and aren't repeated here.

**How to use:** start a session with "read ROADMAP.md and do the next item". Agents tick boxes as they go and fill in **Handoff** notes before stopping (see `AGENTS.md` → Workflow).

Status values: `Done` · `In progress` · `Next` · `Planned` · `Idea`

---

## Done

| Item | Status | Commit | Summary |
| :--- | :--- | :--- | :--- |
| Bootstrap | Done | `f8cdb08` | Repo scaffold, CMake against the Fireball-Core SDK, upgraded `fpv_unlocker` + `time_weather_control`, `stage_mods.py`, native + Python tests |

---

## Next

_Not planned yet. When the user agrees on the next item, write its spec here: deliverables as checkboxes, a Verification list, and an empty Handoff section._

---

## Ideas / open questions
_(Candidates, not decisions.)_
- `time_weather_control`: replace the placeholder clock/weather addresses once a live scan confirms them (research lives in `C:\dev\gta6-modding`).
- Once Fireball-Core Phase 3 ships `fireball_cli.py stage` (load-order resolver + conflict report), switch `scripts/stage_mods.py` to use it instead of its own staging logic.
- Fireball-Core `examples/` has older copies of both mods. Decide which copy is canonical.
- CI for this repo (Fireball-Core Phase 3 adds one there). It would need to check out and build Fireball-Core first.
