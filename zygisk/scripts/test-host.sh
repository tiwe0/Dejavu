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

"${CXX}" -std=c++17 -Wall -Wextra -Werror -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/src/control_channel.cpp" \
    "${ROOT_DIR}/tests/control_relay_test.cpp" -o "${OUT_DIR}/control_relay_test"
"${OUT_DIR}/control_relay_test"

"${CXX}" -std=c++17 -Wall -Wextra -Werror -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/src/mapping_protection.cpp" \
    "${ROOT_DIR}/tests/mapping_protection_test.cpp" -o "${OUT_DIR}/mapping_protection_test"
"${OUT_DIR}/mapping_protection_test"
"${CXX}" -std=c++17 -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/tests/plugin-stubs" -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src" \
    "${ROOT_DIR}/src/mapping_protection.cpp" \
    "${ROOT_DIR}/tests/root_helper_client_test.cpp" -o "${OUT_DIR}/root_helper_client_test"
"${OUT_DIR}/root_helper_client_test"
"${ROOT_DIR}/tests/test-native-hook-manager.sh"
bash "${ROOT_DIR}/tests/test-lsplant-protection.sh"
if [[ -f "${ROOT_DIR}/third_party/Dobby/.dejavu-native-patch" ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "${ROOT_DIR}/tests/dobby_safety_test.py"
else
    echo "SKIP: Dobby safety tests require scripts/fetch-third-party.sh"
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    PLUGIN_TEST_LINK_FLAGS=(-Wl,-dead_strip)
else
    PLUGIN_TEST_LINK_FLAGS=(-Wl,--gc-sections -ldl)
fi
"${CXX}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -ffunction-sections -fdata-sections -fvisibility=hidden \
    -I"${ROOT_DIR}/tests/plugin-stubs" -I"${ROOT_DIR}/tests/native-stubs" \
    -I"${ROOT_DIR}/include" -I"${PROJECT_DIR}/include" -I"${ROOT_DIR}/src" \
    "${ROOT_DIR}/tests/plugin_hook_lifecycle_test.cpp" \
    "${PLUGIN_TEST_LINK_FLAGS[@]}" -o "${OUT_DIR}/plugin_hook_lifecycle_test"
"${OUT_DIR}/plugin_hook_lifecycle_test"

"${CC}" -std=c11 -O2 \
    -I"${ROOT_DIR}/third_party/kcp" \
    -c "${ROOT_DIR}/third_party/kcp/ikcp.c" -o "${OUT_DIR}/ikcp.o"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
    -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/third_party/kcp" \
    "${ROOT_DIR}/src/stream_service.cpp" \
    "${ROOT_DIR}/tests/stream_service_test.cpp" \
    "${OUT_DIR}/ikcp.o" -o "${OUT_DIR}/stream_service_test"
"${OUT_DIR}/stream_service_test"
bash "${ROOT_DIR}/tests/test-hook-events.sh"
bash "${ROOT_DIR}/tests/test-lua-stream-waits.sh"

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

"${ROOT_DIR}/tests/module_installer_test.sh"

PYTHONPYCACHEPREFIX="${OUT_DIR}/python" \
    python3 -m py_compile "${ROOT_DIR}/scripts/dejavuctl"
PYTHONPYCACHEPREFIX="${OUT_DIR}/python" \
    python3 -m unittest "${ROOT_DIR}/tests/dejavuctl_test.py"
PYTHONPYCACHEPREFIX="${OUT_DIR}/python" \
    python3 "${ROOT_DIR}/tests/deploy_agent_test.py"
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
