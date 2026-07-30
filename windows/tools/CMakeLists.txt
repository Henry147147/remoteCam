# Development and verification tools. Not shipped in the installer.

set(RC_TOOL_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")

# rc-vcam-probe: the automated half of M1's verification. Opens the virtual camera
# through Media Foundation AND DirectShow, because one implementation is supposed to
# serve both and that claim is worth testing rather than assuming.
add_executable(rc-vcam-probe vcam_probe/main.cpp)
target_link_libraries(rc-vcam-probe PRIVATE
    rcwin-common
    mf
    mfplat
    mfuuid
    mfreadwrite
    strmiids    # DirectShow CLSIDs and IIDs
    ole32
    oleaut32
)
rc_target_strict_warnings(rc-vcam-probe)

# rc-fakewriter: stands in for the capture pipeline so the Session 0 handoff can be
# proven with nothing else built.
add_executable(rc-fakewriter fakewriter/main.cpp)
target_link_libraries(rc-fakewriter PRIVATE rcwin-common)
rc_target_strict_warnings(rc-fakewriter)

# rc-fakepc: deliberately insecure integration harness for a Debug iOS build. It is
# never installed and refuses to send `ready` unless the bypass flag is explicit.
add_executable(rc-fakepc fakepc/main.cpp)
target_link_libraries(rc-fakepc PRIVATE rcwin-net)
target_compile_definitions(rc-fakepc PRIVATE
    $<$<CONFIG:Debug>:RC_FAKEPC_INSECURE_ENABLED=1>
)
rc_target_strict_warnings(rc-fakepc)

# rc-fakephone: external TCP peer that behaves like the shipping iOS client. The
# library split lets CTest drive the same state machine in-process over real loopback
# sockets, while the CLI drives a running app/backend from another process.
add_library(rcfakephone STATIC
    fakephone/src/phone_model.cpp
    fakephone/src/replay_media.cpp
    fakephone/src/reporter.cpp
    fakephone/src/scenario_engine.cpp
    fakephone/src/synthetic_media.cpp
)
target_include_directories(rcfakephone PUBLIC fakephone/include)
target_link_libraries(rcfakephone PUBLIC rcwin-net)
target_compile_features(rcfakephone PUBLIC cxx_std_20)
rc_target_strict_warnings(rcfakephone)

add_executable(rc-fakephone fakephone/main.cpp)
target_link_libraries(rc-fakephone PRIVATE rcfakephone)
rc_target_strict_warnings(rc-fakephone)

if(RC_BUILD_TESTS)
    add_executable(rc-fakephone-tests fakephone/tests/fakephone_test.cpp)
    target_link_libraries(rc-fakephone-tests PRIVATE rcfakephone)
    rc_target_strict_warnings(rc-fakephone-tests)
    add_test(NAME rc-fakephone-tests COMMAND rc-fakephone-tests)
endif()

foreach(target rc-vcam-probe rc-fakewriter rc-fakepc rc-fakephone)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         "${RC_TOOL_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${RC_TOOL_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${RC_TOOL_OUTPUT_DIR}"
    )
endforeach()
