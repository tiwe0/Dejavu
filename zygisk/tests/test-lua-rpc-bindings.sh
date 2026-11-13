#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-lua-rpc.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
if [[ -n "${DEJAVU_HOST_OBJ_DIR:-}" ]]; then
  OBJ_DIR=$DEJAVU_HOST_OBJ_DIR
else
  if ! OUT_DIR="$WORK/core" "$ROOT/scripts/smoke-host.sh" >"$WORK/core-build.log" 2>&1; then
    cat "$WORK/core-build.log" >&2
    exit 1
  fi
  OBJ_DIR="$WORK/core/obj"
fi
FLAGS=(-O1 -g -fno-omit-frame-pointer)
if [[ -n "${LUA_RPC_SANITIZERS:-}" ]]; then FLAGS+=(-fsanitize="$LUA_RPC_SANITIZERS"); fi
if [[ ! -f "$OBJ_DIR/dejavu_lua.o" ]]; then
  echo 'Core object directory is missing dejavu_lua.o.' >&2
  exit 1
fi
"${CC:-cc}" -std=c11 "${FLAGS[@]}" -I"$ROOT/router/third_party/cJSON" -c \
  "$ROOT/router/third_party/cJSON/cJSON.c" -o "$WORK/json.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread "${FLAGS[@]}" \
  -I"$ROOT/zygisk/tests/plugin-stubs" -I"$ROOT/include" -I"$ROOT/zygisk/include" \
  -I"$ROOT/router/third_party/cJSON" "$ROOT/zygisk/src/rpc_service.cpp" \
  "$ROOT/zygisk/src/lua_rpc_bindings.cpp" "$ROOT/zygisk/tests/lua_rpc_bindings_test.cpp" \
  "$WORK/json.o" "$OBJ_DIR/dejavu.o" "$OBJ_DIR/dejavu_engine.o" "$OBJ_DIR/dejavu_lua.o" \
  "$OBJ_DIR/dejavu_lua_json.o" "$OBJ_DIR/lua.o" "$OBJ_DIR/tinycc.o" -ldl -lm -o "$WORK/lua_rpc_bindings_test"
"$WORK/lua_rpc_bindings_test" "$ROOT/zygisk/config/rpc.lua"
