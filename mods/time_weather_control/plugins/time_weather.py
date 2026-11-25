"""
time_weather — Reference Fireball-Core Tier 2 plugin state machine for clock & weather control.

Mirrors native/time_weather.c (without the Win32 window) so the behaviour can be tested
under fireball.mock_host:

  Clock   fields at resolve(clock_symbol) + clock_offset in `clock_layout`; +1h / -1h / set write
          them on the next frame; pause and time scale hold the clock (re-write every frame) at a
          time the plugin owns (frozen, or advanced at scale x 60000 / clock_ms_per_game_minute
          game seconds per wall second).
  Weather prev / next u32 slots (+ optional f32 interp) at resolve(weather_symbol) + offsets,
          holding the joaat_lower hash (or the WeatherTypes index). Instant: prev = next = target,
          interp = 0. Blend: prev = current next, next = target, interp 0 -> 1 over
          weather_blend_ms. Lock: re-assert each frame.

Commands (hotkeys, or the methods cmd_* used by the control window) are queued and applied in
FB_EVENT_FRAME_BEGIN only. Every non-hold write is read back at once and again verify_frames
later; `self.log_rows` keeps them (and log_path, if set, gets the same CSV as the native plugin).
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

from fireball.abi import (
    FB_CAP_FRAME_LIFECYCLE,
    FB_CAP_INPUT_EVENTS,
    FB_CAP_RAGE_SYMBOL_REG,
    FB_CAP_TITLE_MEM_RW,
    FB_DEV_KEYBOARD,
    FB_ERR_NOT_FOUND,
    FB_EVENT_FRAME_BEGIN,
    FB_EVENT_INPUT_EVENT,
    FB_EVENT_SHUTDOWN,
    FB_INPUT_PRESS,
    FB_LOG_INFO,
    FB_LOG_WARN,
    FB_OK,
    FB_PLUGIN_ABI_VERSION,
    FrameEvent,
    InputEvent,
)

FB_MOD_SHIFT = 1 << 0
FB_MOD_CTRL = 1 << 1
FB_MOD_ALT = 1 << 2

DAY = 86400.0
LAYOUT_FIELDS = {
    "h,m,s": 3,
    "s,m,h": 3,
    "h,m": 2,
    "m,h": 2,
    "minutes_of_day": 1,
    "seconds_of_day": 1,
    "f32_hours": 1,
}


def _f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def _bits(f: float) -> int:
    return struct.unpack("<I", struct.pack("<f", f))[0]


def decode_tod(layout: str, v: List[int]) -> Optional[float]:
    """Seconds of day from the layout's u32 fields, or None when out of range."""
    if layout == "minutes_of_day":
        return v[0] * 60.0 if v[0] < 1440 else None
    if layout == "seconds_of_day":
        return float(v[0]) if v[0] < 86400 else None
    if layout == "f32_hours":
        f = _f32(v[0])
        return f * 3600.0 if 0.0 <= f < 24.0 else None
    h, m, s = {
        "h,m,s": lambda: (v[0], v[1], v[2]),
        "s,m,h": lambda: (v[2], v[1], v[0]),
        "h,m": lambda: (v[0], v[1], 0),
        "m,h": lambda: (v[1], v[0], 0),
    }[layout]()
    if h >= 24 or m >= 60 or s >= 60:
        return None
    return h * 3600.0 + m * 60.0 + s


def encode_tod(layout: str, tod: float) -> List[Tuple[str, int]]:
    """(field name, u32) per field, in memory order."""
    tod %= DAY
    t = int(tod)
    h, m, s = t // 3600, t % 3600 // 60, t % 60
    return {
        "h,m,s": [("clock.h", h), ("clock.m", m), ("clock.s", s)],
        "s,m,h": [("clock.s", s), ("clock.m", m), ("clock.h", h)],
        "h,m": [("clock.h", h), ("clock.m", m)],
        "m,h": [("clock.m", m), ("clock.h", h)],
        "minutes_of_day": [("clock.min_of_day", t // 60)],
        "seconds_of_day": [("clock.sec_of_day", t)],
        "f32_hours": [("clock.f32_hours", _bits(tod / 3600.0))],
    }[layout]


@dataclass
class PluginInfo:
    abi_version: int
    mod_id: str
    plugin_id: str
    version: str
    required_caps: int


@dataclass
class WeatherEntry:
    index: int
    joaat_lower: int
    joaat: int
    name: str


def load_weather_list(path: str) -> List[WeatherEntry]:
    out: List[WeatherEntry] = []
    with open(path, encoding="ascii") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            idx, lower, kept, name = line.split()[:4]
            out.append(WeatherEntry(int(idx), int(lower, 16), int(kept, 16), name))
    return out


class TimeWeatherPlugin:
    def __init__(self) -> None:
        self._core: Any = None
        self.mod_id = "community.time_weather_control"
        self.plugin_id = "time_weather"
        self.cfg: Dict[str, Any] = {
            "clock_symbol": "fb.clock",
            "clock_offset": 0,
            "clock_layout": "h,m,s",
            "clock_ms_per_game_minute": 2000.0,
            "weather_symbol": "fb.weather",
            "weather_kind": "hash",
            "weather_hash_variant": "joaat_lower",
            "weather_prev_offset": 0,
            "weather_next_offset": 4,
            "weather_interp_offset": 8,
            "weather_has_interp": True,
            "weather_apply_mode": "instant",
            "weather_blend_ms": 10000.0,
            "weather_lock_on_set": True,
            "weather_list_path": "",
            "verify_frames": 30,
            "log_path": "",
            "key_hour_up": 0xDD,
            "key_hour_down": 0xDB,
            "key_pause": 0xDC,
            "key_weather_prev": 0xBA,
            "key_weather_next": 0xDE,
            "key_window": 0x77,
            "ui_enabled": False,
        }
        self.weathers: List[WeatherEntry] = []
        self.clock_va = 0
        self.prev_va = self.next_va = self.interp_va = 0
        self.clock_ok = self.weather_ok = False
        self.frame = 0
        self.paused = False
        self.scale = 1.0
        self.owned_tod = 0.0
        self.weather_target: Optional[int] = None
        self.weather_locked = False
        self.blending = False
        self.blend_elapsed_ms = 0.0
        self.blend_from = 0
        self.status: Dict[str, Any] = {}
        self.log_rows: List[Tuple[Any, ...]] = []
        self._verify: List[Tuple[int, int, int, str]] = []  # (due frame, va, expected, field)
        self._cmds: Dict[str, Any] = {}
        self.writes = 0
        self.reasserts = 0
        self.initialized = False
        self.ui_toggles = 0

    # ---- ABI ----
    def fb_plugin_query(self) -> PluginInfo:
        return PluginInfo(
            abi_version=FB_PLUGIN_ABI_VERSION,
            mod_id=self.mod_id,
            plugin_id=self.plugin_id,
            version="0.2.0",
            required_caps=FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW,
        )

    def fb_plugin_init(self, core: Any) -> int:
        self._core = core
        raw = core.config_get_json(self.mod_id)
        if raw:
            for k, v in json.loads(raw).items():
                if k in self.cfg:
                    self.cfg[k] = v
        if self.cfg["clock_layout"] not in LAYOUT_FIELDS:
            self.cfg["clock_layout"] = "h,m,s"
        if self.cfg["weather_list_path"]:
            try:
                self.weathers = load_weather_list(self.cfg["weather_list_path"])
            except OSError:
                core.log(self.mod_id, FB_LOG_WARN, f"[time_weather] weather list '{self.cfg['weather_list_path']}' not readable")
        clock = core.resolve_engine_symbol(self.cfg["clock_symbol"])
        if clock:
            self.clock_va = clock + int(self.cfg["clock_offset"])
            self.clock_ok = True
        weather = core.resolve_engine_symbol(self.cfg["weather_symbol"])
        if weather and (self.cfg["weather_kind"] == "index" or self.weathers):
            self.prev_va = weather + int(self.cfg["weather_prev_offset"])
            self.next_va = weather + int(self.cfg["weather_next_offset"])
            self.interp_va = weather + int(self.cfg["weather_interp_offset"])
            self.weather_ok = True
        if not (self.clock_ok or self.weather_ok):
            core.log(
                self.mod_id,
                FB_LOG_WARN,
                f"[time_weather] neither '{self.cfg['clock_symbol']}' nor '{self.cfg['weather_symbol']}' "
                "(with a weather list) resolved; plugin standing by.",
            )
            return FB_ERR_NOT_FOUND
        for ev in (FB_EVENT_INPUT_EVENT, FB_EVENT_FRAME_BEGIN, FB_EVENT_SHUTDOWN):
            core.register_event_listener(ev, self._on_event, self)
        self.initialized = True
        core.log(
            self.mod_id,
            FB_LOG_INFO,
            f"[time_weather] Initialized v0.2.0: clock={'on' if self.clock_ok else 'off'} ({self.cfg['clock_layout']}), "
            f"weather={'on' if self.weather_ok else 'off'} ({self.cfg['weather_kind']}, {len(self.weathers)} names)",
        )
        return FB_OK

    def fb_plugin_shutdown(self) -> None:
        if not self.initialized:
            return
        self.initialized = False
        self.paused = False
        self.scale = 1.0
        self.weather_locked = False

    # ---- commands (any caller; applied next frame) ----
    def cmd_set_tod(self, tod: float) -> None:
        self._cmds["set_tod"] = tod
        self._cmds.pop("add_hours", None)

    def cmd_add_hours(self, hours: int) -> None:
        self._cmds["add_hours"] = self._cmds.get("add_hours", 0) + hours

    def cmd_toggle_pause(self) -> None:
        self._cmds["toggle_pause"] = not self._cmds.get("toggle_pause", False)

    def cmd_set_scale(self, scale: float) -> None:
        self._cmds["scale"] = max(0.0, scale)

    def cmd_set_weather(self, list_index: int, blend: bool = False) -> None:
        self._cmds["weather"] = (list_index, blend)

    def cmd_set_lock(self, locked: bool) -> None:
        self._cmds["lock"] = bool(locked)
        self._cmds.pop("toggle_lock", None)

    def cmd_toggle_lock(self) -> None:
        self._cmds["toggle_lock"] = not self._cmds.get("toggle_lock", False)
        self._cmds.pop("lock", None)

    # ---- memory helpers ----
    def _read(self, va: int) -> Optional[int]:
        rc, v = self._core.title_mem_read_u32(va)
        return v if rc == FB_OK else None

    def _csv(self, event: str, field: str, va: int, written: int, readback: Optional[int], note: str) -> None:
        row = (self.frame, event, field, va, written, readback, note)
        self.log_rows.append(row)
        if self.cfg["log_path"]:
            with open(self.cfg["log_path"], "a", encoding="utf-8") as f:
                rb = "fault" if readback is None else "ok"
                f.write(f"{self.frame},0,{event},{field},0x{va:016X},0x{written:08X},{rb},0x{(readback or 0):08X},{note}\n")

    def _write(self, va: int, value: int, field: str, event: str, quiet: bool) -> None:
        before = self._read(va)
        if quiet and before == value:
            return
        rc = self._core.title_mem_write_u32(va, value)
        rb = self._read(va)
        if rc != FB_OK:
            self._core.log(self.mod_id, FB_LOG_WARN, f"[time_weather] write {field} @ 0x{va:016X} failed (rc={rc})")
            self._csv(event, field, va, value, rb, "write_failed")
            return
        self._csv(event, field, va, value, rb, f"before=0x{(before or 0):08X}")
        self.writes += 1
        if quiet:
            self.reasserts += 1
        else:
            self._verify.append((self.frame + int(self.cfg["verify_frames"]), va, value, field))

    def _clock_read(self) -> Optional[float]:
        n = LAYOUT_FIELDS[self.cfg["clock_layout"]]
        vals = [self._read(self.clock_va + 4 * i) for i in range(n)]
        if any(v is None for v in vals):
            return None
        return decode_tod(self.cfg["clock_layout"], vals)  # type: ignore[arg-type]

    def _clock_write(self, tod: float, event: str, quiet: bool) -> None:
        for i, (field, value) in enumerate(encode_tod(self.cfg["clock_layout"], tod)):
            self._write(self.clock_va + 4 * i, value, field, event, quiet)

    def weather_value(self, idx: int) -> int:
        if self.cfg["weather_kind"] == "index":
            return self.weathers[idx].index if self.weathers else idx
        e = self.weathers[idx]
        return e.joaat if self.cfg["weather_hash_variant"] == "joaat" else e.joaat_lower

    def weather_lookup(self, value: int) -> Optional[int]:
        for i in range(len(self.weathers)):
            if self.weather_value(i) == value:
                return i
        if not self.weathers and self.cfg["weather_kind"] == "index" and 0 <= value < 168:
            return value
        return None

    def _weather_write(self, prev: int, nxt: int, interp: float, event: str, quiet: bool) -> None:
        self._write(self.prev_va, prev, "weather.prev", event, quiet)
        self._write(self.next_va, nxt, "weather.next", event, quiet)
        if self.cfg["weather_has_interp"]:
            self._write(self.interp_va, _bits(interp), "weather.interp", event, quiet)

    # ---- events ----
    def _on_event(self, event_id: int, payload: Any, _ctx: Any) -> int:
        if not self.initialized:
            return 0
        if event_id == FB_EVENT_INPUT_EVENT and isinstance(payload, InputEvent):
            return self._input(payload)
        if event_id == FB_EVENT_FRAME_BEGIN and isinstance(payload, FrameEvent):
            self._frame(payload)
        elif event_id == FB_EVENT_SHUTDOWN:
            self.fb_plugin_shutdown()
        return 0

    def _input(self, ev: InputEvent) -> int:
        if ev.device_kind != FB_DEV_KEYBOARD or ev.action != FB_INPUT_PRESS or ev.code == 0:
            return 0
        c = self.cfg
        mods = ev.modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT)
        if self.weather_ok and ev.code == c["key_pause"] and mods == FB_MOD_CTRL:
            self.cmd_toggle_lock()
            return 1
        if mods != 0:
            return 0
        if self.clock_ok and ev.code == c["key_hour_up"]:
            self.cmd_add_hours(1)
            return 1
        if self.clock_ok and ev.code == c["key_hour_down"]:
            self.cmd_add_hours(-1)
            return 1
        if self.clock_ok and ev.code == c["key_pause"]:
            self.cmd_toggle_pause()
            return 1
        if self.weather_ok and ev.code in (c["key_weather_prev"], c["key_weather_next"]):
            n = len(self.weathers) or (168 if c["weather_kind"] == "index" else 0)
            if n:
                cur = self.status.get("weather_cur")
                base = self.weather_target if self.weather_target is not None else (cur if cur is not None else 0)
                step = 1 if ev.code == c["key_weather_next"] else -1
                blend = str(c.get("weather_apply_mode", "instant")).lower() == "blend"
                self.cmd_set_weather((base + step) % n, blend=blend)
            return 1
        if c["ui_enabled"] and ev.code == c["key_window"]:
            self.ui_toggles += 1
            return 1
        return 0

    def _frame(self, fe: FrameEvent) -> None:
        self.frame = fe.frame_index
        dt = fe.dt_sec if 0.0 <= fe.dt_sec <= 1.0 else 0.0
        c, self._cmds = self._cmds, {}

        tod = self._clock_read() if self.clock_ok else None
        if self.clock_ok:
            rate = 60000.0 / (float(self.cfg["clock_ms_per_game_minute"]) or 2000.0)
            owning = self.paused or self.scale != 1.0
            if "scale" in c:
                if not owning and tod is not None:
                    self.owned_tod = tod
                self.scale = c["scale"]
            if c.get("toggle_pause"):
                self.paused = not self.paused
                if self.paused and not owning and tod is not None:
                    self.owned_tod = tod
            owning = self.paused or self.scale != 1.0
            base = self.owned_tod if owning else tod
            explicit = False
            if "set_tod" in c:
                base, explicit = float(c["set_tod"]), True
            if c.get("add_hours") and base is not None:
                base, explicit = base + 3600.0 * c["add_hours"], True
            if explicit and base is not None:
                base %= DAY
                self._clock_write(base, "set", False)
                self.owned_tod = tod = base
            elif owning:
                if not self.paused:
                    self.owned_tod = (self.owned_tod + dt * rate * self.scale) % DAY
                self._clock_write(self.owned_tod, "hold_pause" if self.paused else "hold_scale", True)
                tod = self.owned_tod

        nxt_now = self._read(self.next_va) if self.weather_ok else None
        if self.weather_ok:
            if "lock" in c:
                self.weather_locked = c["lock"]
            if c.get("toggle_lock"):
                self.weather_locked = not self.weather_locked
            if self.weather_locked and self.weather_target is None and nxt_now is not None:
                self.weather_target = self.weather_lookup(nxt_now)
            n = len(self.weathers) or (168 if self.cfg["weather_kind"] == "index" else 0)
            if "weather" in c and 0 <= c["weather"][0] < n:
                idx, blend = c["weather"]
                tv = self.weather_value(idx)
                self.weather_target = idx
                if self.cfg["weather_lock_on_set"]:
                    self.weather_locked = True
                if blend and self.cfg["weather_has_interp"]:
                    self.blending, self.blend_elapsed_ms = True, 0.0
                    self.blend_from = nxt_now if nxt_now is not None else tv
                    self._weather_write(self.blend_from, tv, 0.0, "weather_blend", False)
                else:
                    self.blending = False
                    self._weather_write(tv, tv, 0.0, "weather_set", False)
            elif self.blending and self.weather_target is not None:
                tv = self.weather_value(self.weather_target)
                self.blend_elapsed_ms += dt * 1000.0
                ms = float(self.cfg["weather_blend_ms"])
                p = self.blend_elapsed_ms / ms if ms > 0 else 1.0
                if p >= 1.0:
                    self.blending = False
                    self._weather_write(tv, tv, 0.0, "weather_blend_done", False)
                else:
                    self._weather_write(self.blend_from, tv, p, "hold_blend", True)
            elif self.weather_locked and self.weather_target is not None:
                tv = self.weather_value(self.weather_target)
                self._weather_write(tv, tv, 0.0, "hold_lock", True)

        due = [v for v in self._verify if v[0] <= self.frame]
        self._verify = [v for v in self._verify if v[0] > self.frame]
        for _, va, expected, field in due:
            rb = self._read(va)
            self._csv("verify", field, va, expected, rb, "fault" if rb is None else ("kept" if rb == expected else "changed"))

        self.status = {
            "clock_ok": tod is not None,
            "tod_sec": tod,
            "paused": self.paused,
            "scale": self.scale,
            "weather_cur": self.weather_lookup(nxt_now) if nxt_now is not None else None,
            "weather_target": self.weather_target,
            "weather_locked": self.weather_locked,
            "blending": self.blending,
        }


def create_plugin() -> TimeWeatherPlugin:
    return TimeWeatherPlugin()
