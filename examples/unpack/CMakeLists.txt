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

add_library(dejavu_unpack SHARED
    src/unpack_manager.cpp
    src/plugin_entry.cpp
    ${DEJAVU_ROOT}/zygisk/src/art_elf_resolver.cpp
)
target_include_directories(dejavu_unpack PRIVATE
    ${DEJAVU_ROOT}/include
    ${DEJAVU_ROOT}/zygisk/include
    src
)
target_compile_options(dejavu_unpack PRIVATE
    -Wall
    -Wextra
    -fvisibility=hidden
)
target_link_options(dejavu_unpack PRIVATE
    -Wl,--no-undefined
)
target_link_libraries(dejavu_unpack PRIVATE
    log
    dl
)
