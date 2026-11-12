#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT="$ROOT/out/rpc-echo-host-test"
mkdir -p "$OUT"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
    -I"$ROOT/include" -I"$ROOT/zygisk/tests/plugin-stubs" \
    "$ROOT/examples/rpc_echo/plugin_entry.cpp" "$ROOT/examples/rpc_echo/test-host.cpp" \
    -o "$OUT/rpc_echo_test"
"$OUT/rpc_echo_test"
