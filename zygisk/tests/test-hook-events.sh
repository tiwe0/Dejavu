#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
CXX=${CXX:-c++}
OUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-hook-events.XXXXXX")
trap 'rm -rf "${OUT_DIR}"' EXIT
FLAGS=(-fno-omit-frame-pointer)
if [[ -n "${HOOK_EVENT_TEST_SANITIZERS:-}" ]]; then
    FLAGS+=(-fsanitize="${HOOK_EVENT_TEST_SANITIZERS}")
fi
"${CXX}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread "${FLAGS[@]}" \
    -I"${ROOT_DIR}/include" "${ROOT_DIR}/tests/hook_event_sink_test.cpp" \
    -o "${OUT_DIR}/hook_event_sink_test"
"${OUT_DIR}/hook_event_sink_test"
# Give the cancellation probe a much longer natural deadline, so losing
# cancellation cannot be masked by the production 100 ms send timeout.
"${CXX}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread "${FLAGS[@]}" \
    -DDEJAVU_EVENT_SEND_TIMEOUT_MS=5000 -DDEJAVU_EVENT_CANCELLATION_PROBE=1 \
    -I"${ROOT_DIR}/include" "${ROOT_DIR}/tests/hook_event_sink_test.cpp" \
    -o "${OUT_DIR}/hook_event_cancellation_test"
"${OUT_DIR}/hook_event_cancellation_test" --cancellation
"${CXX}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread "${FLAGS[@]}" \
    -I"${ROOT_DIR}/tests/event-stubs" -I"${ROOT_DIR}/include" -I"${PROJECT_DIR}/include" \
    "${ROOT_DIR}/src/hook_api_stream.cpp" "${ROOT_DIR}/tests/hook_api_stream_test.cpp" \
    -o "${OUT_DIR}/hook_api_stream_test"
"${OUT_DIR}/hook_api_stream_test"
"${CXX}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread "${FLAGS[@]}" \
    -I"${ROOT_DIR}/include" "${ROOT_DIR}/src/hook_event_sink.cpp" \
    "${ROOT_DIR}/tests/hook_event_lease_test.cpp" -o "${OUT_DIR}/hook_event_lease_test"
"${OUT_DIR}/hook_event_lease_test"
