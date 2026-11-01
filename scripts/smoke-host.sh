#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CC=${CC:-cc}
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/host-smoke"}
OBJ_DIR="${OUT_DIR}/obj"
GEN_DIR="${OUT_DIR}/generated"
TINYCC_SRC="${OUT_DIR}/tinycc-src"
TINYCC_PATCH="${ROOT_DIR}/patches/tinycc-runtime-safety.patch"

mkdir -p "${OBJ_DIR}" "${GEN_DIR}"

# TinyCC is pinned as a submodule; compile from a pristine + patched copy so the
# submodule stays clean and the runtime-safety patch (writable-data page
# separation and non-fatal relocation errors) is always applied.
prepare_tinycc() {
    rm -rf "${TINYCC_SRC}"
    mkdir -p "${TINYCC_SRC}"
    git -C "${ROOT_DIR}/extern/tinycc" archive --format=tar HEAD | tar -xf - -C "${TINYCC_SRC}"
    patch -d "${TINYCC_SRC}" -p1 < "${TINYCC_PATCH}"
}
prepare_tinycc
cat > "${GEN_DIR}/config.h" <<'EOF'
#ifndef TCC_CONFIG_H
#define TCC_CONFIG_H
#define TCC_VERSION "0.9.27-frida"
#define CONFIG_TCCDIR "/tmp/dejavu/tcc"
#endif
EOF

COMMON_FLAGS=(
    -std=c11
    -O2
    -g
    -fPIC
    -fno-omit-frame-pointer
)
PROJECT_INCLUDES=(
    -I"${ROOT_DIR}/include"
    -I"${ROOT_DIR}/extern/lua"
    -I"${TINYCC_SRC}"
)

for source in dejavu dejavu_engine dejavu_lua; do
    "${CC}" "${COMMON_FLAGS[@]}" -Wall -Wextra -Werror \
        -DDEJAVU_TCC_VERSION='"0.9.27-frida"' \
        "${PROJECT_INCLUDES[@]}" \
        -c "${ROOT_DIR}/src/${source}.c" -o "${OBJ_DIR}/${source}.o"
done

"${CC}" "${COMMON_FLAGS[@]}" -DMAKE_LIB -DLUA_USE_LINUX \
    -I"${ROOT_DIR}/extern/lua" \
    -c "${ROOT_DIR}/extern/lua/onelua.c" -o "${OBJ_DIR}/lua.o"

"${CC}" "${COMMON_FLAGS[@]}" -DONE_SOURCE=1 \
    -I"${GEN_DIR}" \
    -I"${TINYCC_SRC}" \
    -c "${TINYCC_SRC}/libtcc.c" -o "${OBJ_DIR}/tinycc.o"

"${CC}" "${COMMON_FLAGS[@]}" -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    -c "${ROOT_DIR}/tests/smoke.c" -o "${OBJ_DIR}/smoke.o"

"${CC}" \
    "${OBJ_DIR}/dejavu.o" \
    "${OBJ_DIR}/dejavu_engine.o" \
    "${OBJ_DIR}/dejavu_lua.o" \
    "${OBJ_DIR}/lua.o" \
    "${OBJ_DIR}/tinycc.o" \
    "${OBJ_DIR}/smoke.o" \
    -ldl -lm -pthread \
    -o "${OUT_DIR}/smoke"

"${OUT_DIR}/smoke"

echo "== JIT ABI header compile"
"${CC}" "${COMMON_FLAGS[@]}" -std=c11 -Wall -Wextra -Werror     -I"${ROOT_DIR}/include" -fsyntax-only "${ROOT_DIR}/tests/jit_abi_compile.c"

echo "== SHA-256 known answers"
"${CXX:-c++}" -std=c++17 -O2 -g -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/zygisk/include" \
    "${ROOT_DIR}/tests/sha256_test.cpp" "${ROOT_DIR}/zygisk/src/sha256.cpp" \
    -o "${OUT_DIR}/sha256_test"
"${OUT_DIR}/sha256_test"

echo "== MCP protocol"
PYTHONDONTWRITEBYTECODE=1 python3 "${ROOT_DIR}/tests/mcp_server_test.py"

if [[ "$(uname -s)" == "Linux" ]]; then
    # Simulate an execmem-restricted domain: deny PROT_EXEC mprotect calls and
    # verify the engine reports an error instead of exiting the process.
    "${CC}" "${COMMON_FLAGS[@]}" -Wall -Wextra -Werror \
        -I"${ROOT_DIR}/include" \
        "${ROOT_DIR}/tests/relocate_failure.c" \
        "${OBJ_DIR}/dejavu.o" \
        "${OBJ_DIR}/dejavu_engine.o" \
        "${OBJ_DIR}/dejavu_lua.o" \
        "${OBJ_DIR}/lua.o" \
        "${OBJ_DIR}/tinycc.o" \
        -Wl,--wrap=mprotect \
        -ldl -lm -pthread \
        -o "${OUT_DIR}/relocate_failure"
    "${OUT_DIR}/relocate_failure"
fi
