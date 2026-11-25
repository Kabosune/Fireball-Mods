# Time & Weather Control (`community.time_weather_control`)

Strictly offline / single-player Tier 2 native C11 plugin (`time_weather.fbplugin.dll`) for controlling the in-game clock and cycling, blending, and locking weather presets through guarded title memory.

## Mod Menu Features (`F1` / `LB + View`)

- **Clock Section**:
  - Live `Clock` status row (`HH:MM:SS`, pause state, scale, write count).
  - `clock_paused` (`BOOL`) and `time_scale` (`ENUM`: `1x`, `0.25x`, `0.5x`, `2x`, `5x`, `10x`, `30x`, `60x`, `Real time`), bidirectionally synced with hotkeys and the optional legacy window.
  - Controller-friendly `clock_hour` (`0..23`) + `clock_minute` (`0..55`, step 5) + `Apply Time Now` button (staged values never clobber the game clock on boot).
  - Quick time-of-day preset buttons: `Dawn (06:00)`, `Noon (12:00)`, `Sunset (18:00)`, `Midnight (00:00)`, plus `-1h` / `+1h` buttons.
- **Weather Section**:
  - Live `Weather` status row (current weather, held target, `[locked]`, `[blending]`).
  - `weather_preset` (`ENUM`), `weather_apply_mode` (`ENUM`: `Instant` / `Blend`, respected by `weather_prev` / `weather_next`), `weather_blend_ms` (`FLOAT`), `weather_lock_on_set` (`BOOL`), `weather_locked` (`BOOL`), plus `Apply Weather Now` and `Blend to Weather` buttons.

## Default Controls

| Action | Keyboard | Gamepad |
| :--- | :--- | :--- |
| `hour_up` (Clock +1 Hour) | `]` | `LB + DPad Up` |
| `hour_down` (Clock -1 Hour) | `[` | `LB + DPad Down` |
| `toggle_pause` (Pause / Resume Clock) | `\` | `LB + Start` |
| `weather_prev` (Previous Weather) | `;` | `LB + DPad Left` |
| `weather_next` (Next Weather) | `'` | `LB + DPad Right` |
| `toggle_weather_lock` (Lock / Unlock Weather) | `Ctrl + \` | `LB + Y` |
| `toggle_window` (Legacy Win32 Window, opt-in `ui_enabled = true`) | `F8` | Unbound |

The legacy standalone Win32 `F8` window (`time_weather_ui.c`) is opt-in (`ui_enabled = false` by default in `mod.toml`) and remains synchronized with the Mod Menu if enabled.
