# Provide CMake package `cxx::cxx` directly from Android NDK r30+'s bundled
# libc++ C++23 standard library module sources (`std.cppm` / `std.compat.cppm`).
# This avoids external Prefab/Maven binaries while keeping LSPlant ABI-identical
# with Dejavu's static libc++ runtime.

if(TARGET cxx::cxx)
    set(cxx_FOUND TRUE)
    return()
endif()

if(NOT DEFINED ANDROID_TOOLCHAIN_ROOT OR NOT EXISTS "${ANDROID_TOOLCHAIN_ROOT}")
    message(FATAL_ERROR "cxxConfig.cmake requires ANDROID_TOOLCHAIN_ROOT from Android NDK r30+")
endif()

set(_dejavu_libcxx_include "${ANDROID_TOOLCHAIN_ROOT}/sysroot/usr/include/c++/v1")
set(_dejavu_libcxx_share "${ANDROID_TOOLCHAIN_ROOT}/share/libc++/v1")
set(_dejavu_std_cppm "${_dejavu_libcxx_share}/std.cppm")
set(_dejavu_std_compat_cppm "${_dejavu_libcxx_share}/std.compat.cppm")

if(NOT EXISTS "${_dejavu_std_cppm}" OR NOT EXISTS "${_dejavu_std_compat_cppm}")
    message(FATAL_ERROR
        "Android NDK r30+ libc++ C++23 module sources not found in ${_dejavu_libcxx_share}")
endif()

add_library(dejavu_ndk_cxx_modules STATIC)
set_target_properties(dejavu_ndk_cxx_modules PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_SCAN_FOR_MODULES ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_sources(dejavu_ndk_cxx_modules
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS "${_dejavu_libcxx_share}"
        FILES
            "${_dejavu_std_cppm}"
            "${_dejavu_std_compat_cppm}"
)
target_include_directories(dejavu_ndk_cxx_modules SYSTEM PRIVATE "${_dejavu_libcxx_share}")
target_compile_options(dejavu_ndk_cxx_modules
    PRIVATE
        -U_FORTIFY_SOURCE
        -D__BIONIC_CTYPE_INLINE=inline
        -Wno-reserved-module-identifier
        -Wno-reserved-user-defined-literal
    INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:-isystem;${_dejavu_libcxx_include}>"
)

add_library(cxx::cxx INTERFACE IMPORTED)
set_target_properties(cxx::cxx PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_dejavu_libcxx_include}"
    INTERFACE_LINK_LIBRARIES "dejavu_ndk_cxx_modules"
)

set(cxx_FOUND TRUE)
