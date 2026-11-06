#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CXX=${CXX:-c++}
OUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-lua-stream-waits.XXXXXX")
trap 'rm -rf "${OUT_DIR}"' EXIT
FLAGS=(-fno-omit-frame-pointer)
if [[ -n "${LUA_WAIT_TEST_SANITIZERS:-}" ]]; then
    FLAGS+=(-fsanitize="${LUA_WAIT_TEST_SANITIZERS}")
fi
python3 - "${ROOT_DIR}/src/hook_control.cpp" "${OUT_DIR}/lua_stream_callbacks.inc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
functions = []
for name in ("lua_stream_accept", "lua_stream_read"):
    start = source.index("int HookManager::Impl::" + name + "(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    functions.append(source[start:end])
Path(sys.argv[2]).write_text("\n\n".join(functions) + "\n")
PY
"${CXX}" -std=c++17 -O1 -g -Wall -Wextra -Werror "${FLAGS[@]}" \
    -I"${ROOT_DIR}/include" "${ROOT_DIR}/tests/lua_stream_wait_test.cpp" \
    -o "${OUT_DIR}/lua_stream_wait_test"
"${OUT_DIR}/lua_stream_wait_test"
"${CXX}" -std=c++17 -O1 -g -Wall -Wextra -Werror "${FLAGS[@]}" \
    -I"${OUT_DIR}" -I"${ROOT_DIR}/include" "${ROOT_DIR}/tests/lua_stream_callbacks_test.cpp" \
    -o "${OUT_DIR}/lua_stream_callbacks_test"
"${OUT_DIR}/lua_stream_callbacks_test"
