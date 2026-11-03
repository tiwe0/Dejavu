#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CXX=${CXX:-c++}
OUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-lsplant-protection.XXXXXX")
trap 'rm -rf "${OUT_DIR}"' EXIT
FLAGS=(-fno-omit-frame-pointer)
if [[ -n "${LSPLANT_TEST_SANITIZERS:-}" ]]; then
    FLAGS+=(-fsanitize="${LSPLANT_TEST_SANITIZERS}")
fi
"${CXX}" -std=c++20 -O0 -g -Wall -Wextra -Werror -pthread "${FLAGS[@]}" \
    -I"${ROOT_DIR}/tests/lsplant-stubs" -I"${ROOT_DIR}/tests/native-stubs" \
    -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/src" \
    "${ROOT_DIR}/tests/lsplant_protection_test.cpp" -o "${OUT_DIR}/lsplant_protection_test"
"${OUT_DIR}/lsplant_protection_test"
