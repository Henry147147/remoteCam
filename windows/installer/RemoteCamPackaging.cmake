# CPack + NSIS configuration for RemoteCam-<version>-win64.exe.
#
# Included from the root CMakeLists rather than added as a subdirectory: include(CPack)
# writes CPackConfig.cmake into the current binary directory, and `cpack` looks for it
# in the build root. Being include()d means this file runs in the top-level scope, so
# the CPACK_* variables it sets are the ones CPack picks up.
#
# The payload is deliberately self-contained -- Qt, the VC runtime and the camera DLL
# all ship inside it. A user installing this has no SDK, no redistributable and no Qt.

if(NOT TARGET rc-app)
    message(FATAL_ERROR
        "RC_BUILD_INSTALLER is ON but rc-app is not being built, so the package would "
        "contain a virtual camera and nothing to drive it. Point CMAKE_PREFIX_PATH at a "
        "dynamic Qt 6.5+ installation, or turn RC_BUILD_INSTALLER off.")
endif()

# The debug CRT and the debug Qt libraries are not redistributable. A Debug package is
# therefore always a mistake, and one whose symptom on the target machine is a missing
# DLL dialog rather than anything pointing back here.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "Packaging requires a Release build; the debug runtime cannot be shipped.")
endif()
install(CODE [[
    if(CMAKE_INSTALL_CONFIG_NAME STREQUAL "Debug")
        message(FATAL_ERROR
            "Packaging requires a Release build; the debug runtime cannot be shipped. "
            "Build with --config Release and run cpack -C Release.")
    endif()
]])

# rc-vcam.dll and rc-vcam-register.exe link the static CRT (see windows/common), so
# these are only here for RemoteCam.exe and the Qt DLLs. App-local beside the
# executable is the one location the loader searches first and unconditionally.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "." RENAME "LICENSE.txt")
install(FILES "${PROJECT_SOURCE_DIR}/windows/installer/THIRD-PARTY-NOTICES.txt"
        DESTINATION ".")
install(FILES
    "${PROJECT_SOURCE_DIR}/windows/installer/GPL-3.0.txt"
    "${PROJECT_SOURCE_DIR}/windows/installer/LGPL-3.0.txt"
    DESTINATION ".")

set(CPACK_GENERATOR "NSIS")
set(CPACK_MONOLITHIC_INSTALL ON)

set(CPACK_PACKAGE_NAME "RemoteCam")
set(CPACK_PACKAGE_VENDOR "RemoteCam")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Use an iPhone as a webcam on Windows 11")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "RemoteCam")
set(CPACK_PACKAGE_FILE_NAME "RemoteCam-${PROJECT_VERSION}-win64")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")

# Stable across versions on purpose: this is the key CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL
# reads to find a previous install. The default includes the version number, which
# means every upgrade would fail to notice the release it is replacing.
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "RemoteCam")

set(CPACK_NSIS_PACKAGE_NAME "RemoteCam")
set(CPACK_NSIS_DISPLAY_NAME "RemoteCam")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/Henry147147/remoteCam")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

# x64 only -- windows/CMakeLists.txt fails to configure otherwise, because the Frame
# Server cannot load a 32-bit in-proc server. The CPack default is 32-bit Program Files.
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")

# Shortcuts are written by hand rather than through CPACK_PACKAGE_EXECUTABLES, whose
# generator hardcodes a bin/ subdirectory. The package is flat: rc-vcam-register.exe
# resolves rc-vcam.dll next to itself, and Qt resolves its plugins and QML modules
# relative to RemoteCam.exe, so one directory serves all three.
set(CPACK_NSIS_DEFINES "!define MUI_FINISHPAGE_RUN \\\"$INSTDIR/RemoteCam.exe\\\"")
set(CPACK_NSIS_CREATE_ICONS_EXTRA
    "  CreateShortCut \\\"$SMPROGRAMS/$STARTMENU_FOLDER/RemoteCam.lnk\\\" \\\"$INSTDIR/RemoteCam.exe\\\"")
set(CPACK_NSIS_DELETE_ICONS_EXTRA
    "  Delete \\\"$SMPROGRAMS/$MUI_TEMP/RemoteCam.lnk\\\"")

# The one thing only an installer can do. MFCreateVirtualCamera and the HKLM CLSID
# write both need admin; the NSIS template already declares RequestExecutionLevel
# admin, and rc-vcam-register.exe is deliberately asInvoker and will not self-elevate.
# Plain IntCmp rather than LogicLib ${If}: CPack runs this string through
# configure_file, which would eat the ${...}.
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    DetailPrint 'Registering the RemoteCam virtual camera'
    ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --register' $0
    IntCmp $0 0 rcRegisterDone 0 0
      MessageBox MB_ICONEXCLAMATION|MB_OK 'RemoteCam is installed, but registering the virtual camera failed (exit code $0). No camera will appear in Zoom, Teams or OBS until it succeeds. Run rc-vcam-register.exe --register from the RemoteCam install directory in an elevated Command Prompt to retry.'
    rcRegisterDone:

    DetailPrint 'Allowing inbound RemoteCam connections on TCP port 7890'
    nsExec::ExecToLog '\\\"$SYSDIR/netsh.exe\\\" advfirewall firewall delete rule name=\\\"RemoteCam\\\" program=\\\"$INSTDIR/RemoteCam.exe\\\"'
    Pop $0
    nsExec::ExecToLog '\\\"$SYSDIR/netsh.exe\\\" advfirewall firewall add rule name=\\\"RemoteCam\\\" dir=in action=allow enable=yes profile=private protocol=TCP localport=7890 program=\\\"$INSTDIR/RemoteCam.exe\\\"'
    Pop $0
    StrCmp $0 0 rcFirewallDone 0
      MessageBox MB_ICONEXCLAMATION|MB_OK 'RemoteCam is installed, but its private-network firewall rule could not be created (exit code $0). Automatic discovery may work while phone connections time out. Allow RemoteCam.exe through Windows Defender Firewall to continue.'
    rcFirewallDone:
")

# Runs before the file-deletion block in the template, so rc-vcam-register.exe still
# exists here. --unregister is idempotent and tolerates a camera that is already gone.
# It does not touch %ProgramData%\\RemoteCam, which is where every component logs.
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    nsExec::ExecToLog '\\\"$SYSDIR/netsh.exe\\\" advfirewall firewall delete rule name=\\\"RemoteCam\\\" program=\\\"$INSTDIR/RemoteCam.exe\\\"'
    Pop $0
    ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --unregister'
    ExpandEnvStrings $R0 '%ProgramData%'
    StrCmp $R0 '%ProgramData%' rcLogsDone 0
      RMDir /r '$R0/RemoteCam'
    rcLogsDone:
")

include(CPack)
