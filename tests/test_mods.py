"""Offline Python unit tests for Fireball-Mods (manifests, staging helper, and reference plugins)."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import tomllib
import unittest
from typing import Any, Callable, Dict, List, Optional, Tuple


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS_DIR = os.path.join(REPO_ROOT, "scripts")
MODS_DIR = os.path.join(REPO_ROOT, "mods")
CORE_DIR = os.environ.get("FIREBALL_CORE_DIR", r"C:\dev\fireball-core")

if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)
if os.path.isdir(CORE_DIR) and CORE_DIR not in sys.path:
    sys.path.insert(0, CORE_DIR)

import stage_mods  # noqa: E402


def _load_module_from_path(name: str, path: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load module {name} from {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


class _MiniCore:
    """Lightweight offline mock of `fb_core_services_v1` for reference Python plugins."""

    def __init__(self, symbols: Dict[str, int], config_by_mod: Optional[Dict[str, Dict[str, Any]]] = None) -> None:
        self.symbols = dict(symbols)
        self.configs = config_by_mod or {}
        self.mem: Dict[int, int] = {}
        self.listeners: Dict[int, List[Tuple[Callable[..., int], Any]]] = {}
        self.logs: List[Tuple[str, int, str]] = []

    def log(self, mod_id: str, level: int, msg: str) -> None:
        self.logs.append((mod_id, level, msg))

    def config_get_json(self, mod_id: str) -> str:
        cfg = self.configs.get(mod_id)
        return json.dumps(cfg) if cfg is not None else ""

    def resolve_engine_symbol(self, sym: str) -> int:
        return self.symbols.get(sym, 0)

    def register_event_listener(self, event_id: int, fn: Callable[..., int], ctx: Any) -> int:
        self.listeners.setdefault(event_id, []).append((fn, ctx))
        return 0

    def title_mem_read_u32(self, va: int) -> Tuple[int, int]:
        if va % 4 != 0 or va not in self.mem:
            return (-5, 0)
        return (0, self.mem[va])

    def title_mem_write_u32(self, va: int, value: int) -> int:
        if va % 4 != 0 or va not in self.mem:
            return -5
        self.mem[va] = value & 0xFFFFFFFF
        return 0

    def emit(self, event_id: int, payload: Any) -> int:
        consumed = 0
        for fn, ctx in self.listeners.get(event_id, []):
            if fn(event_id, payload, ctx):
                consumed = 1
        return consumed


class TestFireballModsOffline(unittest.TestCase):
    def test_manifests_valid_and_unique(self) -> None:
        validated = stage_mods.validate_all_mods(MODS_DIR)
        ids = {m["mod_id"] for m in validated}
        self.assertEqual(
            ids,
            {
                "community.fpv_unlocker",
                "community.time_weather_control",
                "community.fpv_drone",
                "community.quake_movement",
            },
        )

        with open(os.path.join(MODS_DIR, "fpv_unlocker", "mod.toml"), "rb") as f:
            fpv_toml = tomllib.load(f)
        fpv_cfg = fpv_toml["plugins"][0]["config"]
        self.assertFalse(fpv_cfg["start_in_fpv"])
        self.assertTrue(fpv_cfg["restore_on_shutdown"])
        self.assertFalse(fpv_cfg["verbose_reassert_log"])
        self.assertTrue(fpv_cfg["hold_guard_enabled"])

        with open(os.path.join(MODS_DIR, "time_weather_control", "mod.toml"), "rb") as f:
            tw_toml = tomllib.load(f)
        tw_cfg = tw_toml["plugins"][0]["config"]
        self.assertFalse(tw_cfg["ui_enabled"])
        self.assertEqual(tw_cfg["weather_apply_mode"], "instant")

        with open(os.path.join(MODS_DIR, "fpv_drone", "mod.toml"), "rb") as f:
            drone_toml = tomllib.load(f)
        drone_cfg = drone_toml["plugins"][0]["config"]
        self.assertEqual(drone_cfg["camera_layout"], "mat34_fov")
        self.assertFalse(drone_cfg["osd_window_enabled"])
        self.assertFalse(drone_cfg["enable_lens_distortion"])
        self.assertEqual(drone_cfg["configurator_port"], 5761)

        with open(os.path.join(MODS_DIR, "quake_movement", "mod.toml"), "rb") as f:
            qm_toml = tomllib.load(f)
        qm_cfg = qm_toml["plugins"][0]["config"]
        self.assertFalse(qm_cfg["start_enabled"])
        self.assertEqual(qm_cfg["default_preset"], "quake_vq3")
        self.assertEqual(qm_cfg["player_vel_symbol"], "fb.player_vel")
        self.assertEqual(qm_cfg["player_flags_symbol"], "fb.player_flags")
        self.assertEqual(qm_cfg["camera_basis_symbol"], "fb.camera_basis")
        self.assertEqual(qm_cfg["key_toggle_movement"], 0x75)

    def test_stage_mods_cli_help_and_staging(self) -> None:
        proc = subprocess.run(
            [sys.executable, "-B", os.path.join(SCRIPTS_DIR, "stage_mods.py"), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Validate and stage Fireball-Mods", proc.stdout)

        with tempfile.TemporaryDirectory() as td:
            game_dir = os.path.join(td, "GTA6")
            with self.assertRaises(ValueError):
                stage_mods.check_out_not_in_game_dir(os.path.join(game_dir, "stage"), game_dir)

            build_dir = os.path.join(td, "build")
            out_dir = os.path.join(td, "stage")
            os.makedirs(build_dir, exist_ok=True)
            # Create dummy DLL placeholders in temp dir only
            for name in (
                "fpv_toggle.fbplugin.dll",
                "time_weather.fbplugin.dll",
                "fpv_drone.fbplugin.dll",
                "quake_movement.fbplugin.dll",
            ):
                with open(os.path.join(build_dir, name), "wb") as wf:
                    wf.write(b"MZ_TEST")

            rep = stage_mods.stage_mods(MODS_DIR, build_dir, out_dir, game_dir=game_dir)
            self.assertEqual(len(rep["mods"]), 4)
            for m in rep["mods"]:
                self.assertEqual(len(m["staged_dlls"]), 1)
                self.assertTrue(os.path.isfile(m["staged_dlls"][0]))
                self.assertTrue(os.path.isfile(os.path.join(m["staged_dir"], "mod.toml")))

    @unittest.skipUnless(os.path.isdir(os.path.join(CORE_DIR, "fireball")), "FIREBALL_CORE_DIR/fireball not available")
    def test_reference_fpv_plugin_state_machine(self) -> None:
        from fireball.abi import (
            FB_DEV_KEYBOARD,
            FB_EVENT_FRAME_BEGIN,
            FB_EVENT_INPUT_EVENT,
            FB_INPUT_PRESS,
            FB_INPUT_RELEASE,
            FB_OK,
            FrameEvent,
            InputEvent,
        )

        fpv_mod = _load_module_from_path(
            "fb_mods_fpv_toggle",
            os.path.join(MODS_DIR, "fpv_unlocker", "plugins", "fpv_toggle.py"),
        )
        plugin = fpv_mod.create_plugin()
        core = _MiniCore(
            {"rage::camGameplayDirector::s_Instance": 0x1000},
            {"community.fpv_unlocker": {"start_in_fpv": True, "restore_on_shutdown": True}},
        )
        core.mem[0x1000] = 0
        self.assertEqual(plugin.fb_plugin_init(core), FB_OK)

        # start_in_fpv latches on first valid frame
        core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=1, dt_sec=1 / 60))
        self.assertEqual(core.mem[0x1000], 2)
        self.assertTrue(plugin.fpv_latched)

        # Hold-guard re-asserts on context reset
        core.mem[0x1000] = 0
        core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=2, dt_sec=1 / 60))
        self.assertEqual(core.mem[0x1000], 2)
        self.assertEqual(plugin.reassert_count, 1)

        # Toggle OFF -> 0, game sets Cinematic (1), Alt+V peek -> 2 -> release restores 1
        self.assertEqual(
            core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_PRESS, 0)),
            1,
        )
        self.assertEqual(core.mem[0x1000], 0)
        core.mem[0x1000] = 1
        self.assertEqual(
            core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_PRESS, fpv_mod.FB_MOD_ALT)),
            1,
        )
        self.assertEqual(core.mem[0x1000], 2)
        self.assertEqual(
            core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_RELEASE, fpv_mod.FB_MOD_ALT)),
            1,
        )
        self.assertEqual(core.mem[0x1000], 1)

        # Latch ON then shutdown restores ThirdPerson (0)
        core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_PRESS, 0))
        self.assertEqual(core.mem[0x1000], 2)
        plugin.fb_plugin_shutdown()
        self.assertEqual(core.mem[0x1000], 0)

    @unittest.skipUnless(os.path.isdir(os.path.join(CORE_DIR, "fireball")), "FIREBALL_CORE_DIR/fireball not available")
    def test_reference_time_weather_plugin_state_machine(self) -> None:
        from fireball.abi import (
            FB_DEV_KEYBOARD,
            FB_EVENT_FRAME_BEGIN,
            FB_EVENT_INPUT_EVENT,
            FB_INPUT_PRESS,
            FB_OK,
            FrameEvent,
            InputEvent,
        )

        tw_mod = _load_module_from_path(
            "fb_mods_time_weather",
            os.path.join(MODS_DIR, "time_weather_control", "plugins", "time_weather.py"),
        )
        plugin = tw_mod.create_plugin()
        with tempfile.TemporaryDirectory() as td:
            wlist = os.path.join(td, "weathers.txt")
            with open(wlist, "w", encoding="ascii") as wf:
                wf.write("0 0xF5A87B65 0x1 CLEAR\n1 0x614A1F91 0x2 SUNNY\n2 0x5974E8E5 0x3 STORM\n")
            core = _MiniCore(
                {"fb.clock": 0x2000, "fb.weather": 0x3000},
                {
                    "community.time_weather_control": {
                        "clock_layout": "h,m,s",
                        "weather_kind": "hash",
                        "weather_list_path": wlist,
                        "weather_apply_mode": "blend",
                        "weather_blend_ms": 100.0,
                    }
                },
            )
            core.mem[0x2000] = 10
            core.mem[0x2004] = 0
            core.mem[0x2008] = 0
            core.mem[0x3000] = 0xF5A87B65
            core.mem[0x3004] = 0xF5A87B65
            core.mem[0x3008] = 0
            self.assertEqual(plugin.fb_plugin_init(core), FB_OK)
            self.assertFalse(plugin.cfg["ui_enabled"])

            core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=1, dt_sec=1 / 30))
            # ' (weather_next) blends because weather_apply_mode == "blend"
            self.assertEqual(core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0xDE, FB_INPUT_PRESS, 0)), 1)
            core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=2, dt_sec=0.05))
            self.assertTrue(plugin.blending)
            self.assertEqual(core.mem[0x3000], 0xF5A87B65)
            self.assertEqual(core.mem[0x3004], 0x614A1F91)
            core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=3, dt_sec=0.11))
            self.assertFalse(plugin.blending)
            self.assertEqual(core.mem[0x3000], 0x614A1F91)
            self.assertTrue(plugin.weather_locked)

            # Ctrl+\ toggles weather lock OFF
            self.assertEqual(
                core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0xDC, FB_INPUT_PRESS, tw_mod.FB_MOD_CTRL)),
                1,
            )
            core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=4, dt_sec=1 / 30))
            self.assertFalse(plugin.weather_locked)
            plugin.fb_plugin_shutdown()

    @unittest.skipUnless(os.path.isdir(os.path.join(CORE_DIR, "fireball")), "FIREBALL_CORE_DIR/fireball not available")
    def test_reference_fpv_drone_plugin_state_machine(self) -> None:
        import struct
        from fireball.abi import (
            FB_DEV_KEYBOARD,
            FB_EVENT_FRAME_BEGIN,
            FB_EVENT_INPUT_EVENT,
            FB_INPUT_PRESS,
            FB_OK,
            FrameEvent,
            InputEvent,
        )

        drone_mod = _load_module_from_path(
            "fb_mods_fpv_drone",
            os.path.join(MODS_DIR, "fpv_drone", "plugins", "fpv_drone.py"),
        )
        plugin = drone_mod.create_plugin()
        core = _MiniCore(
            {
                "fb.drone_camera": 0x4000,
                "fb.drone_camera_override": 0x4100,
                "fb.pilot_pos": 0x4200,
                "fb.streaming_focus": 0x4300,
            },
            {"community.fpv_drone": {"camera_layout": "mat34_fov"}},
        )
        for off in range(0x4000, 0x4040, 4):
            core.mem[off] = 0
        core.mem[0x4100] = 0
        for i, val in enumerate((10.0, 20.0, 2.0, 0.0)):
            core.mem[0x4200 + 4 * i] = struct.unpack("<I", struct.pack("<f", val))[0]
        for off in range(0x4300, 0x4320, 4):
            core.mem[off] = 0

        self.assertEqual(plugin.fb_plugin_init(core), FB_OK)
        # F7 launches drone
        self.assertEqual(core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x76, FB_INPUT_PRESS, 0)), 1)
        self.assertTrue(plugin.drone_active)
        # 50 ms settle guard blocks first frames
        core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=1, dt_sec=0.02))
        self.assertEqual(core.mem[0x4100], 0)
        self.assertEqual(plugin.camera_writes, 0)
        # After >= 50 ms, camera override + mat34_fov + focus written
        core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=2, dt_sec=0.06))
        self.assertEqual(core.mem[0x4100], 1)
        self.assertEqual(plugin.camera_writes, 1)
        # Ctrl+F switches to Pilot view and clears camera override
        self.assertEqual(
            core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x46, FB_INPUT_PRESS, drone_mod.FB_MOD_CTRL)),
            1,
        )
        self.assertEqual(plugin.view, drone_mod.FPV_VIEW_PILOT)
        self.assertEqual(core.mem[0x4100], 0)
        # F7 recalls drone
        self.assertEqual(core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x76, FB_INPUT_PRESS, 0)), 1)
        self.assertFalse(plugin.drone_active)
        plugin.fb_plugin_shutdown()

    @unittest.skipUnless(os.path.isdir(os.path.join(CORE_DIR, "fireball")), "FIREBALL_CORE_DIR/fireball not available")
    def test_reference_quake_movement_plugin_state_machine(self) -> None:
        import struct
        from fireball.abi import (
            FB_DEV_KEYBOARD,
            FB_EVENT_FRAME_BEGIN,
            FB_EVENT_INPUT_EVENT,
            FB_INPUT_PRESS,
            FB_OK,
            FrameEvent,
            InputEvent,
        )

        qm_mod = _load_module_from_path(
            "fb_mods_quake_movement",
            os.path.join(MODS_DIR, "quake_movement", "plugins", "quake_movement.py"),
        )
        plugin = qm_mod.create_plugin()
        core = _MiniCore(
            {
                "fb.player_pos": 0x5000,
                "fb.player_vel": 0x5100,
                "fb.player_flags": 0x5200,
                "fb.camera_basis": 0x5300,
            },
            {"community.quake_movement": {"default_preset": "quake_vq3", "camera_layout": "yaw_f32"}},
        )
        for off in (0x5000, 0x5004, 0x5008, 0x5100, 0x5104, 0x5108, 0x5300):
            core.mem[off] = 0
        core.mem[0x5200] = qm_mod.FLAG_ON_GROUND

        self.assertEqual(plugin.fb_plugin_init(core), FB_OK)
        # F6 enables movement
        self.assertEqual(core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x75, FB_INPUT_PRESS, 0)), 1)
        self.assertTrue(plugin.enabled)
        # Hold W -> accelerates along +Y
        core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x57, FB_INPUT_PRESS, 0))
        core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=1, dt_sec=1 / 60))
        vy = struct.unpack("<f", struct.pack("<I", core.mem[0x5104]))[0]
        self.assertGreater(vy, 1.0)
        self.assertEqual(plugin.vel_writes, 1)

        # Ctrl + P cycles presets VQ3 -> CPMA -> Half-Life -> Apex -> Deadlock
        for expected in ("quake_cpma", "half_life", "apex_legends", "deadlock"):
            self.assertEqual(
                core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x50, FB_INPUT_PRESS, qm_mod.FB_MOD_CTRL)),
                1,
            )
            self.assertEqual(plugin.preset, expected)

        # Shift + Q triggers directional dash in Deadlock
        self.assertEqual(
            core.emit(FB_EVENT_INPUT_EVENT, InputEvent(FB_DEV_KEYBOARD, 0x51, FB_INPUT_PRESS, qm_mod.FB_MOD_SHIFT)),
            1,
        )
        core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=2, dt_sec=1 / 60))
        vy_dash = struct.unpack("<f", struct.pack("<I", core.mem[0x5104]))[0]
        self.assertGreater(vy_dash, 12.0)

        # Dead/in-vehicle guard blocks writes
        prev_writes = plugin.vel_writes
        core.mem[0x5200] = qm_mod.FLAG_ON_GROUND | qm_mod.FLAG_IN_VEHICLE
        core.emit(FB_EVENT_FRAME_BEGIN, FrameEvent(frame_index=3, dt_sec=1 / 60))
        self.assertEqual(plugin.vel_writes, prev_writes)
        self.assertEqual(plugin.guard_skips, 1)
        plugin.fb_plugin_shutdown()


if __name__ == "__main__":
    unittest.main()
