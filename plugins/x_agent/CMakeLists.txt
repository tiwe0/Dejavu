cmake_minimum_required(VERSION 3.28)
project(dejavu_x_agent LANGUAGES C CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "X agent plugin must be cross-compiled for Android")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(DEJAVU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(CJSON_DIR "${DEJAVU_ROOT}/dejavurouter/third_party/cJSON" ABSOLUTE)

add_library(dejavu_x_agent SHARED
    plugin_entry.cpp
    ${CJSON_DIR}/cJSON.c
)
target_include_directories(dejavu_x_agent PRIVATE
    ${DEJAVU_ROOT}/include
    ${CJSON_DIR}
)
target_compile_options(dejavu_x_agent PRIVATE -Wall -Wextra -fvisibility=hidden -fno-exceptions -fno-rtti)
target_compile_definitions(dejavu_x_agent PRIVATE _GNU_SOURCE)
target_link_options(dejavu_x_agent PRIVATE -Wl,--no-undefined)
target_link_libraries(dejavu_x_agent PRIVATE dl log)
