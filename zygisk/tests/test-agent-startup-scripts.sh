#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-startup-test.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
LUA_OBJECT=${LUA_OBJECT:-"${TEST_DIR}/lua-lib.o"}
if [[ ! -f "${LUA_OBJECT}" ]]; then
    "${CC}" -std=c11 -O2 -DMAKE_LIB -DLUA_USE_LINUX -I"${PROJECT_DIR}/extern/lua" \
        -c "${PROJECT_DIR}/extern/lua/onelua.c" -o "${LUA_OBJECT}"
fi
"${CXX}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" -I"${PROJECT_DIR}/extern/lua" \
    "${ROOT_DIR}/src/agent_startup_scripts.cpp" \
    "${ROOT_DIR}/tests/agent_startup_scripts_test.cpp" "${LUA_OBJECT}" \
    -ldl -lm -o "${TEST_DIR}/agent_startup_scripts_test"
"${TEST_DIR}/agent_startup_scripts_test"
