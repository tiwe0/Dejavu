#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
CXX=${CXX:-c++}
SANITIZER_FLAGS=(-fno-omit-frame-pointer)
if [[ -n "${NATIVE_HOOK_TEST_SANITIZERS:-}" ]]; then
    SANITIZER_FLAGS+=(-fsanitize="${NATIVE_HOOK_TEST_SANITIZERS}")
fi
OUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-native-manager.XXXXXX")
trap 'rm -rf "${OUT_DIR}"' EXIT
case "$(uname -s)" in
    Darwin) SHARED_FLAGS=(-dynamiclib); LIBRARY="${OUT_DIR}/libcold.dylib" ;;
    *) SHARED_FLAGS=(-shared -fPIC); LIBRARY="${OUT_DIR}/libcold.so" ;;
esac
"${CXX}" -std=c++17 -O0 -Wall -Wextra -Werror "${SANITIZER_FLAGS[@]}" "${SHARED_FLAGS[@]}" \
    "${ROOT_DIR}/tests/native-stubs/cold_library.cpp" -o "${LIBRARY}"
"${CXX}" -std=c++17 -O0 -g -Wall -Wextra -Werror -pthread "${SANITIZER_FLAGS[@]}" \
    -I"${ROOT_DIR}/tests/native-stubs" -I"${ROOT_DIR}/include" \
    -I"${PROJECT_DIR}/include" -I"${ROOT_DIR}/src" \
    "${ROOT_DIR}/tests/native-stubs/manager_wrapper.cpp" \
    "${ROOT_DIR}/tests/native_hook_manager_test.cpp" -ldl -o "${OUT_DIR}/native_hook_manager_test"
python3 - "${OUT_DIR}/native_hook_manager_test" "${LIBRARY}" <<'PY'
import subprocess
import sys
subprocess.run(sys.argv[1:], check=True, timeout=20)
subprocess.run([*sys.argv[1:], "--address-pin"], check=True, timeout=20)
PY
