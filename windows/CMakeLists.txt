# Windows client. Added by the root CMakeLists only when WIN32, so a Linux or macOS
# host can still configure and test core/.
#
# Empty for now -- see windows/CLAUDE.md for the target list and the order to build
# them in. Uncomment each as it lands.
#
# add_subdirectory(vcam)        # rc-vcam.dll     MF media source, loaded in Session 0
# add_subdirectory(register)    # rc-vcam-register.exe
# add_subdirectory(platform)    # D3D11 pipeline, FFmpeg decode, effects, SHM writer
# add_subdirectory(app)         # Qt 6 / QML client
# add_subdirectory(tools)       # rc-fakephone, rc-vcam-probe
# add_subdirectory(obs-plugin)
# add_subdirectory(installer)

if(NOT WIN32)
    message(FATAL_ERROR "windows/ requires a Windows host; it does not cross-compile.")
endif()

if(CMAKE_SYSTEM_VERSION VERSION_LESS 10.0.22000)
    # MFCreateVirtualCamera is Windows 11 build 22000+. Warn rather than fail: the
    # SDK version, not the running OS, is what the build actually needs.
    message(WARNING
        "Targeting a Windows SDK older than 10.0.22000. MFCreateVirtualCamera "
        "requires Windows 11 build 22000+ and the virtual camera will not build.")
endif()
