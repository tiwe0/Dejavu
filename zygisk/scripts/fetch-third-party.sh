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

fetch_aar() {
    local name=$1
    local version=$2
    local sha256=$3
    local url=$4
    local destination=$5
    local archive="${CACHE_DIR}/${name}-${version}.aar"
    local staging

    if [[ -f "${destination}/.dejavu-pin" ]] &&
        [[ "$(<"${destination}/.dejavu-pin")" == "${version}" ]]; then
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
    unzip -q "${archive}" -d "${staging}"
    printf '%s\n' "${version}" > "${staging}/.dejavu-pin"
    if [[ -e "${destination}" ]]; then
        find "${destination}" -mindepth 1 -delete
        rmdir "${destination}"
    fi
    mv "${staging}" "${destination}"
}

fetch_archive \
    lsplant \
    7217ac6f41e2bda549e4acb54e632abb03e5ccaf \
    d0bfedc5760b39768bfb965c27065fc0178ce7675ad667201417e76cffcaf12a \
    https://github.com/LSPosed/LSPlant/archive/7217ac6f41e2bda549e4acb54e632abb03e5ccaf.tar.gz \
    "${THIRD_PARTY_DIR}/LSPlant"

fetch_archive \
    dex-builder \
    9d57844a301077abf4c29e061b2458c56a363c8f \
    da8687186389c3ce7a37d54a0e5ae213b57e4d63b6bb2505f39da4e655c6d9fc \
    https://github.com/LSPosed/DexBuilder/archive/9d57844a301077abf4c29e061b2458c56a363c8f.tar.gz \
    "${THIRD_PARTY_DIR}/LSPlant/lsplant/src/main/jni/external/dex_builder"

fetch_archive \
    parallel-hashmap \
    65775fa09fecaa65d0b0022ab6bf091c0e509445 \
    a930ad39e956ee16183976aa64678f23404478a8edddd4a450e30c929514f74b \
    https://github.com/greg7mdp/parallel-hashmap/archive/65775fa09fecaa65d0b0022ab6bf091c0e509445.tar.gz \
    "${THIRD_PARTY_DIR}/LSPlant/lsplant/src/main/jni/external/dex_builder/external/parallel_hashmap"

fetch_archive \
    dobby \
    5dfc8546954ce3b3198132ab13fddb89ee92cdd7 \
    01a417233c40929aa21098c8a9724c00f6f05b5cde3f11abcac84b8e00053748 \
    https://github.com/jmpews/Dobby/archive/5dfc8546954ce3b3198132ab13fddb89ee92cdd7.tar.gz \
    "${THIRD_PARTY_DIR}/Dobby"

fetch_aar \
    dobby-prefab \
    1.2 \
    251f48ae21686d7f69276c50644ca345f450e45110057437f7d76bb14cddf3a1 \
    https://repo1.maven.org/maven2/io/github/vvb2060/ndk/dobby/1.2/dobby-1.2.aar \
    "${THIRD_PARTY_DIR}/DobbyPrefab"

LSPLANT_PATCH_STAMP="${THIRD_PARTY_DIR}/LSPlant/.dejavu-patches-v3"
if [[ ! -f "${LSPLANT_PATCH_STAMP}" ]]; then
    if [[ ! -f "${THIRD_PARTY_DIR}/LSPlant/.dejavu-patches-v1" ]]; then
        patch -d "${THIRD_PARTY_DIR}/LSPlant" -p1 \
            < "${ROOT_DIR}/patches/lsplant-v6.4-reflection-shorty.patch"
    fi
    if [[ ! -f "${THIRD_PARTY_DIR}/LSPlant/.dejavu-patches-v2" ]]; then
        patch -d "${THIRD_PARTY_DIR}/LSPlant" -p1 \
            < "${ROOT_DIR}/patches/lsplant-v6.4-android15-interpreter-bridge.patch"
    fi
    patch -d "${THIRD_PARTY_DIR}/LSPlant" -p1 \
        < "${ROOT_DIR}/patches/lsplant-v6.4-jit-do-collection.patch"
    printf '%s\n' "reflection-shorty-v1 android15-interpreter-bridge-v1 jit-do-collection-v1" \
        > "${LSPLANT_PATCH_STAMP}"
fi

echo "OK: pinned LSPlant dependencies are available in ${THIRD_PARTY_DIR}"
