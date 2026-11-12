#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-rpc-service.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
FLAGS=(-O1 -g -fno-omit-frame-pointer)
if [[ -n "${RPC_SERVICE_SANITIZERS:-}" ]]; then FLAGS+=(-fsanitize="$RPC_SERVICE_SANITIZERS"); fi
"${CC:-cc}" -std=c11 "${FLAGS[@]}" -I"$ROOT/router/third_party/cJSON" -c \
  "$ROOT/router/third_party/cJSON/cJSON.c" -o "$WORK/json.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread "${FLAGS[@]}" \
  -I"$ROOT/zygisk/tests/plugin-stubs" -I"$ROOT/include" -I"$ROOT/zygisk/include" \
  -I"$ROOT/router/third_party/cJSON" "$ROOT/zygisk/src/rpc_service.cpp" \
  "$ROOT/zygisk/tests/rpc_service_test.cpp" "$WORK/json.o" -o "$WORK/rpc_service_test"
"$WORK/rpc_service_test"
