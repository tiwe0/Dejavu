#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
CACHE_DIR="${THIRD_PARTY_DIR}/.cache"

mkdir -p "${THIRD_PARTY_DIR}" "${CACHE_DIR}"

fetch_archive() {
    local name=$1
    local commit=$2
    local sha256=$3
    local url=$4
    local destination=$5
    local archive="${CACHE_DIR}/${name}-${commit}.tar.gz"
    local staging

    if [[ -f "${destination}/.dejavu-pin" ]] &&
        [[ "$(<"${destination}/.dejavu-pin")" == "${commit}" ]]; then
        return
    fi

    if [[ ! -f "${archive}" ]] ||
        [[ "$(shasum -a 256 "${archive}" | awk '{print $1}')" != "${sha256}" ]]; then
        curl -L --fail --retry 3 --output "${archive}" "${url}"
    fi
    if [[ "$(shasum -a 256 "${archive}" | awk '{print $1}')" != "${sha256}" ]]; then
        echo "error: checksum mismatch for ${name}" >&2
        exit 1
    fi

    staging=$(mktemp -d "${THIRD_PARTY_DIR}/.${name}.XXXXXX")
    tar -xzf "${archive}" --strip-components=1 -C "${staging}"
    printf '%s\n' "${commit}" > "${staging}/.dejavu-pin"
    if [[ -e "${destination}" ]]; then
        find "${destination}" -mindepth 1 -delete
        rmdir "${destination}"
    fi
    mv "${staging}" "${destination}"
}

fetch_archive \
    lsplant \
    d8b5d1dbb664abc606644036822e4bb64547edf6 \
    d640013c09037c2b771a68127b2910076715e639e814864287af5e8e4dcb06c9 \
    https://github.com/LSPosed/LSPlant/archive/d8b5d1dbb664abc606644036822e4bb64547edf6.tar.gz \
    "${THIRD_PARTY_DIR}/LSPlant"

fetch_archive \
    dex-builder \
    ac7fb2230954ee311808bad469b0db501f31bfb8 \
    23fdcd5e92acb74b4d0b1b77554c93077ec7255538d852124c9b794d88284efc \
    https://github.com/LSPosed/DexBuilder/archive/ac7fb2230954ee311808bad469b0db501f31bfb8.tar.gz \
    "${THIRD_PARTY_DIR}/LSPlant/lsplant/src/main/jni/external/dex_builder"

fetch_archive \
    parallel-hashmap \
    0cd57d29a959256ed66b2afdd1009928fc625d09 \
    88f37e8c06b034e972b65abcd55ebc753d49374be801006d98faec3f16512684 \
    https://github.com/greg7mdp/parallel-hashmap/archive/0cd57d29a959256ed66b2afdd1009928fc625d09.tar.gz \
    "${THIRD_PARTY_DIR}/LSPlant/lsplant/src/main/jni/external/dex_builder/external/parallel_hashmap"

DOBBY_PATCH="${ROOT_DIR}/patches/dobby-native-publication.patch"
DOBBY_PATCH_HASH=$(shasum -a 256 "${DOBBY_PATCH}" | awk '{print $1}')
DOBBY_PATCH_STAMP="${THIRD_PARTY_DIR}/Dobby/.dejavu-native-patch"
if [[ -f "${THIRD_PARTY_DIR}/Dobby/.dejavu-pin" ]] &&
    { [[ ! -f "${DOBBY_PATCH_STAMP}" ]] || [[ "$(<"${DOBBY_PATCH_STAMP}")" != "${DOBBY_PATCH_HASH}" ]]; }; then
    # A changed maintained patch is always reapplied to the checksum-pinned
    # pristine archive, never on top of an older generated dependency tree.
    rm "${THIRD_PARTY_DIR}/Dobby/.dejavu-pin"
fi

fetch_archive \
    dobby \
    5dfc8546954ce3b3198132ab13fddb89ee92cdd7 \
    01a417233c40929aa21098c8a9724c00f6f05b5cde3f11abcac84b8e00053748 \
    https://github.com/jmpews/Dobby/archive/5dfc8546954ce3b3198132ab13fddb89ee92cdd7.tar.gz \
    "${THIRD_PARTY_DIR}/Dobby"

if [[ ! -f "${DOBBY_PATCH_STAMP}" ]] || [[ "$(<"${DOBBY_PATCH_STAMP}")" != "${DOBBY_PATCH_HASH}" ]]; then
    patch -d "${THIRD_PARTY_DIR}/Dobby" -p1 < "${DOBBY_PATCH}"
    printf '%s\n' "${DOBBY_PATCH_HASH}" > "${DOBBY_PATCH_STAMP}"
fi

LSPLANT_PATCH_STAMP="${THIRD_PARTY_DIR}/LSPlant/.dejavu-patches-v4"
if [[ ! -f "${LSPLANT_PATCH_STAMP}" ]]; then
    patch -d "${THIRD_PARTY_DIR}/LSPlant" -p1 \
        < "${ROOT_DIR}/patches/lsplant-main-reflection-shorty.patch"
    printf '%s\n' "reflection-shorty-v2" > "${LSPLANT_PATCH_STAMP}"
fi

echo "OK: pinned LSPlant dependencies are available in ${THIRD_PARTY_DIR}"
