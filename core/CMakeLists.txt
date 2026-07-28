# rc-core: the portable half of RemoteCam.
#
# Nothing in this target may touch D3D11, Media Foundation, Qt or any Windows API.
# That constraint is what lets the transform math, the wire protocol and the pairing
# handshake be built and tested on any host, including CI runners without a GPU.
# Windows-only code lives in windows/.

add_library(rc-core STATIC
    src/transform.cpp
)

target_include_directories(rc-core PUBLIC include)

target_compile_features(rc-core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(rc-core PRIVATE /W4 /permissive-)
else()
    target_compile_options(rc-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(RC_BUILD_TESTS)
    add_executable(rc-core-tests tests/transform_test.cpp)
    target_link_libraries(rc-core-tests PRIVATE rc-core)
    target_compile_features(rc-core-tests PRIVATE cxx_std_20)
    add_test(NAME rc-core-tests COMMAND rc-core-tests)
endif()
