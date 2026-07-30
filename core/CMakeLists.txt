# rc-core: the portable half of RemoteCam.
#
# Nothing in this target may touch D3D11, Media Foundation, Qt or any Windows API.
# That constraint is what lets the transform math, the wire protocol and the pairing
# handshake be built and tested on any host, including CI runners without a GPU.
# Windows-only code lives in windows/.

add_library(rc-core STATIC
    src/annexb.cpp
    src/cbor.cpp
    src/control.cpp
    src/transform.cpp
    src/wire.cpp
)

target_include_directories(rc-core PUBLIC include)

target_compile_features(rc-core PUBLIC cxx_std_20)
rc_target_strict_warnings(rc-core)

if(RC_BUILD_TESTS)
    # One executable per area rather than one omnibus binary: a failure names the layer
    # it is in before anyone reads the output.
    foreach(rc_core_test IN ITEMS transform cbor wire control annexb)
        add_executable(rc-core-${rc_core_test}-tests tests/${rc_core_test}_test.cpp)
        target_link_libraries(rc-core-${rc_core_test}-tests PRIVATE rc-core)
        target_compile_features(rc-core-${rc_core_test}-tests PRIVATE cxx_std_20)
        rc_target_strict_warnings(rc-core-${rc_core_test}-tests)
        add_test(NAME rc-core-${rc_core_test}-tests COMMAND rc-core-${rc_core_test}-tests)
    endforeach()
endif()
