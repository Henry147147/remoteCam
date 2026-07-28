cmake_minimum_required(VERSION 3.21)
project(RemoteCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RC_BUILD_TESTS "Build rc-core unit tests" ON)

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
