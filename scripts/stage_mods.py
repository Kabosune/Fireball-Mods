#!/usr/bin/env python3
"""
stage_mods.py — Stdlib-only mod manifest validator and staging helper for Fireball-Mods.

Validates `mods/*/mod.toml`, stages built `*.fbplugin.dll` binaries alongside mod manifests
and patch files into an `--out` directory (default `D:\\GTA6-work\\fireball-mods-stage`),
and optionally invokes `${FIREBALL_CORE_DIR}/fireball_cli.py` to pack `.gta6mod` archives
or compile a `fireball_bundle.fbin` patch bundle.

Strictly read-only against `--game-dir` (refuses writing inside the game install directory).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tomllib
from typing import Any, Dict, List, Optional, Tuple


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MODS_DIR = os.path.join(REPO_ROOT, "mods")
DEFAULT_BUILD_DIR = os.path.join(REPO_ROOT, "build", "Release")
DEFAULT_CORE_DIR = os.environ.get("FIREBALL_CORE_DIR", r"C:\dev\fireball-core")
DEFAULT_GAME_DIR = os.environ.get("GTA6_DIR", r"D:\Games\GTA6")
DEFAULT_OUT_DIR = r"D:\GTA6-work\fireball-mods-stage"

REQUIRED_MOD_FIELDS = ("id", "name", "version", "target_game_version", "min_fireball_abi")


def check_out_not_in_game_dir(out_path: str, game_dir: str) -> None:
    """Refuse writing outputs inside the read-only game install directory."""
    abs_out = os.path.abspath(out_path)
    abs_game = os.path.abspath(game_dir)
    try:
        common = os.path.commonpath([abs_out, abs_game])
    except ValueError:
        return
    if os.path.normcase(common) == os.path.normcase(abs_game):
        raise ValueError(f"Refusing to write inside read-only game directory: {abs_out}")


def discover_mods(mods_dir: str, selected: Optional[List[str]] = None) -> List[str]:
    """Return sorted paths to mod directories containing `mod.toml`."""
    if not os.path.isdir(mods_dir):
        raise FileNotFoundError(f"Mods directory not found: {mods_dir}")
    selected_set = set(selected) if selected else None
    found: List[str] = []
    for entry in sorted(os.listdir(mods_dir)):
        full = os.path.join(mods_dir, entry)
        if not os.path.isdir(full):
            continue
        if not os.path.isfile(os.path.join(full, "mod.toml")):
            continue
        if selected_set is not None and entry not in selected_set:
            continue
        found.append(full)
    if selected_set is not None:
        found_names = {os.path.basename(p) for p in found}
        missing = sorted(selected_set - found_names)
        if missing:
            raise FileNotFoundError(f"Requested mod(s) not found in {mods_dir}: {', '.join(missing)}")
    return found


def validate_mod_dir(mod_dir: str) -> Dict[str, Any]:
    """Validate a single mod directory's `mod.toml`, patch files, and plugin entries."""
    manifest_path = os.path.join(mod_dir, "mod.toml")
    if not os.path.isfile(manifest_path):
        raise FileNotFoundError(f"Missing mod.toml in {mod_dir}")
    with open(manifest_path, "rb") as rf:
        data = tomllib.load(rf)

    mod_sec = data.get("mod")
    if not isinstance(mod_sec, dict):
        raise ValueError(f"{manifest_path}: missing [mod] table")
    for field in REQUIRED_MOD_FIELDS:
        if field not in mod_sec:
            raise ValueError(f"{manifest_path}: missing required field [mod].{field}")

    min_abi = int(mod_sec["min_fireball_abi"])
    if min_abi < 1:
        raise ValueError(f"{manifest_path}: [mod].min_fireball_abi must be >= 1 (got {min_abi})")

    patches_sec = data.get("patches", {})
    patch_files: List[str] = []
    for kind in ("xml", "kv", "pso"):
        for item in patches_sec.get(kind, []):
            rel = str(item.get("patch", "")).strip()
            if not rel:
                raise ValueError(f"{manifest_path}: [[patches.{kind}]] entry missing 'patch' path")
            full_patch = os.path.join(mod_dir, rel)
            if not os.path.isfile(full_patch):
                raise FileNotFoundError(f"{manifest_path}: missing patch file '{rel}'")
            with open(full_patch, "rb") as pf:
                tomllib.load(pf)
            patch_files.append(rel)

    plugins: List[Dict[str, Any]] = list(data.get("plugins", []))
    for plg in plugins:
        entry = str(plg.get("entry", "")).strip()
        if entry:
            full_entry = os.path.join(mod_dir, entry)
            if not os.path.isfile(full_entry):
                raise FileNotFoundError(f"{manifest_path}: missing plugin entry '{entry}'")

    return {
        "dir_name": os.path.basename(mod_dir),
        "mod_dir": os.path.abspath(mod_dir),
        "mod_id": str(mod_sec["id"]),
        "name": str(mod_sec["name"]),
        "version": str(mod_sec["version"]),
        "target_game_version": str(mod_sec["target_game_version"]),
        "min_fireball_abi": min_abi,
        "patch_count": len(patch_files),
        "plugins": [
            {
                "id": str(p.get("id", "")),
                "entry": str(p.get("entry", "")),
                "native_path": str(p.get("native_path", "")),
            }
            for p in plugins
        ],
    }


def validate_all_mods(mods_dir: str, selected: Optional[List[str]] = None) -> List[Dict[str, Any]]:
    """Validate all selected mods and verify unique mod IDs."""
    mod_dirs = discover_mods(mods_dir, selected=selected)
    results: List[Dict[str, Any]] = []
    seen_ids: Dict[str, str] = {}
    for md in mod_dirs:
        info = validate_mod_dir(md)
        mid = info["mod_id"]
        if mid in seen_ids:
            raise ValueError(f"Duplicate [mod].id '{mid}' in {seen_ids[mid]} and {md}")
        seen_ids[mid] = md
        results.append(info)
    return results


def _copy_mod_tree(src_dir: str, dst_dir: str) -> int:
    """Copy manifest, patches, and reference plugins into staged mod directory (excluding C sources/caches)."""
    copied = 0
    os.makedirs(dst_dir, exist_ok=True)
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = [d for d in sorted(dirs) if d not in ("native", "__pycache__", "build", "bin")]
        rel_root = os.path.relpath(root, src_dir)
        for fn in sorted(files):
            if fn.endswith((".pyc", ".pyo")):
                continue
            src_f = os.path.join(root, fn)
            dst_f = os.path.join(dst_dir, fn) if rel_root == "." else os.path.join(dst_dir, rel_root, fn)
            os.makedirs(os.path.dirname(dst_f), exist_ok=True)
            shutil.copy2(src_f, dst_f)
            copied += 1
    return copied


def stage_mods(
    mods_dir: str,
    build_dir: str,
    out_dir: str,
    *,
    game_dir: str = DEFAULT_GAME_DIR,
    selected: Optional[List[str]] = None,
    allow_missing_dlls: bool = False,
) -> Dict[str, Any]:
    """Validate and stage mods + built `*.fbplugin.dll` binaries into `out_dir`."""
    check_out_not_in_game_dir(out_dir, game_dir)
    validated = validate_all_mods(mods_dir, selected=selected)
    os.makedirs(out_dir, exist_ok=True)

    staged: List[Dict[str, Any]] = []
    for info in validated:
        mod_out = os.path.join(out_dir, info["dir_name"])
        if os.path.isdir(mod_out):
            shutil.rmtree(mod_out)
        files_copied = _copy_mod_tree(info["mod_dir"], mod_out)

        staged_dlls: List[str] = []
        missing_dlls: List[str] = []
        for plg in info["plugins"]:
            native_rel = plg.get("native_path", "")
            if not native_rel:
                continue
            dll_name = os.path.basename(native_rel)
            src_dll = os.path.join(build_dir, dll_name)
            if os.path.isfile(src_dll):
                dst_dll = os.path.join(mod_out, native_rel.replace("/", os.sep))
                os.makedirs(os.path.dirname(dst_dll), exist_ok=True)
                shutil.copy2(src_dll, dst_dll)
                staged_dlls.append(os.path.abspath(dst_dll))
            else:
                missing_dlls.append(src_dll)

        if missing_dlls and not allow_missing_dlls:
            raise FileNotFoundError(
                f"Built plugin DLL(s) missing for '{info['mod_id']}': {', '.join(missing_dlls)}. "
                "Build with CMake first or pass --allow-missing-dlls."
            )

        staged.append(
            {
                "mod_id": info["mod_id"],
                "dir_name": info["dir_name"],
                "version": info["version"],
                "staged_dir": os.path.abspath(mod_out),
                "files_copied": files_copied,
                "staged_dlls": staged_dlls,
                "missing_dlls": missing_dlls,
            }
        )

    return {
        "out_dir": os.path.abspath(out_dir),
        "build_dir": os.path.abspath(build_dir),
        "mods": staged,
    }


def _invoke_fireball_cli(core_dir: str, cli_args: List[str]) -> Tuple[int, str, str]:
    cli_path = os.path.join(core_dir, "fireball_cli.py")
    if not os.path.isfile(cli_path):
        raise FileNotFoundError(f"fireball_cli.py not found under --core-dir: {cli_path}")
    proc = subprocess.run(
        [sys.executable, "-B", cli_path, *cli_args],
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout, proc.stderr


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Validate and stage Fireball-Mods manifests and built *.fbplugin.dll plugins."
    )
    p.add_argument("--mods-dir", default=DEFAULT_MODS_DIR, help=f"Mods source directory (default: {DEFAULT_MODS_DIR})")
    p.add_argument("--build-dir", default=DEFAULT_BUILD_DIR, help=f"CMake build output directory containing *.fbplugin.dll (default: {DEFAULT_BUILD_DIR})")
    p.add_argument("--core-dir", default=DEFAULT_CORE_DIR, help=f"Fireball-Core directory containing fireball_cli.py (default: {DEFAULT_CORE_DIR})")
    p.add_argument("--game-dir", default=DEFAULT_GAME_DIR, help=f"Read-only GTA VI directory (default: {DEFAULT_GAME_DIR})")
    p.add_argument("--out", default=DEFAULT_OUT_DIR, help=f"Staging output directory (default: {DEFAULT_OUT_DIR})")
    p.add_argument("--mod", action="append", default=None, help="Specific mod folder name under mods/ to process (repeatable; default: all)")
    p.add_argument("--validate-only", action="store_true", help="Validate mod manifests and exit without staging files")
    p.add_argument("--allow-missing-dlls", action="store_true", help="Stage manifests even if *.fbplugin.dll has not been built yet")
    p.add_argument("--pack", action="store_true", help="Also invoke fireball_cli.py pack to create .gta6mod packages in <out>/packages")
    p.add_argument("--build-bundle", action="store_true", help="Also invoke fireball_cli.py build-bundle to compile fireball_bundle.fbin in <out>")
    p.add_argument("--size-preserve", action="store_true", help="Pass --size-preserve to fireball_cli.py build-bundle")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.validate_only:
        validated = validate_all_mods(args.mods_dir, selected=args.mod)
        print(json.dumps({"validated": validated}, indent=2))
        return 0

    report = stage_mods(
        args.mods_dir,
        args.build_dir,
        args.out,
        game_dir=args.game_dir,
        selected=args.mod,
        allow_missing_dlls=bool(args.allow_missing_dlls),
    )

    if args.pack:
        pkg_dir = os.path.join(args.out, "packages")
        os.makedirs(pkg_dir, exist_ok=True)
        packed: List[str] = []
        for m in report["mods"]:
            out_zip = os.path.join(pkg_dir, f"{m['dir_name']}.gta6mod")
            rc, out, err = _invoke_fireball_cli(
                args.core_dir,
                ["--game-dir", args.game_dir, "--out", pkg_dir, "pack", "--mod", m["staged_dir"], "--output", out_zip],
            )
            if rc != 0:
                print(err or out, file=sys.stderr)
                return rc
            packed.append(os.path.abspath(out_zip))
        report["packed"] = packed

    if args.build_bundle:
        cli_args = ["--game-dir", args.game_dir, "--out", args.out, "build-bundle"]
        for m in report["mods"]:
            cli_args.extend(["--mod", m["staged_dir"]])
        if args.size_preserve:
            cli_args.append("--size-preserve")
        rc, out, err = _invoke_fireball_cli(args.core_dir, cli_args)
        if rc != 0:
            print(err or out, file=sys.stderr)
            return rc
        report["bundle_output"] = os.path.join(os.path.abspath(args.out), "fireball_bundle.fbin")

    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
