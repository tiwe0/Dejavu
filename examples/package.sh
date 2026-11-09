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
python3 "${ROOT_DIR}/examples/prepare-library.py"
rm -f "${ZIP_PATH}"

python3 - "${ROOT_DIR}" "${ZIP_PATH}" "${VERSION}" <<'PY'
import json
import sys
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
archive = Path(sys.argv[2])
version = sys.argv[3]

with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
    readme = root / "examples" / "README.md"
    if readme.is_file():
        bundle.write(readme, "README.md")

    for name in ("hello", "capture", "algorithm", "unpack", "hellojit", "hellofsjit", "hellohookjit"):
        out_dir = root / "out" / "plugins" / name
        manifest = json.loads((out_dir / "manifest.json").read_text())
        bundle.write(out_dir / "manifest.json", f"{name}/manifest.json")
        for relative in manifest["files"]:
            bundle.write(out_dir / relative, f"{name}/{relative}")

    unpack_cli = root / "examples" / "unpack" / "cli.py"
    if unpack_cli.is_file():
        bundle.write(unpack_cli, "unpack/cli.py")

    manifest = (
        "Dejavu example plugins\n"
        f"module version: {version}\n"
        "layout: <plugin>/manifest.json plus declared resources; unpack/cli.py\n"
    )
    bundle.writestr("MANIFEST.txt", manifest)

print(archive)
PY
echo "OK: ${ZIP_PATH}"
