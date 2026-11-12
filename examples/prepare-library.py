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


def prepare():
    packages = []
    names = ["hello", "capture", "algorithm", "unpack", "rpc_echo"]
    for name in names:
        source = ROOT / "examples" / name
        destination = OUTPUT / name
        manifest = json.loads((source / "manifest.json").read_text())
        # Validate every declared resource before publishing the manifest.
        resources = [(path, destination / path if manifest["kind"] == "native" and
                      path == manifest["entry"] else source / path) for path in manifest["files"]]
        for _, path in resources:
            if not path.is_file():
                raise SystemExit(f"missing plugin resource: {path}; build the plugin first")
        for relative, path in resources:
            copy_resource(path, destination / relative)
        copy_resource(source / "manifest.json", destination / "manifest.json")
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
