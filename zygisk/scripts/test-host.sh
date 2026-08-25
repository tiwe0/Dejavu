#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/host-tests"}

mkdir -p "${OUT_DIR}"
"${CXX}" \
    -std=c++17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/src/target_config.cpp" \
    "${ROOT_DIR}/tests/target_config_test.cpp" \
    -o "${OUT_DIR}/target_config_test"

"${OUT_DIR}/target_config_test"

"${CXX}" \
    -std=c++17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/src/aarch64_adrp_add.cpp" \
    "${ROOT_DIR}/tests/aarch64_adrp_add_test.cpp" \
    -o "${OUT_DIR}/aarch64_adrp_add_test"

"${OUT_DIR}/aarch64_adrp_add_test"

"${CXX}" \
    -std=c++17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/src/control_channel.cpp" \
    "${ROOT_DIR}/tests/control_channel_test.cpp" \
    -o "${OUT_DIR}/control_channel_test"

"${OUT_DIR}/control_channel_test"

"${CXX}" \
    -std=c++17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/src/hook_signature.cpp" \
    "${ROOT_DIR}/tests/hook_signature_test.cpp" \
    -o "${OUT_DIR}/hook_signature_test"

"${OUT_DIR}/hook_signature_test"

"${CC}" \
    -std=c17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    -fsyntax-only \
    "${ROOT_DIR}/tests/hook_api_compile_test.c"

PYTHONPYCACHEPREFIX="${OUT_DIR}/python" \
    python3 -m py_compile "${ROOT_DIR}/scripts/dejavuctl"
"${ROOT_DIR}/scripts/dejavuctl" --help >/dev/null
echo "OK: dejavuctl syntax and argument parser"
