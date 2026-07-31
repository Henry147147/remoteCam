# CPack + NSIS configuration for RemoteCam-<version>-win64.exe.
#
# Included from the root CMakeLists rather than added as a subdirectory: include(CPack)
# writes CPackConfig.cmake into the current binary directory, and cpack looks for it
# in the build root. The payload is self-contained: Qt, the VC runtime, FFmpeg, the
# camera DLL, and the registration tool all ship app-local.

if(NOT TARGET rc-app)
    message(FATAL_ERROR
        "RC_BUILD_INSTALLER is ON but rc-app is not being built, so the package would "
        "contain a virtual camera and nothing to drive it. Point CMAKE_PREFIX_PATH at a "
        "dynamic Qt 6.5+ installation, or turn RC_BUILD_INSTALLER off.")
endif()

if(NOT RC_WITH_FFMPEG OR NOT FFMPEG_FOUND)
    message(FATAL_ERROR
        "RC_BUILD_INSTALLER requires the pinned dynamic FFmpeg build. Configure with "
        "RC_WITH_FFMPEG=ON, VCPKG_MANIFEST_FEATURES=ffmpeg, and the x64-windows "
        "triplet so the installed app has the decoder it was built to use.")
endif()

if(NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows")
    message(FATAL_ERROR
        "Packaging requires VCPKG_TARGET_TRIPLET=x64-windows. Static triplets would "
        "make FFmpeg non-replaceable and violate the release's LGPL policy.")
endif()

find_package(OpenSSL 3.5.7 EXACT REQUIRED COMPONENTS Crypto)
if(NOT OPENSSL_VERSION VERSION_EQUAL "3.5.7")
    message(FATAL_ERROR
        "Production packaging requires OpenSSL 3.5.7 exactly; found ${OPENSSL_VERSION}.")
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

# FFMPEG_INCLUDE_DIRS is rooted at <vcpkg-prefix>/include. Resolve the runtime and
# exact licence from there instead of copying whatever DLL happens to be on PATH.
# Exactly one DLL for each required library is expected from the pinned manifest; a
# surprise ABI split stops the release for review.
list(GET FFMPEG_INCLUDE_DIRS 0 _rc_ffmpeg_include_dir)
cmake_path(GET _rc_ffmpeg_include_dir PARENT_PATH _rc_ffmpeg_prefix)
file(GLOB _rc_ffmpeg_avcodec_dll "${_rc_ffmpeg_prefix}/bin/avcodec-*.dll")
file(GLOB _rc_ffmpeg_avutil_dll "${_rc_ffmpeg_prefix}/bin/avutil-*.dll")
list(LENGTH _rc_ffmpeg_avcodec_dll _rc_ffmpeg_avcodec_count)
list(LENGTH _rc_ffmpeg_avutil_dll _rc_ffmpeg_avutil_count)
if(NOT _rc_ffmpeg_avcodec_count EQUAL 1 OR NOT _rc_ffmpeg_avutil_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one avcodec and one avutil runtime under "
        "${_rc_ffmpeg_prefix}/bin; found ${_rc_ffmpeg_avcodec_count} and "
        "${_rc_ffmpeg_avutil_count}.")
endif()
set(_rc_ffmpeg_copyright "${_rc_ffmpeg_prefix}/share/ffmpeg/copyright")
if(NOT EXISTS "${_rc_ffmpeg_copyright}")
    message(FATAL_ERROR "Pinned FFmpeg licence is missing: ${_rc_ffmpeg_copyright}")
endif()

# The production manifest also pins dynamic OpenSSL 3. Package both shared runtime
# libraries even though pairing currently consumes libcrypto: keeping the coherent
# OpenSSL runtime together avoids a later libssl feature silently depending on a DLL
# that the installer never learned to ship.
file(GLOB _rc_openssl_crypto_dll "${_rc_ffmpeg_prefix}/bin/libcrypto-*-x64.dll")
file(GLOB _rc_openssl_ssl_dll "${_rc_ffmpeg_prefix}/bin/libssl-*-x64.dll")
list(LENGTH _rc_openssl_crypto_dll _rc_openssl_crypto_count)
list(LENGTH _rc_openssl_ssl_dll _rc_openssl_ssl_count)
if(NOT _rc_openssl_crypto_count EQUAL 1 OR NOT _rc_openssl_ssl_count EQUAL 1)
    message(FATAL_ERROR
        "Expected the pinned OpenSSL 3 dynamic runtime under ${_rc_ffmpeg_prefix}/bin; "
        "found ${_rc_openssl_crypto_count} libcrypto and ${_rc_openssl_ssl_count} "
        "libssl DLLs. Enable the vcpkg openssl feature with x64-windows.")
endif()
set(_rc_openssl_copyright "${_rc_ffmpeg_prefix}/share/openssl/copyright")
if(NOT EXISTS "${_rc_openssl_copyright}")
    message(FATAL_ERROR "Pinned OpenSSL licence is missing: ${_rc_openssl_copyright}")
endif()

install(FILES
    "${_rc_ffmpeg_avcodec_dll}"
    "${_rc_ffmpeg_avutil_dll}"
    DESTINATION ".")
install(FILES "${_rc_ffmpeg_copyright}"
        DESTINATION "." RENAME "FFMPEG-LGPL-2.1.txt")
install(FILES
    "${_rc_openssl_crypto_dll}"
    "${_rc_openssl_ssl_dll}"
    DESTINATION ".")
install(FILES "${_rc_openssl_copyright}"
        DESTINATION "." RENAME "OPENSSL-LICENSE.txt")

# rc-vcam.dll and rc-vcam-register.exe link the static CRT (see windows/common), so
# these are only here for RemoteCam.exe and the Qt DLLs. App-local beside the
# executable is the one location the loader searches first and unconditionally.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "." RENAME "LICENSE.txt")
install(FILES "${PROJECT_BINARY_DIR}/SIGNING-STATUS.txt" DESTINATION ".")
install(FILES "${PROJECT_SOURCE_DIR}/windows/installer/THIRD-PARTY-NOTICES.txt"
        DESTINATION ".")
install(FILES
    "${PROJECT_SOURCE_DIR}/windows/installer/GPL-3.0.txt"
    "${PROJECT_SOURCE_DIR}/windows/installer/LGPL-3.0.txt"
    DESTINATION ".")

# Audit the final staging tree after every install, including CPack's private staging
# install. This catches a missing runtime/license and prevents test tools from leaking
# into a release even when an install rule changes elsewhere.
get_filename_component(_rc_ffmpeg_avcodec_name "${_rc_ffmpeg_avcodec_dll}" NAME)
get_filename_component(_rc_ffmpeg_avutil_name "${_rc_ffmpeg_avutil_dll}" NAME)
get_filename_component(_rc_openssl_crypto_name "${_rc_openssl_crypto_dll}" NAME)
get_filename_component(_rc_openssl_ssl_name "${_rc_openssl_ssl_dll}" NAME)
set(RC_FFMPEG_AVCODEC_NAME "${_rc_ffmpeg_avcodec_name}")
set(RC_FFMPEG_AVUTIL_NAME "${_rc_ffmpeg_avutil_name}")
set(RC_OPENSSL_CRYPTO_NAME "${_rc_openssl_crypto_name}")
set(RC_OPENSSL_SSL_NAME "${_rc_openssl_ssl_name}")
configure_file(
    "${PROJECT_SOURCE_DIR}/windows/installer/VerifyPayload.cmake.in"
    "${PROJECT_BINARY_DIR}/VerifyPayload.cmake"
    @ONLY)
# Keep this install rule last: windows/app's Qt deploy script and every payload install
# rule have already been added by the time this top-level file is included.
install(SCRIPT "${PROJECT_BINARY_DIR}/VerifyPayload.cmake")

set(CPACK_GENERATOR "NSIS")
set(CPACK_MONOLITHIC_INSTALL ON)

set(CPACK_PACKAGE_NAME "RemoteCam")
set(CPACK_PACKAGE_VENDOR "RemoteCam")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Use an iPhone as a webcam on Windows 11")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "RemoteCam")
set(CPACK_PACKAGE_FILE_NAME "RemoteCam-${PROJECT_VERSION}-win64")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")

# Stable across versions on purpose: this is the key
# CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL reads to find a previous install.
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "RemoteCam")

set(CPACK_NSIS_PACKAGE_NAME "RemoteCam")
set(CPACK_NSIS_DISPLAY_NAME "RemoteCam")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/Henry147147/remoteCam")
set(CPACK_NSIS_MODIFY_PATH OFF)
# CPack's stock upgrade prompt runs from .onInit, before our OS/architecture gate.
# Perform the previous-version removal transaction in the guarded preinstall block.
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)

# CPack's NSIS template owns the elevation declaration used by both installer and
# uninstaller. Fail configuration if a different CMake distribution removes or
# weakens it, then audit the generated project.nsi after packaging as a second guard.
set(_rc_nsis_template "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in")
if(NOT EXISTS "${_rc_nsis_template}")
    message(FATAL_ERROR "Cannot verify CPack's NSIS template: ${_rc_nsis_template}")
endif()
file(READ "${_rc_nsis_template}" _rc_nsis_template_contents)
if(NOT _rc_nsis_template_contents MATCHES "RequestExecutionLevel[ \t]+admin")
    message(FATAL_ERROR
        "CPack's NSIS template does not request administrator elevation. RemoteCam "
        "must elevate before it registers the camera or changes the firewall.")
endif()

# CPack's stock NSIS uninstaller ignores failed Delete/RMDir operations and continues
# on to remove its own executable and Add/Remove Programs metadata. Start with the
# installed CMake template, add a guarded post-delete checkpoint, and expose that
# generated template only to the NSIS generator. The registration helper is staged
# under a private temporary name while the generated delete list runs so a failed
# uninstall always retains a working retry tool.
foreach(_rc_marker IN ITEMS
        "@CPACK_NSIS_DELETE_FILES@"
        "@CPACK_NSIS_DELETE_DIRECTORIES@")
    string(FIND "${_rc_nsis_template_contents}" "${_rc_marker}" _rc_marker_position)
    if(_rc_marker_position EQUAL -1)
        message(FATAL_ERROR
            "CPack's NSIS template is missing the uninstall marker ${_rc_marker}; "
            "the safe residual-payload guard cannot be installed.")
    endif()
endforeach()

set(_rc_delete_files_with_helper_stage [=[
  ; RemoteCam keeps its cleanup helper available until every critical file is gone.
  ClearErrors
  Rename "$INSTDIR\rc-vcam-register.exe" "$INSTDIR\rc-vcam-register.cleanup.exe"
  IfErrors rcUninstallHelperStageFailed 0
  Goto rcUninstallHelperStaged
  rcUninstallHelperStageFailed:
    MessageBox MB_ICONSTOP|MB_OK "RemoteCam could not stage its cleanup helper. The uninstaller and retry metadata were retained; retry uninstall after closing programs that may be scanning the install directory."
    SetErrorLevel 5
    Abort
  rcUninstallHelperStaged:
@CPACK_NSIS_DELETE_FILES@
]=])
string(REPLACE "@CPACK_NSIS_DELETE_FILES@"
               "${_rc_delete_files_with_helper_stage}"
               _rc_guarded_nsis_template
               "${_rc_nsis_template_contents}")

set(_rc_delete_directories_with_residual_guard [=[
@CPACK_NSIS_DELETE_DIRECTORIES@

  ; Do not discard the retry path when generated Delete commands were ignored.
  IfFileExists "$INSTDIR\RemoteCam.exe" rcUninstallPayloadResidual 0
  IfFileExists "$INSTDIR\rc-vcam.dll" rcUninstallPayloadResidual 0
  ClearErrors
  Delete "$INSTDIR\rc-vcam-register.cleanup.exe"
  IfErrors rcUninstallHelperDeleteFailed 0
  Goto rcUninstallPayloadRemoved

  rcUninstallPayloadResidual:
    ClearErrors
    Rename "$INSTDIR\rc-vcam-register.cleanup.exe" "$INSTDIR\rc-vcam-register.exe"
    IfErrors rcUninstallHelperRestoreFailed 0
    IfFileExists "$INSTDIR\rc-vcam-register.exe" 0 rcUninstallHelperRestoreFailed
    MessageBox MB_ICONSTOP|MB_OK "RemoteCam removed its camera and firewall registrations, but critical application files remain in $INSTDIR. The cleanup helper, uninstaller, shortcuts, and retry metadata were retained; close RemoteCam and every camera consumer, then retry uninstall."
    SetErrorLevel 32
    Abort

  rcUninstallHelperDeleteFailed:
    ClearErrors
    Rename "$INSTDIR\rc-vcam-register.cleanup.exe" "$INSTDIR\rc-vcam-register.exe"
    IfErrors rcUninstallHelperRestoreFailed 0
    IfFileExists "$INSTDIR\rc-vcam-register.exe" 0 rcUninstallHelperRestoreFailed
    MessageBox MB_ICONSTOP|MB_OK "RemoteCam removed its camera, firewall registrations, and main application files, but could not remove the cleanup helper. The uninstaller and retry metadata were retained; retry uninstall after closing programs that may be scanning the install directory."
    SetErrorLevel 5
    Abort

  rcUninstallHelperRestoreFailed:
    MessageBox MB_ICONSTOP|MB_OK "RemoteCam could not restore its cleanup helper after an incomplete uninstall. The uninstaller and retry metadata were retained, and the helper backup remains at $INSTDIR\rc-vcam-register.cleanup.exe. Rerun setup to repair the installation before retrying uninstall."
    SetErrorLevel 5
    Abort

  rcUninstallPayloadRemoved:
]=])
string(REPLACE "@CPACK_NSIS_DELETE_DIRECTORIES@"
               "${_rc_delete_directories_with_residual_guard}"
               _rc_guarded_nsis_template
               "${_rc_guarded_nsis_template}")

set(_rc_cpack_template_dir "${CMAKE_CURRENT_BINARY_DIR}/RemoteCamCPackTemplates")
file(MAKE_DIRECTORY "${_rc_cpack_template_dir}")
file(WRITE "${_rc_cpack_template_dir}/NSIS.template.in"
           "${_rc_guarded_nsis_template}")
set(_rc_cpack_project_config
    "${CMAKE_CURRENT_BINARY_DIR}/RemoteCamCPackProjectConfig.cmake")
file(WRITE "${_rc_cpack_project_config}"
    "if(CPACK_GENERATOR STREQUAL \"NSIS\")\n"
    "  list(PREPEND CMAKE_MODULE_PATH [==[${_rc_cpack_template_dir}]==])\n"
    "endif()\n")
set(CPACK_PROJECT_CONFIG_FILE "${_rc_cpack_project_config}")

# x64 only -- windows/CMakeLists.txt fails to configure otherwise, because the Frame
# Server cannot load a 32-bit in-proc server. The CPack default is 32-bit Program Files.
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")

# Shortcuts are written by hand because the package is flat. The rollback function is
# declared at script scope and used by both strict post-install checks below.
set(CPACK_NSIS_DEFINES
"VIProductVersion \\\"${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0\\\"
VIAddVersionKey /LANG=1033 \\\"ProductName\\\" \\\"RemoteCam\\\"
VIAddVersionKey /LANG=1033 \\\"CompanyName\\\" \\\"RemoteCam contributors\\\"
VIAddVersionKey /LANG=1033 \\\"FileDescription\\\" \\\"RemoteCam installer\\\"
VIAddVersionKey /LANG=1033 \\\"FileVersion\\\" \\\"${PROJECT_VERSION}.0\\\"
VIAddVersionKey /LANG=1033 \\\"ProductVersion\\\" \\\"${PROJECT_VERSION}\\\"
VIAddVersionKey /LANG=1033 \\\"LegalCopyright\\\" \\\"Copyright (C) RemoteCam contributors\\\"

Function rcRollbackInstall
    DetailPrint 'Rolling back the incomplete RemoteCam installation'
    IfFileExists '$INSTDIR/Uninstall.exe' 0 rcRollbackFallback
    ClearErrors
    ExecWait '\\\"$INSTDIR/Uninstall.exe\\\" /S _?=$INSTDIR' $R8
    IfErrors rcRollbackFallback 0
    StrCmp $R8 0 rcRollbackVerifyPayload rcRollbackFallback

    rcRollbackVerifyPayload:
      ; NSIS's generated uninstaller does not turn failed Delete/RMDir commands into
      ; a nonzero exit code. Treat critical payload still being present as failure.
      IfFileExists '$INSTDIR/RemoteCam.exe' rcRollbackUnexpectedResidual 0
      IfFileExists '$INSTDIR/rc-vcam.dll' rcRollbackUnexpectedResidual 0
      IfFileExists '$INSTDIR/rc-vcam-register.exe' rcRollbackUnexpectedResidual 0
      Goto rcRollbackDone

    rcRollbackUnexpectedResidual:
      MessageBox MB_ICONEXCLAMATION|MB_OK 'RemoteCam cleanup reported success, but critical application files remain in $INSTDIR. The selected directory and in-place Uninstall.exe were retained as the retry path and were not recursively deleted.'
      Return

    rcRollbackFallback:
      IfFileExists '$INSTDIR/rc-vcam-register.exe' 0 rcRollbackPreserve
      ClearErrors
      ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --unregister' $R8
      IfErrors rcRollbackPreserve 0
      StrCmp $R8 0 rcRollbackCameraGone rcRollbackPreserve
    rcRollbackCameraGone:
      ClearErrors
      ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --firewall-remove' $R8
      IfErrors rcRollbackPreserve 0
      StrCmp $R8 0 rcRollbackResidual rcRollbackPreserve
    rcRollbackResidual:
      MessageBox MB_ICONEXCLAMATION|MB_OK 'RemoteCam removed the virtual camera and firewall rule, but the generated uninstaller failed. The remaining directory and in-place Uninstall.exe were retained as the retry path; the selected directory was not recursively deleted.'
      Return
    rcRollbackPreserve:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam setup could not finish removing the virtual camera or firewall rule. The cleanup tools have been retained in the install directory so setup or uninstall can be retried.'
      Return
    rcRollbackDone:
      ; _?= makes the uninstaller run in place so ExecWait can observe its result.
      ; NSIS therefore leaves that executable for the caller to remove afterward.
      ClearErrors
      Delete '$INSTDIR/Uninstall.exe'
      IfErrors rcRollbackUninstallerResidual 0
      RMDir '$INSTDIR'
      Return
    rcRollbackUninstallerResidual:
      MessageBox MB_ICONEXCLAMATION|MB_OK 'RemoteCam cleanup succeeded, but its in-place uninstaller could not be removed from $INSTDIR. The selected directory was retained and was not recursively deleted.'
      Return
FunctionEnd")
set(CPACK_NSIS_CREATE_ICONS_EXTRA
    "  CreateShortCut \\\"$SMPROGRAMS/$STARTMENU_FOLDER/RemoteCam.lnk\\\" \\\"$INSTDIR/RemoteCam.exe\\\"")
set(CPACK_NSIS_DELETE_ICONS_EXTRA
    "  Delete \\\"$SMPROGRAMS/$MUI_TEMP/RemoteCam.lnk\\\"")

# Reject unsupported machines before any files, registry keys, shortcuts, or
# previous-version removal. The NSIS stub is 32-bit, so PROCESSOR_ARCHITEW6432 is the
# authoritative native architecture on a 64-bit OS. MFCreateVirtualCamera requires
# Windows 11 build 22000 or newer. CPack normally stores uninstall metadata in the
# 32-bit registry view; the 64-bit fallback recovers older development installs.
set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS [=[
    ReadEnvStr $0 'PROCESSOR_ARCHITEW6432'
    StrCmp $0 'AMD64' rcArchitectureOk 0
    ReadEnvStr $0 'PROCESSOR_ARCHITECTURE'
    StrCmp $0 'AMD64' rcArchitectureOk 0
      SetOutPath '$TEMP'
      RMDir '$INSTDIR'
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam requires an x64 Windows PC. This installer cannot run on x86 or ARM64 Windows.'
      SetErrorLevel 1633
      Abort
    rcArchitectureOk:

    SetRegView 64
    ReadRegStr $0 HKLM 'SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion' 'CurrentBuildNumber'
    IntCmp $0 22000 rcWindowsBuildOk rcWindowsBuildTooOld rcWindowsBuildOk
    rcWindowsBuildTooOld:
      SetOutPath '$TEMP'
      RMDir '$INSTDIR'
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam requires Windows 11 build 22000 or newer.'
      SetErrorLevel 1150
      Abort
    rcWindowsBuildOk:

    SetRegView 32
    ReadRegStr $1 HKLM 'Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RemoteCam' 'UninstallString'
    StrCmp $1 '' 0 rcPreviousInstallFound
    SetRegView 64
    ReadRegStr $1 HKLM 'Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RemoteCam' 'UninstallString'
    SetRegView 32
    StrCmp $1 '' rcNoPreviousInstall 0

    rcPreviousInstallFound:
      MessageBox MB_YESNO|MB_ICONEXCLAMATION 'RemoteCam is already installed. The previous version must be removed before setup can continue.' /SD IDYES IDYES rcRemovePreviousInstall
      SetErrorLevel 1223
      Abort

    rcRemovePreviousInstall:
      ; CPack stores this value with one quote at each end. Strip exactly those
      ; characters, validate the expected executable tail, then derive its parent.
      StrCpy $4 $1
      StrCpy $1 $1 -1 1
      StrLen $2 'Uninstall.exe'
      StrCpy $0 $1 $2 -$2
      StrCmp $0 'Uninstall.exe' 0 rcPreviousUninstallMetadataInvalid
      IntOp $2 $2 + 1
      StrCpy $3 $1 -$2
      StrCmp $3 '' rcPreviousUninstallMetadataInvalid 0
      IfFileExists '$1' rcPreviousUninstallMetadataValid 0
    rcPreviousUninstallMetadataInvalid:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam setup found invalid previous-version uninstall metadata. The existing installation was left untouched.'
      SetErrorLevel 13
      Abort
    rcPreviousUninstallMetadataValid:
      ClearErrors
      ExecWait '$4 /S _?=$3' $2
      IfErrors rcPreviousUninstallLaunchFailed 0
      StrCmp $2 0 rcPreviousUninstallComplete rcPreviousUninstallFailed
    rcPreviousUninstallLaunchFailed:
      StrCpy $2 1
    rcPreviousUninstallFailed:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam setup could not remove the previous version (exit code $2). The new version was not installed.'
      SetErrorLevel $2
      Abort
    rcPreviousUninstallComplete:
      ; Older generated uninstallers can report success after a locked Delete failed.
      ; Keep their uninstaller/metadata intact when any critical payload remains.
      IfFileExists '$3/RemoteCam.exe' rcPreviousUninstallResidual 0
      IfFileExists '$3/rc-vcam.dll' rcPreviousUninstallResidual 0
      IfFileExists '$3/rc-vcam-register.exe' rcPreviousUninstallResidual 0
      Goto rcPreviousUninstallPayloadGone
    rcPreviousUninstallResidual:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam setup could not fully remove the previous version because critical files remain in $3. The remaining directory and in-place Uninstall.exe were retained as the retry path; close RemoteCam and every camera consumer, then retry setup.'
      SetErrorLevel 32
      Abort
    rcPreviousUninstallPayloadGone:
      ; _?= leaves the in-place uninstaller for this parent process to remove.
      ClearErrors
      Delete '$3/Uninstall.exe'
      RMDir '$3'
      IfErrors rcPreviousUninstallCleanupFailed rcNoPreviousInstall
    rcPreviousUninstallCleanupFailed:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam setup could not finish cleaning the previous installation directory. The new version was not installed.'
      SetErrorLevel 5
      Abort
    rcNoPreviousInstall:
      ; A successful old-version uninstall removes $INSTDIR after CPack's earlier
      ; SetOutPath. Recreate and select it before CPACK_NSIS_FULL_INSTALL extracts.
      SetOutPath '$INSTDIR'
]=])

# MFCreateVirtualCamera and the HKLM CLSID write need admin. CPack runs these strings
# through configure_file, so the commands intentionally avoid LogicLib expressions.
# Any registration or firewall failure invokes the already-written uninstaller to
# remove files, registry state, shortcuts, the firewall rule, and a partially
# registered camera before returning a failing code.
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    DetailPrint 'Registering the RemoteCam virtual camera'
    ClearErrors
    ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --register' $0
    IfErrors rcRegisterLaunchFailed 0
    StrCmp $0 0 rcRegisterDone rcRegisterFailed
    rcRegisterLaunchFailed:
      StrCpy $0 1
    rcRegisterFailed:
      StrCpy $R9 $0
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam setup could not register the virtual camera (exit code $R9). Setup will attempt a complete rollback.'
      Call rcRollbackInstall
      SetErrorLevel $R9
      Abort
    rcRegisterDone:

    DetailPrint 'Allowing inbound RemoteCam connections on TCP port 7890'
    ClearErrors
    ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --firewall-add --app \\\"$INSTDIR/RemoteCam.exe\\\"' $0
    IfErrors rcFirewallLaunchFailed 0
    StrCmp $0 0 rcFirewallDone rcFirewallFailed
    rcFirewallLaunchFailed:
      StrCpy $0 1
    rcFirewallFailed:
      StrCpy $R9 $0
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam setup could not create its private-network firewall rule (exit code $R9). Setup will attempt a complete rollback.'
      Call rcRollbackInstall
      SetErrorLevel $R9
      Abort
    rcFirewallDone:
")

# Runs before CPack's file-deletion block, so the unregister tool still exists. The
# NSIS template's RequestExecutionLevel admin applies to this uninstaller as well.
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    DetailPrint 'Checking that RemoteCam files are not in use'
    ClearErrors
    ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --uninstall-preflight' $2
    IfErrors rcUninstallPreflightLaunchFailed 0
    StrCmp $2 0 rcUninstallPreflightDone rcUninstallPreflightFailed
    rcUninstallPreflightLaunchFailed:
      StrCpy $2 1
    rcUninstallPreflightFailed:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam uninstall cannot continue because the application or virtual-camera DLL is in use (exit code $2). Close RemoteCam and every camera consumer, then retry. No camera, firewall, application files, or installer metadata were changed.'
      SetErrorLevel $2
      Abort
    rcUninstallPreflightDone:
    ClearErrors
    ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --unregister' $1
    IfErrors rcUnregisterLaunchFailed 0
    StrCmp $1 0 rcUnregisterDone rcUnregisterFailed
    rcUnregisterLaunchFailed:
      StrCpy $1 1
    rcUnregisterFailed:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam uninstall could not remove the virtual camera (exit code $1). No application files were deleted; close camera consumers and retry.'
      SetErrorLevel $1
      Abort
    rcUnregisterDone:
    ClearErrors
    ExecWait '\\\"$INSTDIR/rc-vcam-register.exe\\\" --firewall-remove' $0
    IfErrors rcFirewallRemovalLaunchFailed 0
    StrCmp $0 0 rcFirewallRemovalDone rcFirewallRemovalFailed
    rcFirewallRemovalLaunchFailed:
      StrCpy $0 1
    rcFirewallRemovalFailed:
      MessageBox MB_ICONSTOP|MB_OK 'RemoteCam uninstall could not remove its firewall rule (exit code $0). No application files were deleted; fix Windows Firewall and retry.'
      SetErrorLevel $0
      Abort
    rcFirewallRemovalDone:
    ExpandEnvStrings $R0 '%ProgramData%'
    StrCmp $R0 '%ProgramData%' rcLogsDone 0
      RMDir /r '$R0/RemoteCam'
    rcLogsDone:
")

include(CPack)
