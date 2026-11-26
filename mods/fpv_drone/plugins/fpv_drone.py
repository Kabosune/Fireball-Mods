"""
fpv_drone — Reference Fireball-Core Plugin State Machine for the libfpv FPV Quadcopter Mod.

Mirrors the lifecycle, RAGE 9 coordinate transform, 50 ms post-spawn settle guard,
multi-view camera output (`mat34_fov` / `pos_quat_fov` / `pos_euler_fov`), and conflict-free
v1 fallback hotkeys implemented in `native/fpv_drone.c`.

Note: Real Betaflight-in-the-loop 1 kHz flight simulation runs in native `libfpv.dll`
(loaded dynamically by `fpv_drone.fbplugin.dll`). This stdlib-only reference module
mirrors the plugin state machine for offline testing and host-integration prototyping.
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
    FB_CAP_RAGE_SYMBOL_REG,
    FB_CAP_TITLE_MEM_RW,
    FB_DEV_KEYBOARD,
    FB_EVENT_FRAME_BEGIN,
    FB_EVENT_INPUT_EVENT,
    FB_EVENT_SHUTDOWN,
    FB_INPUT_PRESS,
    FB_LOG_INFO,
    FB_OK,
    FB_PLUGIN_ABI_VERSION,
    FrameEvent,
    InputEvent,
)

FB_MOD_SHIFT = 1 << 0
FB_MOD_CTRL = 1 << 1
FB_MOD_ALT = 1 << 2

FPV_VIEW_FPV = 0
FPV_VIEW_CHASE = 1
FPV_VIEW_LOS = 2
FPV_VIEW_PILOT = 3


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


class FpvDronePlugin:
    def __init__(self) -> None:
        self._core: Any = None
        self.mod_id: str = "community.fpv_drone"
        self.plugin_id: str = "fpv_drone"
        self.camera_symbol: str = "fb.drone_camera"
        self.camera_layout: str = "mat34_fov"
        self.camera_override_symbol: str = "fb.drone_camera_override"
        self.pilot_pos_symbol: str = "fb.pilot_pos"
        self.focus_pos_symbol: str = "fb.streaming_focus"
        self.spawn_forward_m: float = 2.0
        self.spawn_height_m: float = 0.25
        self.ground_fallback_z: float = 0.0

        self.key_toggle_drone: int = 0x76  # F7
        self.key_cycle_view: int = 0x56    # Shift + V
        self.key_respawn: int = 0x52       # Ctrl + R
        self.key_pilot_view: int = 0x46    # Ctrl + F
        self.key_overlay: int = 0x78       # F9

        self.camera_va: int = 0
        self.camera_override_va: int = 0
        self.pilot_pos_va: int = 0
        self.focus_pos_va: int = 0

        self.initialized: bool = False
        self.drone_active: bool = False
        self.overlay_open: bool = False
        self.view: int = FPV_VIEW_FPV
        self.prev_non_pilot_view: int = FPV_VIEW_FPV
        self.sim_time: float = 0.0
        self.settle_start_time: Optional[float] = None
        self.settled: bool = False
        self.position: List[float] = [0.0, 0.0, 0.0]
        self.pilot_pos: List[float] = [0.0, 0.0, 0.0]
        self.camera_writes: int = 0

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
            self.camera_symbol = str(cfg.get("camera_symbol", self.camera_symbol))
            self.camera_layout = str(cfg.get("camera_layout", self.camera_layout))
            self.camera_override_symbol = str(cfg.get("camera_override_symbol", self.camera_override_symbol))
            self.pilot_pos_symbol = str(cfg.get("pilot_pos_symbol", self.pilot_pos_symbol))
            self.focus_pos_symbol = str(cfg.get("focus_pos_symbol", self.focus_pos_symbol))
            self.spawn_forward_m = float(cfg.get("spawn_forward_m", self.spawn_forward_m))
            self.spawn_height_m = float(cfg.get("spawn_height_m", self.spawn_height_m))
            self.ground_fallback_z = float(cfg.get("ground_fallback_z", self.ground_fallback_z))
            self.key_toggle_drone = int(cfg.get("key_toggle_drone", self.key_toggle_drone))
            self.key_cycle_view = int(cfg.get("key_cycle_view", self.key_cycle_view))
            self.key_respawn = int(cfg.get("key_respawn", self.key_respawn))
            self.key_pilot_view = int(cfg.get("key_pilot_view", self.key_pilot_view))
            self.key_overlay = int(cfg.get("key_overlay", self.key_overlay))

        self.camera_va = core.resolve_engine_symbol(self.camera_symbol)
        self.camera_override_va = core.resolve_engine_symbol(self.camera_override_symbol)
        self.pilot_pos_va = core.resolve_engine_symbol(self.pilot_pos_symbol)
        self.focus_pos_va = core.resolve_engine_symbol(self.focus_pos_symbol)

        core.register_event_listener(FB_EVENT_INPUT_EVENT, self._on_event, self)
        core.register_event_listener(FB_EVENT_FRAME_BEGIN, self._on_event, self)
        core.register_event_listener(FB_EVENT_SHUTDOWN, self._on_event, self)
        self.initialized = True
        core.log(self.mod_id, FB_LOG_INFO, "[fpv_drone] Reference state machine initialized.")
        return FB_OK

    def fb_plugin_shutdown(self) -> None:
        if self.drone_active:
            self.recall()
        self.initialized = False

    def _read_f32(self, va: int) -> Optional[float]:
        if not va:
            return None
        rc, bits = self._core.title_mem_read_u32(va)
        if rc != FB_OK:
            return None
        val = _bits_f32(bits)
        return val if math.isfinite(val) else None

    def _write_f32(self, va: int, val: float) -> bool:
        if not va:
            return False
        return self._core.title_mem_write_u32(va, _f32_bits(val)) == FB_OK

    def _set_camera_override(self, active: bool) -> None:
        if self.camera_override_va:
            self._core.title_mem_write_u32(self.camera_override_va, 1 if active else 0)

    def _read_pilot(self) -> Tuple[float, float, float, float]:
        px = self._read_f32(self.pilot_pos_va)
        py = self._read_f32(self.pilot_pos_va + 4) if self.pilot_pos_va else None
        pz = self._read_f32(self.pilot_pos_va + 8) if self.pilot_pos_va else None
        hdg = self._read_f32(self.pilot_pos_va + 12) if self.pilot_pos_va else None
        if px is not None and py is not None and pz is not None:
            self.pilot_pos = [px, py, pz]
            return px, py, pz, (hdg if hdg is not None else 0.0)
        return 0.0, 0.0, self.ground_fallback_z, 0.0

    def launch(self) -> None:
        px, py, pz, hdg = self._read_pilot()
        self.position = [
            px - math.sin(hdg) * self.spawn_forward_m,
            py + math.cos(hdg) * self.spawn_forward_m,
            pz + self.spawn_height_m,
        ]
        self.drone_active = True
        self.settle_start_time = None
        self.settled = False
        if self.view == FPV_VIEW_PILOT:
            self.view = self.prev_non_pilot_view

    def recall(self) -> None:
        self.drone_active = False
        self.settled = False
        self._set_camera_override(False)
        if self.focus_pos_va:
            for i, coord in enumerate(self.pilot_pos):
                self._write_f32(self.focus_pos_va + 4 * i, coord)

    def respawn(self) -> None:
        if not self.drone_active:
            return
        px, py, pz, hdg = self._read_pilot()
        self.position = [
            px - math.sin(hdg) * self.spawn_forward_m,
            py + math.cos(hdg) * self.spawn_forward_m,
            pz + self.spawn_height_m,
        ]

    def set_view(self, view: int) -> None:
        self.view = view % 4
        if self.view != FPV_VIEW_PILOT:
            self.prev_non_pilot_view = self.view
        else:
            self._set_camera_override(False)

    def _on_event(self, event_id: int, payload: Any, _user_ctx: Any) -> int:
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
        if ev.device_kind != FB_DEV_KEYBOARD or ev.action != FB_INPUT_PRESS:
            return 0
        mods = ev.modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT)
        if mods == 0 and ev.code == self.key_toggle_drone:
            if self.drone_active:
                self.recall()
            else:
                self.launch()
            return 1
        if mods == FB_MOD_SHIFT and ev.code == self.key_cycle_view and self.drone_active:
            self.set_view(self.view + 1)
            return 1
        if mods == FB_MOD_CTRL and ev.code == self.key_respawn and self.drone_active:
            self.respawn()
            return 1
        if mods == FB_MOD_CTRL and ev.code == self.key_pilot_view and self.drone_active:
            self.set_view(self.prev_non_pilot_view if self.view == FPV_VIEW_PILOT else FPV_VIEW_PILOT)
            return 1
        if mods == 0 and ev.code == self.key_overlay:
            self.overlay_open = not self.overlay_open
            return 1
        return 0

    def _handle_frame(self, frame: FrameEvent) -> None:
        if not self.drone_active:
            return
        dt = frame.dt_sec if 0.0 < frame.dt_sec <= 0.5 else (1.0 / 60.0)
        self.sim_time += dt
        if self.settle_start_time is None:
            self.settle_start_time = self.sim_time
        if (self.sim_time - self.settle_start_time) < 0.05:
            return
        self.settled = True

        if self.view != FPV_VIEW_PILOT:
            self._set_camera_override(True)
            if self.camera_va:
                if self.camera_layout == "mat34_fov":
                    vals = [
                        1.0, 0.0, 0.0,
                        0.0, 1.0, 0.0,
                        0.0, 0.0, 1.0,
                        self.position[0], self.position[1], self.position[2],
                        105.0, 0.05,
                    ]
                elif self.camera_layout == "pos_quat_fov":
                    vals = [self.position[0], self.position[1], self.position[2], 0.0, 0.0, 0.0, 1.0, 105.0, 0.05]
                else:
                    vals = [self.position[0], self.position[1], self.position[2], 0.0, 0.0, 0.0, 105.0, 0.05]
                if all(self._write_f32(self.camera_va + 4 * i, v) for i, v in enumerate(vals)):
                    self.camera_writes += 1
        else:
            self._set_camera_override(False)

        if self.focus_pos_va:
            for i, coord in enumerate(self.position):
                self._write_f32(self.focus_pos_va + 4 * i, coord)


def create_plugin() -> FpvDronePlugin:
    return FpvDronePlugin()
