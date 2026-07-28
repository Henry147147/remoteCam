# rc-vcam-register.exe -- one-shot registration, run by the installer with admin rights.
#
# The manifest stays asInvoker rather than requireAdministrator on purpose: --status
# must work for an ordinary user so it is usable as a diagnostic, and the tool checks
# elevation itself for the two verbs that need it, printing the exact command to run.

add_executable(rc-vcam-register main.cpp)

target_link_libraries(rc-vcam-register PRIVATE
    rcwin-common
    mf
    mfplat
    mfuuid
    mfsensorgroup   # MFCreateVirtualCamera
    ole32
    shell32
    advapi32
)

target_compile_options(rc-vcam-register PRIVATE /W4 /permissive-)

# rc-vcam-register looks for rc-vcam.dll next to itself, so keep both in one directory.
set_target_properties(rc-vcam-register PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
