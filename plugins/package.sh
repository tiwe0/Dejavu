#!/usr/bin/env bash
# Package the example plugins as a separate release asset.
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
mkdir -p "${OUT_DIR}"
rm -f "${ZIP_PATH}"

python3 - "${ROOT_DIR}" "${ZIP_PATH}" "${VERSION}" <<'PY'
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
archive = Path(sys.argv[2])
version = sys.argv[3]

with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
    readme = root / "plugins" / "README.md"
    if readme.is_file():
        bundle.write(readme, "README.md")

    for name in ("hello", "capture", "algorithm", "unpack"):
        plugin_dir = root / "plugins" / name
        out_dir = root / "out" / "plugins" / name
        libraries = sorted(out_dir.glob("*.so")) if out_dir.is_dir() else []
        if not libraries:
            raise SystemExit(f"missing built plugin: {out_dir}/*.so")
        for library in libraries:
            bundle.write(library, f"{name}/{library.name}")
        skill = plugin_dir / "SKILL.md"
        if skill.is_file():
            bundle.write(skill, f"{name}/SKILL.md")

    jit_dir = root / "plugins" / "hello"
    for source in sorted(jit_dir.glob("*.c")):
        bundle.write(source, f"jit/{source.name}")
        sidecar = source.with_name(source.name + ".SKILL.md")
        if sidecar.is_file():
            bundle.write(sidecar, f"jit/{sidecar.name}")

    unpack_cli = root / "plugins" / "unpack" / "cli.py"
    if unpack_cli.is_file():
        bundle.write(unpack_cli, "unpack/cli.py")

    manifest = (
        "Dejavu example plugins\n"
        f"module version: {version}\n"
        "layout: <plugin>/<library>.so + SKILL.md; jit/*.c + sidecars; unpack/cli.py\n"
    )
    bundle.writestr("MANIFEST.txt", manifest)

print(archive)
PY
echo "OK: ${ZIP_PATH}"
