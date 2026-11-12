#!/usr/bin/env bash
# Source from host build/deployment scripts; only operate on delivery copies.

resolve_llvm_strip() {
    local search_root candidate
    if [[ -n "${LLVM_STRIP:-}" ]]; then
        if [[ "${LLVM_STRIP}" == */* ]]; then
            [[ -x "${LLVM_STRIP}" ]] || return 1
        else
            candidate=$(command -v "${LLVM_STRIP}") || return 1
            LLVM_STRIP=${candidate}
        fi
        return 0
    fi
    for search_root in \
        "${ANDROID_NDK_HOME:-}" \
        "${ANDROID_NDK_ROOT:-}" \
        "${HOME}/Library/Android/sdk/ndk" \
        "${HOME}/Android/Sdk/ndk"; do
        [[ -d "${search_root}" ]] || continue
        candidate=$(find "${search_root}" \
            -path '*/toolchains/llvm/prebuilt/*/bin/llvm-strip' \
            -print -quit 2>/dev/null || true)
        if [[ -n "${candidate}" && -x "${candidate}" ]]; then
            LLVM_STRIP=${candidate}
            return 0
        fi
    done
    candidate=$(command -v llvm-strip) || return 1
    LLVM_STRIP=${candidate}
}

strip_runtime_files() {
    if ! resolve_llvm_strip; then
        echo "error: llvm-strip not found; set LLVM_STRIP to the Android NDK llvm-strip" >&2
        return 1
    fi
    "${LLVM_STRIP}" --strip-unneeded "$@" || {
        local status=$?
        echo "error: llvm-strip failed for runtime delivery copies" >&2
        return "${status}"
    }
}
