cmake_minimum_required(VERSION 3.28)
project(dejavu_hello_plugin LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "Plugins must be cross-compiled for Android")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(DEJAVU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(dejavu_hello SHARED plugin_entry.cpp)
target_include_directories(dejavu_hello PRIVATE ${DEJAVU_ROOT}/include)
target_compile_options(dejavu_hello PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_options(dejavu_hello PRIVATE -Wl,--no-undefined)
target_link_libraries(dejavu_hello PRIVATE dl)
