cmake_minimum_required(VERSION 3.28)
project(dejavu_zygisk LANGUAGES C CXX ASM)

if(NOT ANDROID)
    message(FATAL_ERROR "The Zygisk module must be cross-compiled for Android")
endif()
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LSPLANT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(DEX_BUILDER_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(
    third_party/LSPlant/lsplant/src/main/jni
    ${CMAKE_CURRENT_BINARY_DIR}/lsplant
    EXCLUDE_FROM_ALL
)

add_library(dobby_static STATIC IMPORTED GLOBAL)
set_target_properties(dobby_static PROPERTIES
    IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/DobbyPrefab/prefab/modules/dobby/libs/android.arm64-v8a/libdobby.a
    INTERFACE_INCLUDE_DIRECTORIES
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/DobbyPrefab/prefab/modules/dobby/include
)

set(HOOK_BRIDGE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/hook-bridge)
add_custom_command(
    OUTPUT ${HOOK_BRIDGE_GENERATED_DIR}/hook_bridge_dex.h
    COMMAND
        ${CMAKE_COMMAND} -E env
        DEJAVU_RUN_DEVICE_STRESS=$<IF:$<BOOL:${DEJAVU_RUN_DEVICE_STRESS}>,1,0>
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-hook-bridge.sh
        ${HOOK_BRIDGE_GENERATED_DIR}
    DEPENDS
        java/io/dejavu/bridge/HookBridge.java
        tests/device/java/io/dejavu/bridge/HookStress.java
        scripts/generate-hook-bridge.sh
    VERBATIM
)

add_library(dejavu_agent SHARED
    ${HOOK_BRIDGE_GENERATED_DIR}/hook_bridge_dex.h
    src/aarch64_adrp_add.cpp
    src/art_elf_resolver.cpp
    src/agent.cpp
    src/control_channel.cpp
    src/dejavu_runtime.cpp
    src/hook_api_java_call.cpp
    src/hook_api_primitives.cpp
    src/hook_api_runtime.cpp
    src/hook_api_strings.cpp
    src/hook_compile_diagnostics.cpp
    src/hook_control.cpp
    src/hook_dispatch.cpp
    src/hook_manager.cpp
    src/hook_signature.cpp
    src/lsplant_backend.cpp
    src/lsplant_runtime.cpp
)

add_library(dejavu_zygisk SHARED
    src/control_channel.cpp
    src/module.cpp
    src/target_config.cpp
)

target_include_directories(dejavu_agent PRIVATE
    include
    ../include
    ${HOOK_BRIDGE_GENERATED_DIR}
)
target_include_directories(dejavu_zygisk PRIVATE
    include
    ../include
)
target_compile_definitions(dejavu_agent PRIVATE
    DEJAVU_WITH_LSPLANT=1
    DEJAVU_RUN_DEVICE_SMOKE=$<IF:$<BOOL:${DEJAVU_RUN_DEVICE_SMOKE}>,1,0>
    DEJAVU_RUN_DEVICE_STRESS=$<IF:$<BOOL:${DEJAVU_RUN_DEVICE_STRESS}>,1,0>
)
target_compile_options(dejavu_agent PRIVATE
    -Wall
    -Wextra
    -Werror
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections
    -fno-omit-frame-pointer
)
target_link_options(dejavu_agent PRIVATE
    -Wl,--gc-sections
    -Wl,--no-undefined
    -Wl,-z,max-page-size=16384
    -Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/config/agent-exports.map
)
target_link_libraries(dejavu_agent PRIVATE
    lsplant_static
    dobby_static
    log
    dl
    z
)
target_compile_options(dejavu_zygisk PRIVATE
    -Wall
    -Wextra
    -Werror
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections
    -fno-omit-frame-pointer
)
target_link_options(dejavu_zygisk PRIVATE
    -Wl,--gc-sections
    -Wl,--no-undefined
    -Wl,-z,max-page-size=16384
    -Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/config/exports.map
)
target_link_libraries(dejavu_zygisk PRIVATE log dl)
set_target_properties(dejavu_agent PROPERTIES
    OUTPUT_NAME dejavu_agent
    ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON
)
set_target_properties(dejavu_zygisk PROPERTIES
    OUTPUT_NAME dejavu_zygisk
    ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON
)
