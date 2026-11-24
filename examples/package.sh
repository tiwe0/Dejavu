#!/usr/bin/env bash
# Package the example plugins as a separate release asset and per-plugin Manager ZIPs.
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
VERSION=${PACKAGE_VERSION:-$(sed -n 's/^version=//p' "${ROOT_DIR}/zygisk/module/module.prop" | head -n 1)}
VERSION=${VERSION#v}
if [[ -z "${VERSION}" ]]; then
    echo "error: unable to determine module version" >&2
    exit 1
fi
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/plugins"}
ZIP_NAME=${ZIP_NAME:-"dejavu-plugins-v${VERSION}-arm64.zip"}
ZIP_PATH="${OUT_DIR}/${ZIP_NAME}"
SINGLE_ZIPS_DIR="${OUT_DIR}/zips"
mkdir -p "${OUT_DIR}" "${SINGLE_ZIPS_DIR}"
python3 "${ROOT_DIR}/examples/prepare-library.py"
rm -f "${ZIP_PATH}"

python3 - "${ROOT_DIR}" "${ZIP_PATH}" "${SINGLE_ZIPS_DIR}" "${VERSION}" <<'PY'
import json
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
archive = Path(sys.argv[2])
single_dir = Path(sys.argv[3])
version = sys.argv[4]
plugin_names = (
    "hello",
    "capture",
    "algorithm",
    "unpack",
    "rpc_echo",
    "rpc_lua_echo",
    "hellojit",
    "hellofsjit",
    "hellohookjit",
)

# 1. Build individual Manager-importable ZIPs (exactly one manifest.json + declared files).
single_zips = []
for name in plugin_names:
    out_dir = root / "out" / "plugins" / name
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    single_zip = single_dir / f"{name}.zip"
    if single_zip.exists():
        single_zip.unlink()
    with zipfile.ZipFile(single_zip, "w", zipfile.ZIP_DEFLATED) as pkg:
        pkg.write(out_dir / "manifest.json", f"{name}/manifest.json")
        for relative in manifest["files"]:
            pkg.write(out_dir / relative, f"{name}/{relative}")
    single_zips.append((name, single_zip))

# 2. Build the combined release bundle containing folders, router templates, and ready-to-import ZIPs.
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
    readme = root / "examples" / "README.md"
    if readme.is_file():
        bundle.write(readme, "README.md")

    for name in plugin_names:
        out_dir = root / "out" / "plugins" / name
        manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        bundle.write(out_dir / "manifest.json", f"{name}/manifest.json")
        for relative in manifest["files"]:
            bundle.write(out_dir / relative, f"{name}/{relative}")
        router_example = out_dir / "router.example.json"
        if router_example.is_file():
            bundle.write(router_example, f"{name}/router.example.json")

    for name, single_zip in single_zips:
        bundle.write(single_zip, f"zips/{name}.zip")

    unpack_cli = root / "examples" / "unpack" / "cli.py"
    if unpack_cli.is_file():
        bundle.write(unpack_cli, "unpack/cli.py")

    manifest_txt = (
        "Dejavu example plugins\n"
        f"module version: {version}\n"
        "layout: <plugin>/manifest.json plus declared resources and optional router.example.json;\n"
        "        zips/<plugin>.zip ready for Manager Import ZIP; unpack/cli.py\n"
    )
    bundle.writestr("MANIFEST.txt", manifest_txt)

print(archive)
PY
echo "OK: ${ZIP_PATH}"
