#!/usr/bin/env python3
"""Assemble importable plugin folders, each with its own manifest.json."""

import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "out" / "plugins"


def copy_resource(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() != destination.resolve():
        shutil.copyfile(source, destination)


def validate_json_file(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise SystemExit(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise SystemExit(f"expected JSON object in {path}")
    return data


def prepare():
    packages = []
    names = ["hello", "capture", "algorithm", "unpack", "rpc_echo", "rpc_lua_echo"]
    for name in names:
        source = ROOT / "examples" / name
        destination = OUTPUT / name
        manifest = validate_json_file(source / "manifest.json")
        if manifest.get("schemaVersion") != 1:
            raise SystemExit(f"unsupported manifest schemaVersion in {source / 'manifest.json'}")
        # Validate every declared resource before publishing the manifest.
        resources = [(path, destination / path if manifest["kind"] == "native" and
                      path == manifest["entry"] else source / path) for path in manifest["files"]]
        for relative, path in resources:
            if not path.is_file():
                raise SystemExit(f"missing plugin resource: {path}; build the plugin first")
            if relative == manifest.get("ui") or relative == manifest.get("router"):
                validate_json_file(path)
        for relative, path in resources:
            copy_resource(path, destination / relative)
        copy_resource(source / "manifest.json", destination / "manifest.json")
        router_example = source / "router.example.json"
        if router_example.is_file():
            validate_json_file(router_example)
            copy_resource(router_example, destination / "router.example.json")
        packages.append(destination)

    for identifier, filename, version, description in (
        ("hellojit", "hello_jit.c", "1.1.0", "TinyCC JIT host ABI example."),
        ("hellofsjit", "hello_fs_jit.c", "1.0.0", "TinyCC JIT filesystem service example."),
        ("hellohookjit", "hello_hook_jit.c", "1.0.0", "TinyCC JIT tracked-hook lifecycle example."),
    ):
        destination = OUTPUT / identifier
        files = [filename, filename + ".SKILL.md"]
        for relative in files:
            copy_resource(ROOT / "examples" / "hello" / relative, destination / relative)
        manifest = dict(schemaVersion=1, id=identifier, name=identifier, version=version,
                        description=description, kind="jit", entry=filename, files=files)
        (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        packages.append(destination)
    return packages


if __name__ == "__main__":
    for folder in prepare():
        print(folder)
