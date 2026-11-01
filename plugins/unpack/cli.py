#!/usr/bin/env python3
"""Host-side runner for the plugins/unpack use-case demo.

This is intentionally *not* part of the framework CLI: it wires the generic
`./dejavu` helpers to the Lua strategy in this directory. Use it to capture
DEX early (via the example's system property) and to drive active invocation.

Examples:
    python3 plugins/unpack/cli.py -p com.android.settings \\
        --out /data/user_de/0/com.android.settings/files/dejavu-unpack
    python3 plugins/unpack/cli.py -p com.android.settings --status
    python3 plugins/unpack/cli.py -p com.android.settings --flush
    python3 plugins/unpack/cli.py -p com.android.settings \\
        --out <device-dir> --active-invoke-manifest
"""

from __future__ import annotations

import argparse
import os
import re
import runpy
import shlex
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = Path(__file__).resolve().parent
PLUGIN_BUILD = EXAMPLE_DIR / "build.sh"
DEFAULT_PLUGIN = REPO_ROOT / "out" / "plugins" / "unpack" / "libdejavu_unpack.so"
MODULE_DIR = "/data/adb/modules/dejavu_zygisk"
MODULE_PLUGIN = MODULE_DIR + "/plugins/libdejavu_unpack.so"
examples = runpy.run_path(str(REPO_ROOT / "dejavu"))
fail = examples["fail"]
root_shell = examples["root_shell"]
exec_inline = examples["exec_inline"]
delegate_dejavuctl = examples["delegate_dejavuctl"]
lua_quote = examples["lua_quote"]
restart_target = examples["restart_target"]
CliError = examples["CliError"]


def example_script(config_lua: str) -> str:
    parts = []
    for name in ("dex.lua", "manifest.lua", "main.lua"):
        path = EXAMPLE_DIR / name
        if not path.is_file():
            fail(f"unpack example file not found: {path}")
        content = path.read_text(encoding="utf-8")
        content = re.sub(r"\nreturn [A-Za-z_][A-Za-z0-9_]*\s*$", "\n", content)
        parts.append(content)
    parts.append(f"\nreturn run_unpack_example({config_lua})\n")
    return "\n".join(parts)


def command(args: argparse.Namespace) -> int:
    process = args.process
    if args.active_invoke_manifest:
        if not args.out:
            fail("--active-invoke-manifest requires --out <device-dir>")
        filter_literal = lua_quote(args.class_filter) if args.class_filter else "nil"
        config = (
            "{dex_dir=" + lua_quote(args.out)
            + ", class_filter=" + filter_literal
            + ", delay_ms=" + str(args.delay_ms) + "}"
        )
        script = example_script(config)
        with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as handle:
            handle.write(script)
            path = Path(handle.name)
        try:
            return delegate_dejavuctl(args, ["-p", process, "--timeout", "180", str(path)])
        finally:
            path.unlink(missing_ok=True)
    if args.active_invoke_dex:
        filter_literal = lua_quote(args.class_filter) if args.class_filter else "nil"
        config = (
            "{dex_file=" + lua_quote(args.active_invoke_dex)
            + ", class_filter=" + filter_literal
            + ", delay_ms=" + str(args.delay_ms) + "}"
        )
        script = example_script(config)
        with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as handle:
            handle.write(script)
            path = Path(handle.name)
        try:
            return delegate_dejavuctl(args, ["-p", process, "--timeout", "180", str(path)])
        finally:
            path.unlink(missing_ok=True)
    if args.flush:
        return exec_inline(args, process, "return unpack.flush(120000)")
    if args.status:
        exec_inline(args, process, "return plugin.list()")
        return exec_inline(args, process, "return unpack.status()")
    if args.disable:
        code = (
            "local ok = pcall(unpack.disable); "
            "return tostring(ok) .. ' ' .. tostring(unpack.status())"
        )
        exec_inline(args, process, code)
        try:
            root_shell(args, "setprop debug.dejavu.unpack.dir ''")
            root_shell(args, "setprop debug.dejavu.plugins ''")
            root_shell(args, "setprop debug.dejavu.unpack.include_file_backed 0")
        except CliError:
            pass
        print("unpack example hooks disabled; system property cleared")
        return 0
    if not args.out:
        fail("unpack example requires --out <device-directory> (or use --status/--disable/--flush)")
    if not re.fullmatch(r"[A-Za-z0-9_./-]+", args.out):
        fail("--out may only contain letters, digits, '_', '.', '/' and '-'")
    plugin = Path(args.plugin).expanduser().resolve()
    if not plugin.is_file():
        print(f"building unpack example plugin: {plugin}")
        import subprocess as _subprocess
        _subprocess.run([str(PLUGIN_BUILD)], check=True)
    if not plugin.is_file():
        fail(f"plugin not found after build: {plugin}")
    import subprocess as _subprocess
    adb_binary = os.environ.get("ADB") or "adb"
    adb_prefix = [adb_binary]
    if os.environ.get("ADB_SERIAL"):
        adb_prefix.extend(["-s", os.environ["ADB_SERIAL"]])
    staged = "/data/local/tmp/dejavu_unpack_example.so"
    _subprocess.run([*adb_prefix, "push", str(plugin), staged], check=True)
    plugin_destination = str(Path(args.out).parent / "libdejavu_unpack.so")
    root_shell(
        args,
        f"cp {staged} {shlex.quote(plugin_destination)} && "
        f"chmod 755 {shlex.quote(plugin_destination)} && "
        f"(chcon u:object_r:apk_data_file:s0 {shlex.quote(plugin_destination)} || true)",
    )
    # Default path: load the plugin on demand in the running process, after
    # startup.  This avoids installing ART hooks during early app init (which
    # can ANR); the debug.dejavu.plugins property remains available for the
    # explicit early-autoload workflow.
    root_shell(args, "setprop debug.dejavu.plugins ''")
    root_shell(args, "setprop debug.dejavu.unpack.dir ''")
    include_literal = "true" if args.include_file_backed else "false"
    if not args.no_restart:
        print(f"enabling unpack example for {process} -> {args.out}")
        restart_target(args, process, None)
        time.sleep(3)
    load_code = (
        f"local loaded = plugin.load({lua_quote(plugin_destination)}); "
        f"unpack.include_file_backed({include_literal}); "
        f"return loaded .. ' | ' .. unpack.enable({lua_quote(args.out)})"
    )
    result = exec_inline(args, process, load_code)
    if result != 0:
        time.sleep(2)
        result = exec_inline(args, process, load_code)
    if result == 0:
        print(f"dump directory: {args.out}")
        print("pull results after the target loads its dex files, e.g.:")
        print(f"  adb shell su -c 'cp -r {args.out} /sdcard/Download/'")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--process", default="bin.mt.plus")
    parser.add_argument("--out", help="device-writable output directory (persisted via property)")
    parser.add_argument("--status", action="store_true", help="show current status")
    parser.add_argument("--flush", action="store_true", help="wait for deferred dumps to be written")
    parser.add_argument("--disable", action="store_true", help="disable hooks and clear properties")
    parser.add_argument("--active-invoke-manifest", action="store_true",
                        help="invoke classes in the Manifest main-activity namespace")
    parser.add_argument("--active-invoke-dex", help="invoke classes from a device dex file")
    parser.add_argument("--class-filter", help="only invoke classes whose name contains this text")
    parser.add_argument("--delay-ms", type=int, default=40, help="pacing delay between classes")
    parser.add_argument("--include-file-backed", action="store_true",
                        help="also capture file-backed app/system dex")
    parser.add_argument("--no-restart", action="store_true",
                        help="enable hooks in the running process instead of restarting it")
    parser.add_argument("--plugin", default=str(DEFAULT_PLUGIN),
                        help="path to libdejavu_unpack.so (built if missing)")
    args = parser.parse_args(argv)
    # Namespace fields consumed by the generic `./dejavu` helpers.
    args.adb = None
    args.serial = None
    return command(args)


if __name__ == "__main__":
    raise SystemExit(main())
