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
add_subdirectory(net)         # rcwin-net        TCP receiver the phone connects to
add_subdirectory(security)    # SPAKE2, authenticated sessions, DPAPI pairing store
add_subdirectory(platform)    # decoder, D3D11 transform and frame pipeline
add_subdirectory(vcam)        # rc-vcam.dll      MF media source, loaded in Session 0
add_subdirectory(register)    # rc-vcam-register.exe
add_subdirectory(tools)       # rc-vcam-probe, rc-fakewriter, rc-fakepc
add_subdirectory(backend)     # rcwin-backend    auth-gated session controller

# The M1 virtual-camera targets intentionally build without Qt, because the Windows
# bring-up box and CI only need the SDK. Build the desktop producer when a dynamic Qt 6
# installation is available, but do not make that an artificial dependency of the DLL.
option(RC_BUILD_QT_APP "Build the Qt 6 / QML RemoteCam producer" ON)
if(RC_BUILD_QT_APP)
    find_package(Qt6 6.2 QUIET COMPONENTS Quick QuickControls2 Widgets)
    if(Qt6_FOUND)
        add_subdirectory(app)
    else()
        message(STATUS
            "Qt 6 Quick/QuickControls2 not found; skipping rc-app. "
            "Set CMAKE_PREFIX_PATH to a dynamic Qt 6 installation to build it.")
    endif()
endif()

# Embed consistent Explorer/version metadata in every executable or DLL that ships.
# Defining the resources here keeps the individual component build files focused on
# their runtime responsibilities while still covering non-Qt M1 builds.
function(rc_add_windows_version_resource target original_filename description file_type)
    set(RC_VERSION_ORIGINAL_FILENAME "${original_filename}")
    set(RC_VERSION_FILE_DESCRIPTION "${description}")
    set(RC_VERSION_FILE_TYPE "${file_type}")
    set(_rc_version_resource
        "${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/installer/RemoteCamVersion.rc.in"
        "${_rc_version_resource}"
        @ONLY)
    target_sources(${target} PRIVATE "${_rc_version_resource}")
endfunction()

rc_add_windows_version_resource(
    rc-vcam "rc-vcam.dll" "RemoteCam Media Foundation virtual camera" VFT_DLL)
rc_add_windows_version_resource(
    rc-vcam-register "rc-vcam-register.exe" "RemoteCam virtual camera registration tool" VFT_APP)
if(TARGET rc-app)
    rc_add_windows_version_resource(
        rc-app "RemoteCam.exe" "RemoteCam desktop application" VFT_APP)
endif()

# installer/ holds the CPack + NSIS configuration, but it is not a subdirectory: CPack
# has to be configured from the top-level scope, so the root CMakeLists includes
# windows/installer/RemoteCamPackaging.cmake directly when RC_BUILD_INSTALLER is ON.

# Not started yet -- uncomment as each lands.
# add_subdirectory(obs-plugin)
