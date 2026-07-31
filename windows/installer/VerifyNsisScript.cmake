if(NOT DEFINED NSIS_SCRIPT OR NSIS_SCRIPT STREQUAL "")
    message(FATAL_ERROR "Pass -DNSIS_SCRIPT=<absolute path to generated project.nsi>")
endif()
cmake_path(ABSOLUTE_PATH NSIS_SCRIPT NORMALIZE OUTPUT_VARIABLE _rc_nsis_script)
if(NOT EXISTS "${_rc_nsis_script}")
    message(FATAL_ERROR "Generated NSIS script does not exist: ${_rc_nsis_script}")
endif()

file(READ "${_rc_nsis_script}" _rc_nsis)

string(REGEX MATCHALL "RequestExecutionLevel[ \t]+admin" _rc_admin_requests "${_rc_nsis}")
list(LENGTH _rc_admin_requests _rc_admin_request_count)
if(NOT _rc_admin_request_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one RequestExecutionLevel admin declaration; found "
        "${_rc_admin_request_count} in ${_rc_nsis_script}")
endif()
if(_rc_nsis MATCHES "RequestExecutionLevel[ \t]+(user|highest|none)")
    message(FATAL_ERROR "Generated installer contains a weaker elevation declaration")
endif()

set(_rc_required_fragments
    "PROCESSOR_ARCHITEW6432"
    "CurrentBuildNumber"
    "22000"
    "SetRegView 64"
    "SetRegView 32"
    "StrCmp \"OFF\" \"ON\" 0 inst"
    "rcPreviousInstallFound"
    "rcRemovePreviousInstall"
    "rcPreviousUninstallMetadataInvalid"
    "ExecWait '$4 /S _?=$3' $2"
    "rcPreviousUninstallFailed"
    "rcPreviousUninstallCleanupFailed"
    "rcNoPreviousInstall:"
    "Function rcRollbackInstall"
    "rcRollbackVerifyPayload"
    "rcRollbackUnexpectedResidual"
    "rcRollbackCameraGone"
    "rcRollbackResidual"
    "rcRollbackUninstallerResidual"
    "Call rcRollbackInstall"
    "Uninstall.exe"
    "Delete '$INSTDIR/Uninstall.exe'"
    "RMDir '$INSTDIR'"
    "retry metadata were retained"
    "rcPreviousUninstallResidual"
    "--register"
    "--unregister"
    "--uninstall-preflight"
    "--firewall-add --app"
    "--firewall-remove"
    "rcUninstallPreflightFailed"
    "rcUninstallPreflightDone"
    "rcUninstallHelperStageFailed"
    "rc-vcam-register.cleanup.exe"
    "rcUninstallPayloadResidual"
    "rcUninstallHelperDeleteFailed"
    "rcUninstallHelperRestoreFailed"
    "rcUninstallPayloadRemoved"
    "rcUnregisterFailed"
    "rcFirewallRemovalFailed"
    "SetErrorLevel $R9"
    "Abort")
foreach(_rc_fragment IN LISTS _rc_required_fragments)
    string(FIND "${_rc_nsis}" "${_rc_fragment}" _rc_fragment_position)
    if(_rc_fragment_position EQUAL -1)
        message(FATAL_ERROR
            "Generated installer is missing required transaction fragment: ${_rc_fragment}")
    endif()
endforeach()

if(_rc_nsis MATCHES "RMDir[ \t]+/r[ \t]+['\\\"]?\\$INSTDIR")
    message(FATAL_ERROR
        "Generated installer can recursively delete a user-selected install directory")
endif()

foreach(_rc_residual_scope IN ITEMS
        "rcRollbackUnexpectedResidual:|rcRollbackFallback:"
        "rcRollbackResidual:|rcRollbackPreserve:")
    string(REPLACE "|" ";" _rc_residual_labels "${_rc_residual_scope}")
    list(GET _rc_residual_labels 0 _rc_residual_start_label)
    list(GET _rc_residual_labels 1 _rc_residual_end_label)
    string(FIND "${_rc_nsis}" "${_rc_residual_start_label}"
           _rc_residual_start_position)
    string(FIND "${_rc_nsis}" "${_rc_residual_end_label}"
           _rc_residual_end_position)
    if(_rc_residual_start_position EQUAL -1 OR
       _rc_residual_end_position LESS _rc_residual_start_position)
        message(FATAL_ERROR
            "Cannot locate rollback residual scope ${_rc_residual_start_label}")
    endif()
    math(EXPR _rc_residual_length
         "${_rc_residual_end_position} - ${_rc_residual_start_position}")
    string(SUBSTRING "${_rc_nsis}" ${_rc_residual_start_position}
           ${_rc_residual_length} _rc_residual_body)
    if(_rc_residual_body MATCHES
       "(DeleteRegKey|Delete[ \t]+['\\\"]\\$SMPROGRAMS)")
        message(FATAL_ERROR
            "Rollback residual branch discards its uninstaller retry metadata")
    endif()
endforeach()

if(_rc_nsis MATCHES "(netsh\\.exe|advfirewall)")
    message(FATAL_ERROR
        "Generated installer bypasses rc-vcam-register's checked native firewall path")
endif()

if(_rc_nsis MATCHES "MUI_FINISHPAGE_RUN")
    message(FATAL_ERROR
        "The elevated installer must not offer to launch RemoteCam with its administrator token")
endif()

string(REGEX MATCHALL "Call[ \t]+rcRollbackInstall" _rc_rollback_calls "${_rc_nsis}")
list(LENGTH _rc_rollback_calls _rc_rollback_call_count)
if(NOT _rc_rollback_call_count EQUAL 2)
    message(FATAL_ERROR
        "Expected registration and firewall-add failures to invoke rollback; found "
        "${_rc_rollback_call_count} calls")
endif()

string(REGEX MATCHALL "SetErrorLevel[ \t]+\\$R9" _rc_preserved_exit_codes "${_rc_nsis}")
list(LENGTH _rc_preserved_exit_codes _rc_preserved_exit_code_count)
if(NOT _rc_preserved_exit_code_count EQUAL 2)
    message(FATAL_ERROR
        "Expected both transactional failures to preserve their exit code; found "
        "${_rc_preserved_exit_code_count} SetErrorLevel calls")
endif()

string(FIND "${_rc_nsis}" "_?=$INSTDIR" _rc_in_place_uninstall_position)
string(FIND "${_rc_nsis}" "Delete '$INSTDIR/Uninstall.exe'" _rc_uninstaller_delete_position)
string(FIND "${_rc_nsis}" "rcRollbackVerifyPayload:"
       _rc_rollback_payload_verify_position)
string(FIND "${_rc_nsis}" "rcRollbackDone:" _rc_rollback_done_position)
if(_rc_in_place_uninstall_position EQUAL -1 OR
   _rc_uninstaller_delete_position LESS _rc_in_place_uninstall_position)
    message(FATAL_ERROR
        "Rollback must remove the in-place uninstaller after waiting for it")
endif()
if(_rc_rollback_payload_verify_position LESS _rc_in_place_uninstall_position OR
   _rc_rollback_done_position LESS _rc_rollback_payload_verify_position)
    message(FATAL_ERROR
        "Rollback must verify critical payload after its generated uninstaller returns")
endif()
foreach(_rc_rollback_critical_file IN ITEMS
        "RemoteCam.exe" "rc-vcam.dll" "rc-vcam-register.exe")
    string(FIND "${_rc_nsis}"
           "IfFileExists '$INSTDIR/${_rc_rollback_critical_file}' rcRollbackUnexpectedResidual 0"
           _rc_rollback_critical_check)
    if(_rc_rollback_critical_check LESS _rc_rollback_payload_verify_position OR
       _rc_rollback_critical_check GREATER _rc_rollback_done_position)
        message(FATAL_ERROR
            "Rollback does not verify residual ${_rc_rollback_critical_file}")
    endif()
endforeach()
string(SUBSTRING "${_rc_nsis}" ${_rc_uninstaller_delete_position} -1
       _rc_after_uninstaller_delete)
string(FIND "${_rc_after_uninstaller_delete}" "RMDir '$INSTDIR'"
       _rc_post_uninstall_rmdir_position)
if(_rc_post_uninstall_rmdir_position EQUAL -1)
    message(FATAL_ERROR
        "Rollback must remove the empty installation directory after the uninstaller")
endif()

string(FIND "${_rc_nsis}" "--unregister" _rc_first_unregister_position)
string(FIND "${_rc_nsis}" "--firewall-remove"
       _rc_first_firewall_delete_position)
if(_rc_first_unregister_position EQUAL -1 OR
   _rc_first_firewall_delete_position EQUAL -1 OR
   _rc_first_unregister_position GREATER _rc_first_firewall_delete_position)
    message(FATAL_ERROR
        "Fallback rollback must preserve the firewall until camera cleanup succeeds")
endif()

string(FIND "${_rc_nsis}" "PROCESSOR_ARCHITEW6432" _rc_preflight_position)
string(FIND "${_rc_nsis}" "rcPreviousInstallFound:" _rc_upgrade_position)
string(FIND "${_rc_nsis}" "File /r" _rc_first_file_position)
if(_rc_first_file_position GREATER -1 AND
   _rc_preflight_position GREATER _rc_first_file_position)
    message(FATAL_ERROR "Architecture/OS preflight runs after payload files are written")
endif()
if(_rc_upgrade_position EQUAL -1 OR
   _rc_preflight_position GREATER _rc_upgrade_position)
    message(FATAL_ERROR "Previous-version removal can run before the x64/OS preflight")
endif()
if(_rc_first_file_position GREATER -1 AND
   _rc_upgrade_position GREATER _rc_first_file_position)
    message(FATAL_ERROR "Previous-version removal runs after payload files are written")
endif()

string(FIND "${_rc_nsis}" "rcPreviousUninstallComplete:"
       _rc_previous_uninstall_complete_position)
string(FIND "${_rc_nsis}"
       "IfFileExists '$3/RemoteCam.exe' rcPreviousUninstallResidual 0"
       _rc_previous_payload_check_position)
string(FIND "${_rc_nsis}" "Delete '$3/Uninstall.exe'"
       _rc_previous_uninstaller_delete_position)
if(_rc_previous_uninstall_complete_position EQUAL -1 OR
   _rc_previous_payload_check_position LESS _rc_previous_uninstall_complete_position OR
   _rc_previous_uninstaller_delete_position LESS _rc_previous_payload_check_position)
    message(FATAL_ERROR
        "Upgrade must verify old critical payload is gone before deleting its retry uninstaller")
endif()
foreach(_rc_previous_critical_file IN ITEMS
        "RemoteCam.exe" "rc-vcam.dll" "rc-vcam-register.exe")
    string(FIND "${_rc_nsis}"
           "IfFileExists '$3/${_rc_previous_critical_file}' rcPreviousUninstallResidual 0"
           _rc_previous_critical_check)
    if(_rc_previous_critical_check LESS _rc_previous_uninstall_complete_position OR
       _rc_previous_critical_check GREATER _rc_previous_uninstaller_delete_position)
        message(FATAL_ERROR
            "Upgrade does not retain its retry uninstaller when "
            "${_rc_previous_critical_file} remains")
    endif()
endforeach()

string(FIND "${_rc_nsis}" "rcNoPreviousInstall:" _rc_upgrade_done_position)
string(SUBSTRING "${_rc_nsis}" ${_rc_upgrade_done_position} -1 _rc_after_upgrade)
string(FIND "${_rc_after_upgrade}" "SetOutPath '$INSTDIR'" _rc_reset_out_path_position)
string(FIND "${_rc_after_upgrade}" "File /r" _rc_payload_after_upgrade_position)
if(_rc_reset_out_path_position EQUAL -1 OR
   (_rc_payload_after_upgrade_position GREATER -1 AND
    _rc_reset_out_path_position GREATER _rc_payload_after_upgrade_position))
    message(FATAL_ERROR
        "Installer must recreate/select INSTDIR after removing a previous version")
endif()

string(FIND "${_rc_nsis}" "rcUnregisterDone:" _rc_unregister_done_position)
if(_rc_unregister_done_position EQUAL -1)
    message(FATAL_ERROR "Generated uninstaller has no successful camera-cleanup branch")
endif()
string(SUBSTRING "${_rc_nsis}" ${_rc_unregister_done_position} -1
       _rc_after_unregister_done)
string(FIND "${_rc_after_unregister_done}" "--firewall-remove"
       _rc_firewall_cleanup_position)
if(_rc_firewall_cleanup_position EQUAL -1)
    message(FATAL_ERROR
        "Uninstall must retain the firewall rule until virtual-camera cleanup succeeds")
endif()

string(REGEX MATCHALL "--uninstall-preflight" _rc_uninstall_preflight_calls
       "${_rc_nsis}")
list(LENGTH _rc_uninstall_preflight_calls _rc_uninstall_preflight_call_count)
if(NOT _rc_uninstall_preflight_call_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one uninstall lock preflight; found "
        "${_rc_uninstall_preflight_call_count}")
endif()

string(FIND "${_rc_nsis}" "Section \"Uninstall\"" _rc_uninstall_section_position)
if(_rc_uninstall_section_position EQUAL -1)
    message(FATAL_ERROR "Generated NSIS script has no uninstall section")
endif()
string(SUBSTRING "${_rc_nsis}" ${_rc_uninstall_section_position} -1
       _rc_uninstall_section)
string(FIND "${_rc_uninstall_section}" "--uninstall-preflight"
       _rc_uninstall_preflight_position)
string(FIND "${_rc_uninstall_section}" "--unregister"
       _rc_uninstall_unregister_position)
if(_rc_uninstall_preflight_position EQUAL -1 OR
   _rc_uninstall_unregister_position LESS _rc_uninstall_preflight_position)
    message(FATAL_ERROR
        "Uninstall must check locked payload before changing camera/firewall state")
endif()

set(_rc_helper_stage
    [=[Rename "$INSTDIR\rc-vcam-register.exe" "$INSTDIR\rc-vcam-register.cleanup.exe"]=])
set(_rc_generated_helper_delete
    [=[Delete "$INSTDIR\rc-vcam-register.exe"]=])
set(_rc_app_delete [=[Delete "$INSTDIR\RemoteCam.exe"]=])
set(_rc_camera_dll_delete [=[Delete "$INSTDIR\rc-vcam.dll"]=])
set(_rc_post_delete_guard
    [=[IfFileExists "$INSTDIR\RemoteCam.exe" rcUninstallPayloadResidual 0]=])
set(_rc_post_delete_dll_guard
    [=[IfFileExists "$INSTDIR\rc-vcam.dll" rcUninstallPayloadResidual 0]=])
set(_rc_generated_uninstaller_delete [=[Delete "$INSTDIR\Uninstall.exe"]=])
set(_rc_generated_uninstall_key_delete
    [=[DeleteRegKey SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\RemoteCam"]=])
foreach(_rc_position_spec IN ITEMS
        "_rc_helper_stage|_rc_helper_stage_position"
        "_rc_generated_helper_delete|_rc_generated_helper_delete_position"
        "_rc_app_delete|_rc_app_delete_position"
        "_rc_camera_dll_delete|_rc_camera_dll_delete_position"
        "_rc_post_delete_guard|_rc_post_delete_guard_position"
        "_rc_post_delete_dll_guard|_rc_post_delete_dll_guard_position"
        "_rc_generated_uninstaller_delete|_rc_generated_uninstaller_delete_position"
        "_rc_generated_uninstall_key_delete|_rc_generated_uninstall_key_delete_position")
    string(REPLACE "|" ";" _rc_position_parts "${_rc_position_spec}")
    list(GET _rc_position_parts 0 _rc_fragment_variable)
    list(GET _rc_position_parts 1 _rc_position_variable)
    string(FIND "${_rc_uninstall_section}" "${${_rc_fragment_variable}}"
           _rc_found_position)
    set(${_rc_position_variable} ${_rc_found_position})
endforeach()
if(_rc_helper_stage_position EQUAL -1 OR
   _rc_generated_helper_delete_position LESS _rc_helper_stage_position)
    message(FATAL_ERROR
        "Uninstall must stage the cleanup helper before CPack's generated delete list")
endif()
if(_rc_app_delete_position EQUAL -1 OR
   _rc_camera_dll_delete_position EQUAL -1 OR
   _rc_post_delete_guard_position LESS _rc_app_delete_position OR
   _rc_post_delete_guard_position LESS _rc_camera_dll_delete_position OR
   _rc_post_delete_dll_guard_position LESS _rc_camera_dll_delete_position)
    message(FATAL_ERROR
        "Uninstall residual guard must run after generated critical-file deletes")
endif()
if(_rc_generated_uninstaller_delete_position LESS _rc_post_delete_guard_position OR
   _rc_generated_uninstall_key_delete_position LESS _rc_post_delete_guard_position)
    message(FATAL_ERROR
        "Uninstall must retain its executable and Add/Remove Programs metadata "
        "until the post-delete residual guard succeeds")
endif()
string(FIND "${_rc_uninstall_section}" "rcUninstallPayloadResidual:"
       _rc_payload_residual_label_position)
string(FIND "${_rc_uninstall_section}" "rcUninstallHelperDeleteFailed:"
       _rc_payload_residual_end_position)
if(_rc_payload_residual_label_position EQUAL -1 OR
   _rc_payload_residual_end_position LESS _rc_payload_residual_label_position)
    message(FATAL_ERROR "Cannot locate post-delete residual branch")
endif()
math(EXPR _rc_payload_residual_length
     "${_rc_payload_residual_end_position} - ${_rc_payload_residual_label_position}")
string(SUBSTRING "${_rc_uninstall_section}" ${_rc_payload_residual_label_position}
       ${_rc_payload_residual_length} _rc_payload_residual_body)
if(_rc_payload_residual_body MATCHES "DeleteRegKey")
    message(FATAL_ERROR
        "Residual-payload branch must not delete retry metadata")
endif()

foreach(_rc_failure_label IN ITEMS
        "StrCmp $0 'AMD64' rcArchitectureOk 0"
        "rcWindowsBuildTooOld:")
    string(FIND "${_rc_nsis}" "${_rc_failure_label}" _rc_failure_position)
    if(_rc_failure_position EQUAL -1)
        message(FATAL_ERROR "Missing preflight failure branch: ${_rc_failure_label}")
    endif()
    string(SUBSTRING "${_rc_nsis}" ${_rc_failure_position} -1 _rc_after_failure)
    string(FIND "${_rc_after_failure}" "SetOutPath '$TEMP'" _rc_preflight_temp)
    string(FIND "${_rc_after_failure}" "RMDir '$INSTDIR'" _rc_preflight_rmdir)
    string(FIND "${_rc_after_failure}" "Abort" _rc_preflight_abort)
    if(_rc_preflight_temp EQUAL -1 OR _rc_preflight_rmdir EQUAL -1 OR
       _rc_preflight_abort EQUAL -1 OR
       _rc_preflight_temp GREATER _rc_preflight_rmdir OR
       _rc_preflight_rmdir GREATER _rc_preflight_abort)
        message(FATAL_ERROR
            "Preflight must leave and remove CPack's empty INSTDIR before aborting")
    endif()
endforeach()

message(STATUS "Verified elevated, transactional NSIS script: ${_rc_nsis_script}")
