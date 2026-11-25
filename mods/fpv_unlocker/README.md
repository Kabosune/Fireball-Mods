# FPV Unlocker (`community.fpv_unlocker`)

Unlocks and tunes *Grand Theft Auto VI*'s shipped on-foot First-Person View (`enum 0x4BFBE561 = 2`) with Tier 1 XML/KV/PSO patches and a Tier 2 native C11 plugin (`fpv_toggle.fbplugin.dll`).

## Mod Menu Features (`F1` / `LB + View`)

- **Live Status**: shows current latch/peek state, last-read `view_mode` (`ThirdPerson`, `Cinematic`, `FirstPerson`, or `raw=<n>`), toggle count, and hold-guard re-assert count.
- **Camera Settings**:
  - `hold_guard_enabled` (`BOOL`, persisted, default `true`): re-asserts `FirstPerson` if the game resets `view_mode` while latched ON.
  - `start_in_fpv` (`BOOL`, persisted, default `false`): automatically latches `FirstPerson` on the first valid frame.
  - `restore_on_shutdown` (`BOOL`, persisted, default `true`): restores `ThirdPerson` cleanly on shutdown if latched.
  - `verbose_reassert_log` (`BOOL`, persisted, default `false`): suppresses per-frame hold-guard re-assert logs unless enabled.
  - `view_mode_preset` (`ENUM`: `ThirdPerson` / `Cinematic` / `FirstPerson`): directly switches camera mode and stays synced with actions/buttons.
  - `toggle_now` (`BUTTON`): immediately toggles `ThirdPerson` <-> `FirstPerson`.

## Default Controls

| Action | Mode | Keyboard | Gamepad |
| :--- | :--- | :--- | :--- |
| `toggle_fpv` | Press | `V` | `LB + R3` |
| `peek_fpv` | Hold | `Alt + V` | `LB + L3` |

When loaded without `fb_plugin_register_menu` (v1 fallback), `V` toggles and `Alt + V` peeks directly through `FB_EVENT_INPUT_EVENT`.
