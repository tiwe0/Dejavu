cmake_minimum_required(VERSION 3.28)
project(dejavu_router LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(DEJAVU_ROUTER_SYSTEM_LIBEVENT "Use a host-provided libevent instead of the pinned source" OFF)
if(DEJAVU_ROUTER_SYSTEM_LIBEVENT)
    if(ANDROID)
        message(FATAL_ERROR "Android builds must use the pinned static libevent")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ROUTER_EVENT REQUIRED IMPORTED_TARGET libevent_core)
    set(ROUTER_EVENT_LIBRARIES PkgConfig::ROUTER_EVENT)
else()
    include(FetchContent)
    set(EVENT__LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
    set(EVENT__DISABLE_OPENSSL ON CACHE BOOL "" FORCE)
    set(EVENT__DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(EVENT__DISABLE_REGRESS ON CACHE BOOL "" FORCE)
    set(EVENT__DISABLE_SAMPLES ON CACHE BOOL "" FORCE)
    set(EVENT__DISABLE_BENCHMARK ON CACHE BOOL "" FORCE)
    set(EVENT__DISABLE_THREAD_SUPPORT ON CACHE BOOL "" FORCE)
    # libevent 2.1 caches a failed symbol probe across reconfiguration. Bionic
    # needs this feature macro for pipe2, even on an already configured tree.
    if(ANDROID)
        set(_GNU_SOURCE TRUE)
    endif()
    FetchContent_Declare(dejavu_libevent
        URL https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz
        URL_HASH SHA256=92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # CMake 4 removed libevent 2.1's pre-3.5 policy compatibility. Scope this
    # floor to the dependency and preserve any higher policy version supplied
    # by the caller.
    block(SCOPE_FOR VARIABLES)
        if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM OR CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS 3.5)
            set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        endif()
        FetchContent_MakeAvailable(dejavu_libevent)
    endblock()
    set(ROUTER_EVENT_LIBRARIES event_core_static)
endif()

add_executable(dejavu_router src/tcp_router.c third_party/cJSON/cJSON.c)
target_include_directories(dejavu_router PRIVATE third_party/cJSON)
target_compile_options(dejavu_router PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dejavu_router PRIVATE ${ROUTER_EVENT_LIBRARIES} m)
set_target_properties(dejavu_router PROPERTIES
    OUTPUT_NAME dejavuerouter
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(ANDROID)
    target_link_options(dejavu_router PRIVATE -Wl,-z,max-page-size=16384 -Wl,--no-undefined)
    set_target_properties(dejavu_router PROPERTIES ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES ON)
endif()
