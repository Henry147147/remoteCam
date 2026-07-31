cmake_minimum_required(VERSION 3.21)
project(RemoteCam VERSION 0.1.0 LANGUAGES CXX)

# Keep one machine-readable version beside CMakeCache.txt. Release automation reads
# this instead of duplicating the version in workflow YAML or release notes.
set(RC_WINDOWS_SIGNING_STATUS "unsigned" CACHE STRING
    "Informational status embedded in Windows packages: signed or unsigned")
set_property(CACHE RC_WINDOWS_SIGNING_STATUS PROPERTY STRINGS signed unsigned)
if(NOT RC_WINDOWS_SIGNING_STATUS MATCHES "^(signed|unsigned)$")
    message(FATAL_ERROR "RC_WINDOWS_SIGNING_STATUS must be signed or unsigned")
endif()
if(RC_WINDOWS_SIGNING_STATUS STREQUAL "signed")
    set(RC_WINDOWS_SIGNING_DETAIL
        "Authenticode signatures with RFC 3161 timestamps are required and verified by CI.")
else()
    set(RC_WINDOWS_SIGNING_DETAIL
        "No Authenticode signature was applied. Windows may show an unknown-publisher warning.")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RemoteCamVersion.txt.in"
    "${CMAKE_CURRENT_BINARY_DIR}/RemoteCamVersion.txt"
    @ONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RemoteCamSigningStatus.txt.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SIGNING-STATUS.txt"
    @ONLY)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" _rc_vcpkg_manifest)
string(JSON _rc_vcpkg_version GET "${_rc_vcpkg_manifest}" version-string)
if(NOT _rc_vcpkg_version STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "project() version ${PROJECT_VERSION} and vcpkg version ${_rc_vcpkg_version} differ")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RC_BUILD_TESTS "Build rc-core unit tests" ON)
option(RC_BUILD_INSTALLER "Build the Windows .exe installer (needs Qt 6.5+ and NSIS)" OFF)
option(RC_WITH_FFMPEG "Build the Windows FFmpeg d3d11va decoder" OFF)
option(RC_WARNINGS_AS_ERRORS "Treat compiler warnings as build failures" ON)
option(RC_ENABLE_MSVC_ANALYZE "Run MSVC native code analysis" OFF)

# One warning policy for every source target. Keeping it here closes the easy-to-miss
# gap where a newly added test or tool compiles with the compiler defaults while the
# library beside it is strict.
function(rc_target_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(RC_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(RC_ENABLE_MSVC_ANALYZE)
            target_compile_options(${target} PRIVATE /analyze)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(RC_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(RC_BUILD_TESTS)
    enable_testing()
endif()

# Portable core. Builds anywhere -- this is the only target available on a
# non-Windows host, and it is where the transform math and wire protocol live.
add_subdirectory(core)

# Windows client, virtual camera, OBS plugin and installer. Guarded because the
# project is developed on Linux and built for Windows in CI.
if(WIN32)
    enable_language(RC)
    add_subdirectory(windows)
endif()

# CPack has to be configured from the top-level scope or CPackConfig.cmake lands in a
# subdirectory where `cpack` cannot find it. The settings themselves live under
# windows/ with everything else that is Windows-specific.
if(WIN32 AND RC_BUILD_INSTALLER)
    include(windows/installer/RemoteCamPackaging.cmake)
endif()
