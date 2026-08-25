#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CC=${CC:-cc}
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/host-smoke"}
OBJ_DIR="${OUT_DIR}/obj"
GEN_DIR="${OUT_DIR}/generated"

mkdir -p "${OBJ_DIR}" "${GEN_DIR}"
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
    -I"${ROOT_DIR}/extern/tinycc"
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
    -I"${ROOT_DIR}/extern/tinycc" \
    -c "${ROOT_DIR}/extern/tinycc/libtcc.c" -o "${OBJ_DIR}/tinycc.o"

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
