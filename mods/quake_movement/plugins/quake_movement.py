"""
quake_movement — Reference Fireball-Core Plugin State Machine for Quake Movement.

Mirrors the lifecycle, presets, Z-up movement physics, guarded Tier 2 memory I/O
(`fb.player_pos`, `fb.player_vel`, `fb.player_flags`, `fb.camera_basis`), safety guards
(never write when dead/ragdolled/in-vehicle or unresolved), and v1 fallback hotkeys
implemented in `native/quake_movement.c` and `native/qm_physics.c`.

Stdlib-only Python 3.13.
"""

from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

from fireball.abi import (
    FB_CAP_FRAME_LIFECYCLE,
    FB_CAP_INPUT_EVENTS,
    FB_DEV_KEYBOARD,
    FB_EVENT_FRAME_BEGIN,
    FB_EVENT_INPUT_EVENT,
    FB_EVENT_SHUTDOWN,
    FB_INPUT_PRESS,
    FB_INPUT_REPEAT,
    FB_LOG_INFO,
    FB_OK,
    FB_PLUGIN_ABI_VERSION,
    FrameEvent,
    InputEvent,
)

FB_MOD_SHIFT = 1 << 0
FB_MOD_CTRL = 1 << 1
FB_MOD_ALT = 1 << 2

FLAG_ON_GROUND = 0x00000001
FLAG_CROUCHED = 0x00000002
FLAG_IN_VEHICLE = 0x00000004
FLAG_RAGDOLL = 0x00000008
FLAG_DEAD = 0x00000010

PRESET_ORDER = (
    "quake_vq3",
    "quake_cpma",
    "half_life",
    "apex_legends",
    "deadlock",
    "custom",
)


def _f32_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", float(value)))[0]


def _bits_f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


@dataclass
class PluginInfo:
    abi_version: int
    mod_id: str
    plugin_id: str
    version: str
    required_caps: int


@dataclass
class PresetParams:
    max_speed: float = 9.5
    ground_accel: float = 10.0
    air_accel: float = 1.0
    air_wish_cap: float = 9.5
    air_control: float = 0.0
    friction: float = 6.0
    stop_speed: float = 2.5
    gravity: float = 20.0
    jump_speed: float = 7.0
    auto_bhop: bool = True
    slide_enabled: bool = False
    slide_boost: float = 3.5
    slide_slope_accel: float = 16.0
    dash_enabled: bool = False
    dash_impulse: float = 13.5
    dash_max_stamina: int = 3


def preset_defaults(name: str) -> PresetParams:
    if name == "quake_cpma":
        return PresetParams(max_speed=10.0, ground_accel=15.0, air_accel=1.0, air_wish_cap=10.0,
                            air_control=6.0, friction=8.0, gravity=20.0, jump_speed=7.2, auto_bhop=True)
    if name == "half_life":
        return PresetParams(max_speed=8.5, ground_accel=10.0, air_accel=120.0, air_wish_cap=0.95,
                            friction=4.0, gravity=18.5, jump_speed=6.6, auto_bhop=True)
    if name == "apex_legends":
        return PresetParams(max_speed=7.5, ground_accel=11.0, air_accel=45.0, air_wish_cap=1.1,
                            friction=5.5, gravity=19.0, jump_speed=6.8, auto_bhop=False,
                            slide_enabled=True, slide_boost=3.8, slide_slope_accel=16.0)
    if name == "deadlock":
        return PresetParams(max_speed=7.2, ground_accel=12.0, air_accel=35.0, air_wish_cap=1.1,
                            friction=6.5, gravity=19.6, jump_speed=6.5, auto_bhop=False,
                            slide_enabled=True, slide_boost=1.5, slide_slope_accel=15.0,
                            dash_enabled=True, dash_impulse=13.5, dash_max_stamina=3)
    if name == "custom":
        return PresetParams(max_speed=9.0, ground_accel=12.0, air_accel=20.0, air_wish_cap=2.0,
                            air_control=2.5, friction=6.0, gravity=19.6, jump_speed=7.0, auto_bhop=True,
                            slide_enabled=True, slide_boost=3.0, slide_slope_accel=15.0,
                            dash_enabled=True, dash_impulse=13.0, dash_max_stamina=3)
    return PresetParams()


class QuakeMovementPlugin:
    def __init__(self) -> None:
        self._core: Any = None
        self.mod_id: str = "community.quake_movement"
        self.plugin_id: str = "quake_movement"

        self.player_pos_symbol: str = "fb.player_pos"
        self.player_vel_symbol: str = "fb.player_vel"
        self.player_flags_symbol: str = "fb.player_flags"
        self.camera_basis_symbol: str = "fb.camera_basis"
        self.camera_layout: str = "mat34"

        self.key_toggle: int = 0x75        # F6
        self.key_cycle_preset: int = 0x50  # Ctrl + P
        self.key_dash: int = 0x51          # Shift + Q

        self.player_pos_va: int = 0
        self.player_vel_va: int = 0
        self.player_flags_va: int = 0
        self.camera_basis_va: int = 0

        self.initialized: bool = False
        self.enabled: bool = False
        self.preset: str = "quake_vq3"
        self.params: PresetParams = preset_defaults(self.preset)
        self.stamina: float = float(self.params.dash_max_stamina)
        self.vel: List[float] = [0.0, 0.0, 0.0]
        self.horiz_speed: float = 0.0
        self.vel_writes: int = 0
        self.guard_skips: int = 0

        self._keys: Dict[int, bool] = {}
        self._pending_jump: bool = False
        self._pending_dash: bool = False

    def fb_plugin_query(self) -> PluginInfo:
        return PluginInfo(
            abi_version=FB_PLUGIN_ABI_VERSION,
            mod_id=self.mod_id,
            plugin_id=self.plugin_id,
            version="0.1.0",
            required_caps=FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS,
        )

    def fb_plugin_init(self, core: Any) -> int:
        self._core = core
        cfg_raw = core.config_get_json(self.mod_id)
        if cfg_raw:
            cfg: Dict[str, Any] = json.loads(cfg_raw)
            self.enabled = bool(cfg.get("start_enabled", self.enabled))
            preset = str(cfg.get("default_preset", self.preset))
            if preset in PRESET_ORDER:
                self.set_preset(preset)
            if "auto_bhop" in cfg:
                self.params.auto_bhop = bool(cfg["auto_bhop"])
            self.player_pos_symbol = str(cfg.get("player_pos_symbol", self.player_pos_symbol))
            self.player_vel_symbol = str(cfg.get("player_vel_symbol", self.player_vel_symbol))
            self.player_flags_symbol = str(cfg.get("player_flags_symbol", self.player_flags_symbol))
            self.camera_basis_symbol = str(cfg.get("camera_basis_symbol", self.camera_basis_symbol))
            self.camera_layout = str(cfg.get("camera_layout", self.camera_layout))

        self.player_pos_va = core.resolve_engine_symbol(self.player_pos_symbol)
        self.player_vel_va = core.resolve_engine_symbol(self.player_vel_symbol)
        self.player_flags_va = core.resolve_engine_symbol(self.player_flags_symbol)
        self.camera_basis_va = core.resolve_engine_symbol(self.camera_basis_symbol)

        core.register_event_listener(FB_EVENT_INPUT_EVENT, self._on_event, self)
        core.register_event_listener(FB_EVENT_FRAME_BEGIN, self._on_event, self)
        core.register_event_listener(FB_EVENT_SHUTDOWN, self._on_event, self)
        self.initialized = True
        core.log(self.mod_id, FB_LOG_INFO, "[quake_movement] Reference state machine initialized.")
        return FB_OK

    def fb_plugin_shutdown(self) -> None:
        self.enabled = False
        self.initialized = False

    def set_preset(self, preset: str) -> None:
        if preset in PRESET_ORDER:
            self.preset = preset
            self.params = preset_defaults(preset)
            self.stamina = float(self.params.dash_max_stamina)

    def cycle_preset(self) -> str:
        idx = PRESET_ORDER.index(self.preset)
        nxt = PRESET_ORDER[(idx + 1) % len(PRESET_ORDER)]
        self.set_preset(nxt)
        return nxt

    def _read_f32(self, va: int) -> Optional[float]:
        if not va:
            return None
        rc, bits = self._core.title_mem_read_u32(va)
        if rc != FB_OK:
            return None
        val = _bits_f32(bits)
        return val if math.isfinite(val) else None

    def _write_f32(self, va: int, val: float) -> bool:
        if not va or not math.isfinite(val):
            return False
        return self._core.title_mem_write_u32(va, _f32_bits(val)) == FB_OK

    def _read_yaw(self) -> float:
        if not self.camera_basis_va:
            return 0.0
        if self.camera_layout == "yaw_f32":
            y = self._read_f32(self.camera_basis_va)
            return y if y is not None else 0.0
        off = 0x10 if self.camera_layout == "mat34" else 0x00
        fx = self._read_f32(self.camera_basis_va + off)
        fy = self._read_f32(self.camera_basis_va + off + 4)
        if fx is None or fy is None or math.hypot(fx, fy) < 1e-4:
            return 0.0
        return math.atan2(-fx, fy)

    def _on_event(self, event_id: int, payload: Any, _ctx: Any) -> int:
        if not self.initialized:
            return 0
        if event_id == FB_EVENT_INPUT_EVENT and isinstance(payload, InputEvent):
            return self._handle_input(payload)
        if event_id == FB_EVENT_FRAME_BEGIN and isinstance(payload, FrameEvent):
            self._handle_frame(payload)
            return 0
        if event_id == FB_EVENT_SHUTDOWN:
            self.fb_plugin_shutdown()
            return 0
        return 0

    def _handle_input(self, ev: InputEvent) -> int:
        if ev.device_kind != FB_DEV_KEYBOARD:
            return 0
        pressed = ev.action == FB_INPUT_PRESS
        held = ev.action in (FB_INPUT_PRESS, FB_INPUT_REPEAT)
        mods = ev.modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT)

        if pressed and mods == 0 and ev.code == self.key_toggle:
            self.enabled = not self.enabled
            return 1
        if pressed and mods == FB_MOD_CTRL and ev.code == self.key_cycle_preset:
            self.cycle_preset()
            return 1
        if pressed and mods == FB_MOD_SHIFT and ev.code == self.key_dash:
            if self.enabled:
                self._pending_dash = True
                return 1
            return 0

        if ev.code in (0x57, 0x53, 0x41, 0x44, 0x20, 0x11, 0x43):
            if ev.code == 0x20 and pressed and not self._keys.get(0x20, False):
                self._pending_jump = True
            self._keys[ev.code] = held
        return 0

    def _handle_frame(self, frame: FrameEvent) -> None:
        jump_pressed = self._pending_jump
        dash_pressed = self._pending_dash
        self._pending_jump = False
        self._pending_dash = False

        if not self.enabled or not self.player_vel_va or not self.player_flags_va:
            return

        rc, flags = self._core.title_mem_read_u32(self.player_flags_va)
        if rc != FB_OK or (flags & (FLAG_DEAD | FLAG_RAGDOLL | FLAG_IN_VEHICLE)) != 0:
            self.guard_skips += 1
            return

        vx = self._read_f32(self.player_vel_va)
        vy = self._read_f32(self.player_vel_va + 4)
        vz = self._read_f32(self.player_vel_va + 8)
        if vx is None or vy is None or vz is None:
            self.guard_skips += 1
            return

        dt = frame.dt_sec if 0.0 < frame.dt_sec <= 0.1 else (1.0 / 60.0)
        yaw = self._read_yaw()
        fwd = (-math.sin(yaw), math.cos(yaw))
        right = (math.cos(yaw), math.sin(yaw))

        wf = (1.0 if self._keys.get(0x57) else 0.0) - (1.0 if self._keys.get(0x53) else 0.0)
        wr = (1.0 if self._keys.get(0x44) else 0.0) - (1.0 if self._keys.get(0x41) else 0.0)
        wx = fwd[0] * wf + right[0] * wr
        wy = fwd[1] * wf + right[1] * wr
        wlen = math.hypot(wx, wy)
        wishdir = (wx / wlen, wy / wlen) if wlen > 1e-4 else (0.0, 0.0)
        wishspeed = self.params.max_speed * min(1.0, wlen)

        on_ground = (flags & FLAG_ON_GROUND) != 0

        if self.params.dash_enabled and dash_pressed and self.stamina >= 1.0:
            self.stamina -= 1.0
            ddir = wishdir if wlen > 1e-4 else fwd
            spd = max(math.hypot(vx, vy), self.params.dash_impulse)
            vx, vy = ddir[0] * spd, ddir[1] * spd

        if on_ground and (jump_pressed or (self.params.auto_bhop and self._keys.get(0x20, False))):
            vz = self.params.jump_speed
            on_ground = False

        if on_ground:
            spd = math.hypot(vx, vy)
            if spd > 1e-4:
                control = max(spd, self.params.stop_speed)
                new_spd = max(0.0, spd - control * self.params.friction * dt)
                vx *= new_spd / spd
                vy *= new_spd / spd
            if wlen > 1e-4:
                cur = vx * wishdir[0] + vy * wishdir[1]
                add = wishspeed - cur
                if add > 0.0:
                    acc = min(add, self.params.ground_accel * dt * wishspeed)
                    vx += acc * wishdir[0]
                    vy += acc * wishdir[1]
        else:
            if wlen > 1e-4:
                cap = min(wishspeed, self.params.air_wish_cap)
                cur = vx * wishdir[0] + vy * wishdir[1]
                add = cap - cur
                if add > 0.0:
                    acc = min(add, self.params.air_accel * dt * wishspeed)
                    vx += acc * wishdir[0]
                    vy += acc * wishdir[1]
            vz -= self.params.gravity * dt

        self.vel = [vx, vy, vz]
        self.horiz_speed = math.hypot(vx, vy)
        if (
            self._write_f32(self.player_vel_va, vx)
            and self._write_f32(self.player_vel_va + 4, vy)
            and self._write_f32(self.player_vel_va + 8, vz)
        ):
            self.vel_writes += 1


def create_plugin() -> QuakeMovementPlugin:
    return QuakeMovementPlugin()
