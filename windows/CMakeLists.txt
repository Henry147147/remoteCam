# Windows client. Added by the root CMakeLists only when WIN32, so a Linux or macOS
# host can still configure and test core/.
#
# See windows/CLAUDE.md for the target list and the order to build them in.

if(NOT WIN32)
    message(FATAL_ERROR "windows/ requires a Windows host; it does not cross-compile.")
endif()

# The Frame Server (svchost.exe -k Camera) is x64 and will never load a 32-bit
# in-proc server. A 32-bit build produces a camera that registers, enumerates, and
# then silently never delivers a frame -- so fail at configure time instead, where
# the cause is obvious.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "windows/ must be built for x64. The Media Foundation Frame Server is a 64-bit "
        "process and cannot load a 32-bit rc-vcam.dll. Configure with -A x64.")
endif()

if(CMAKE_SYSTEM_VERSION VERSION_LESS 10.0.22000)
    # MFCreateVirtualCamera is Windows 11 build 22000+. Warn rather than fail: the
    # SDK version, not the running OS, is what the build actually needs.
    message(WARNING
        "Targeting a Windows SDK older than 10.0.22000. MFCreateVirtualCamera "
        "requires Windows 11 build 22000+ and the virtual camera will not build.")
endif()

add_subdirectory(common)      # rcwin-common     Win32 helpers, NV12, SHM ring
add_subdirectory(vcam)        # rc-vcam.dll      MF media source, loaded in Session 0
add_subdirectory(register)    # rc-vcam-register.exe
add_subdirectory(tools)       # rc-vcam-probe, rc-fakewriter

# Not started yet -- uncomment as each lands.
# add_subdirectory(platform)  # D3D11 pipeline, FFmpeg decode, effects, SHM writer
# add_subdirectory(app)       # Qt 6 / QML client
# add_subdirectory(obs-plugin)
# add_subdirectory(installer)
