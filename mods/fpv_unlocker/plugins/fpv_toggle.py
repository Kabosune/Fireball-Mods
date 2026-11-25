"""
fpv_toggle — Reference Fireball-Core Tier 2 Plugin State Machine for On-Foot FPV Toggle & Hold-Guard.

Mirrors the lifecycle and service calls defined in include/fireball_plugin.h:
  - fb_plugin_query()
  - fb_plugin_init(core_services)
  - fb_plugin_shutdown()

Behavior:
  1. Resolves `rage::camGameplayDirector::s_Instance` via `resolve_engine_symbol`.
  2. Listens for `FB_EVENT_INPUT_EVENT`: pressing hotkey (`VK_V` / 0x56 by default)
     flips `camGameplayDirector.view_mode` between `0 (ThirdPerson)` and `2 (FirstPerson)`
     via guarded `title_mem_read_u32` / `title_mem_write_u32`. Holding `Alt+V` peeks into
     FirstPerson and restores the prior view_mode & latch state on release.
  3. Listens for `FB_EVENT_FRAME_BEGIN`: optionally auto-latches FirstPerson on the first
     valid frame when `start_in_fpv == True`; while latched ON (with `hold_guard_enabled`)
     or peeking, re-asserts `2 (FirstPerson)` if the game resets `view_mode`.
  4. On `fb_plugin_shutdown()`, restores ThirdPerson when `restore_on_shutdown == True`
     and currently latched or peeking.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Dict, Optional

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
    FB_INPUT_RELEASE,
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


@dataclass
class PluginInfo:
    abi_version: int
    mod_id: str
    plugin_id: str
    version: str
    required_caps: int


class FpvTogglePlugin:
    def __init__(self) -> None:
        self._core: Any = None
        self.mod_id: str = "community.fpv_unlocker"
        self.plugin_id: str = "fpv_toggle"
        self.hotkey_vkey: int = 0x56  # 'V'
        self.third_person_mode: int = 0
        self.cinematic_mode: int = 1
        self.first_person_mode: int = 2
        self.director_symbol: str = "rage::camGameplayDirector::s_Instance"
        self.view_mode_field_offset: int = 0
        self.hold_guard_enabled: bool = True
        self.start_in_fpv: bool = False
        self.restore_on_shutdown: bool = True
        self.verbose_reassert_log: bool = False

        self.director_va: int = 0
        self.view_mode_va: int = 0
        self.fpv_latched: bool = False
        self.peek_active: bool = False
        self.peek_prev_latched: bool = False
        self.peek_prev_mode: int = 0
        self.startup_applied: bool = False
        self.last_read_mode: Optional[int] = None
        self.toggle_count: int = 0
        self.reassert_count: int = 0
        self.frames_seen: int = 0
        self.initialized: bool = False

    def fb_plugin_query(self) -> PluginInfo:
        return PluginInfo(
            abi_version=FB_PLUGIN_ABI_VERSION,
            mod_id=self.mod_id,
            plugin_id=self.plugin_id,
            version="0.2.0",
            required_caps=(
                FB_CAP_FRAME_LIFECYCLE
                | FB_CAP_INPUT_EVENTS
                | FB_CAP_RAGE_SYMBOL_REG
                | FB_CAP_TITLE_MEM_RW
            ),
        )

    def fb_plugin_init(self, core: Any) -> int:
        self._core = core
        cfg_raw = core.config_get_json(self.mod_id)
        if cfg_raw:
            cfg: Dict[str, Any] = json.loads(cfg_raw)
            self.hotkey_vkey = int(cfg.get("hotkey_vkey", self.hotkey_vkey))
            self.third_person_mode = int(cfg.get("third_person_mode", self.third_person_mode))
            self.cinematic_mode = int(cfg.get("cinematic_mode", self.cinematic_mode))
            self.first_person_mode = int(cfg.get("first_person_mode", self.first_person_mode))
            self.director_symbol = str(cfg.get("director_symbol", self.director_symbol))
            self.view_mode_field_offset = int(cfg.get("view_mode_field_offset", self.view_mode_field_offset))
            self.hold_guard_enabled = bool(cfg.get("hold_guard_enabled", self.hold_guard_enabled))
            self.start_in_fpv = bool(cfg.get("start_in_fpv", self.start_in_fpv))
            self.restore_on_shutdown = bool(cfg.get("restore_on_shutdown", self.restore_on_shutdown))
            self.verbose_reassert_log = bool(cfg.get("verbose_reassert_log", self.verbose_reassert_log))

        self.director_va = core.resolve_engine_symbol(self.director_symbol)
        if not self.director_va:
            core.log(
                self.mod_id,
                FB_LOG_WARN,
                f"[fpv_toggle] Symbol '{self.director_symbol}' unavailable; plugin standing by.",
            )
            return FB_ERR_NOT_FOUND

        self.view_mode_va = self.director_va + self.view_mode_field_offset
        core.register_event_listener(FB_EVENT_INPUT_EVENT, self._on_event, self)
        core.register_event_listener(FB_EVENT_FRAME_BEGIN, self._on_event, self)
        core.register_event_listener(FB_EVENT_SHUTDOWN, self._on_event, self)
        self.initialized = True
        core.log(
            self.mod_id,
            FB_LOG_INFO,
            f"[fpv_toggle] Initialized v0.2.0: symbol={self.director_symbol} @ 0x{self.director_va:016X}, "
            f"view_mode_va=0x{self.view_mode_va:016X}, hotkey=0x{self.hotkey_vkey:02X}, "
            f"hold_guard={self.hold_guard_enabled}, start_in_fpv={self.start_in_fpv}",
        )
        return FB_OK

    def fb_plugin_shutdown(self) -> None:
        if self.initialized and (self.fpv_latched or self.peek_active):
            if self.restore_on_shutdown and self._core is not None:
                restore_mode = (
                    self.third_person_mode
                    if (self.fpv_latched or self.peek_prev_latched)
                    else self.peek_prev_mode
                )
                self._core.title_mem_write_u32(self.view_mode_va, restore_mode)
            self.fpv_latched = False
            self.peek_active = False
        self.initialized = False

    def _on_event(self, event_id: int, payload: Any, _user_ctx: Any) -> int:
        if not self.initialized:
            return 0
        if event_id == FB_EVENT_INPUT_EVENT and isinstance(payload, InputEvent):
            return self._handle_input(payload)
        if event_id == FB_EVENT_FRAME_BEGIN and isinstance(payload, FrameEvent):
            self._handle_frame_begin(payload)
            return 0
        if event_id == FB_EVENT_SHUTDOWN:
            self.fb_plugin_shutdown()
            return 0
        return 0

    def peek(self, active: bool) -> None:
        if active:
            if self.peek_active:
                return
            rc, cur_mode = self._core.title_mem_read_u32(self.view_mode_va)
            if rc == FB_OK:
                self.last_read_mode = cur_mode
                self.peek_prev_mode = cur_mode
            else:
                self.peek_prev_mode = self.first_person_mode if self.fpv_latched else self.third_person_mode
            self.peek_prev_latched = self.fpv_latched
            if self._core.title_mem_write_u32(self.view_mode_va, self.first_person_mode) == FB_OK:
                self.peek_active = True
                self.last_read_mode = self.first_person_mode
        else:
            if not self.peek_active:
                return
            self.peek_active = False
            self.fpv_latched = self.peek_prev_latched
            restore_mode = self.first_person_mode if self.fpv_latched else self.peek_prev_mode
            if self._core.title_mem_write_u32(self.view_mode_va, restore_mode) == FB_OK:
                self.last_read_mode = restore_mode

    def _handle_input(self, ev: InputEvent) -> int:
        if ev.device_kind != FB_DEV_KEYBOARD or ev.code != self.hotkey_vkey:
            return 0
        mods = ev.modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT)
        if mods == FB_MOD_ALT and ev.action == FB_INPUT_PRESS:
            self.peek(True)
            return 1
        if self.peek_active and ev.action == FB_INPUT_RELEASE:
            self.peek(False)
            return 1
        if mods != 0 or ev.action != FB_INPUT_PRESS:
            return 0

        rc, cur_mode = self._core.title_mem_read_u32(self.view_mode_va)
        if rc != FB_OK:
            self._core.log(
                self.mod_id,
                FB_LOG_WARN,
                f"[fpv_toggle] Guarded read failed on 0x{self.view_mode_va:016X} (rc={rc})",
            )
            return 0

        next_latched = not self.fpv_latched
        target_mode = self.first_person_mode if next_latched else self.third_person_mode
        wrc = self._core.title_mem_write_u32(self.view_mode_va, target_mode)
        if wrc == FB_OK:
            self.peek_active = False
            self.fpv_latched = next_latched
            self.startup_applied = True
            self.last_read_mode = target_mode
            self.toggle_count += 1
            self._core.log(
                self.mod_id,
                FB_LOG_INFO,
                f"[fpv_toggle] Hotkey toggled view_mode: {cur_mode} -> {target_mode} (latched={self.fpv_latched})",
            )
            return 1
        return 0

    def _handle_frame_begin(self, frame: FrameEvent) -> None:
        self.frames_seen += 1
        rc, cur_mode = self._core.title_mem_read_u32(self.view_mode_va)
        if rc != FB_OK:
            return
        self.last_read_mode = cur_mode

        if not self.startup_applied:
            self.startup_applied = True
            if self.start_in_fpv and not self.fpv_latched and not self.peek_active:
                if self._core.title_mem_write_u32(self.view_mode_va, self.first_person_mode) == FB_OK:
                    self.fpv_latched = True
                    self.last_read_mode = self.first_person_mode
                return

        if (self.peek_active or (self.fpv_latched and self.hold_guard_enabled)) and cur_mode != self.first_person_mode:
            wrc = self._core.title_mem_write_u32(self.view_mode_va, self.first_person_mode)
            if wrc == FB_OK:
                self.reassert_count += 1
                self.last_read_mode = self.first_person_mode
                if self.verbose_reassert_log:
                    self._core.log(
                        self.mod_id,
                        FB_LOG_INFO,
                        f"[fpv_toggle] Frame #{frame.frame_index}: hold-guard intercepted context reset "
                        f"({cur_mode} -> re-asserted {self.first_person_mode})",
                    )


def create_plugin() -> FpvTogglePlugin:
    return FpvTogglePlugin()
