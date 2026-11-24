#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT
"${CXX:-c++}" -std=c++23 -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" -I"${ROOT_DIR}/zygisk/tests/plugin-stubs" -I"${ROOT_DIR}/examples/common" \
    "${ROOT_DIR}/examples/capture/plugin_entry.cpp" \
    "${ROOT_DIR}/examples/capture/test-host.cpp" \
    -o "${TMP_DIR}/capture_host_test"
"${TMP_DIR}/capture_host_test"
