cmake_minimum_required(VERSION 3.28)
project(dejavu_unpack_plugin LANGUAGES C CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "The unpack example plugin must be cross-compiled for Android")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(DEJAVU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(dejavu_unpack_xz STATIC
    ${DEJAVU_ROOT}/zygisk/third_party/xz-embedded/xz_crc32.c
    ${DEJAVU_ROOT}/zygisk/third_party/xz-embedded/xz_crc64.c
    ${DEJAVU_ROOT}/zygisk/third_party/xz-embedded/xz_sha256.c
    ${DEJAVU_ROOT}/zygisk/third_party/xz-embedded/xz_dec_bcj.c
    ${DEJAVU_ROOT}/zygisk/third_party/xz-embedded/xz_dec_lzma2.c
    ${DEJAVU_ROOT}/zygisk/third_party/xz-embedded/xz_dec_stream.c
)
target_include_directories(dejavu_unpack_xz PUBLIC
    ${DEJAVU_ROOT}/zygisk/third_party/xz-embedded
)
target_compile_definitions(dejavu_unpack_xz
    PUBLIC
        DEJAVU_WITH_XZ_EMBEDDED=1
        XZ_USE_CRC64
        XZ_DEC_CONCATENATED
    PRIVATE
        XZ_USE_SHA256
        XZ_DEC_ANY_CHECK
        XZ_DEC_ARM64
        XZ_DEC_ARM
        XZ_DEC_ARMTHUMB
)
target_compile_options(dejavu_unpack_xz PRIVATE
    -w
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
)

add_library(dejavu_unpack SHARED
    src/unpack_manager.cpp
    src/plugin_entry.cpp
    ${DEJAVU_ROOT}/zygisk/src/art_elf_resolver.cpp
)
target_include_directories(dejavu_unpack PRIVATE
    ${DEJAVU_ROOT}/include
    ${DEJAVU_ROOT}/zygisk/include
    ${DEJAVU_ROOT}/examples/common
    src
)
target_compile_options(dejavu_unpack PRIVATE
    -Wall
    -Wextra
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
)
target_link_options(dejavu_unpack PRIVATE
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
    -Wl,--no-undefined
)
target_link_libraries(dejavu_unpack PRIVATE
    dejavu_unpack_xz
    log
    dl
)
