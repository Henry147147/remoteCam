cmake_minimum_required(VERSION 3.21)
project(RemoteCam VERSION 0.1.0 LANGUAGES CXX)

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
    add_subdirectory(windows)
endif()

# CPack has to be configured from the top-level scope or CPackConfig.cmake lands in a
# subdirectory where `cpack` cannot find it. The settings themselves live under
# windows/ with everything else that is Windows-specific.
if(WIN32 AND RC_BUILD_INSTALLER)
    include(windows/installer/RemoteCamPackaging.cmake)
endif()
