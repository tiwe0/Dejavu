#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
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

"${CXX}" \
    -std=c++17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/src/hook_compile_diagnostics.cpp" \
    "${ROOT_DIR}/tests/hook_compile_diagnostics_test.cpp" \
    -o "${OUT_DIR}/hook_compile_diagnostics_test"

"${OUT_DIR}/hook_compile_diagnostics_test"

"${CC}" \
    -std=c17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    -fsyntax-only \
    "${ROOT_DIR}/tests/hook_api_compile_test.c"

# The convenience macro header intentionally uses GNU C extensions supported by
# TinyCC (statement expressions and variadic comma elision), so compile it in
# GNU C mode and execute the focused runtime regression test.
"${CC}" \
    -std=gnu17 \
    -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/tests/hook_utils_test.c" \
    -o "${OUT_DIR}/hook_utils_test"

"${OUT_DIR}/hook_utils_test"

PYTHONPYCACHEPREFIX="${OUT_DIR}/python" \
    python3 -m py_compile "${ROOT_DIR}/scripts/dejavuctl"
PYTHONPYCACHEPREFIX="${OUT_DIR}/python" \
    python3 -m unittest "${ROOT_DIR}/tests/dejavuctl_test.py"
"${ROOT_DIR}/scripts/dejavuctl" --help >/dev/null
echo "OK: dejavuctl syntax and argument parser"

"${CC}" \
    -std=c11 \
    -O2 \
    -Wall -Wextra -Werror \
    -DMAKE_LIB \
    -DLUA_USE_LINUX \
    -I"${PROJECT_DIR}/extern/lua" \
    -c "${PROJECT_DIR}/extern/lua/onelua.c" \
    -o "${OUT_DIR}/lua-lib.o"

"${CC}" \
    -std=c11 \
    -O2 \
    -Wall -Wextra -Werror \
    -DLUA_USE_LINUX \
    -I"${PROJECT_DIR}/extern/lua" \
    -c "${PROJECT_DIR}/extern/lua/lua.c" \
    -o "${OUT_DIR}/lua-main.o"

"${CC}" \
    "${OUT_DIR}/lua-lib.o" \
    "${OUT_DIR}/lua-main.o" \
    -ldl -lm \
    -o "${OUT_DIR}/lua"

"${OUT_DIR}/lua" "${ROOT_DIR}/tests/hookx_test.lua" "${ROOT_DIR}"
"${OUT_DIR}/lua" "${ROOT_DIR}/tests/hookx_compile_test.lua" "${ROOT_DIR}"
"${OUT_DIR}/lua" "${ROOT_DIR}/tests/hookx_bundle_test.lua" "${ROOT_DIR}"
