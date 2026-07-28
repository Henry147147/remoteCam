# rcwin-common: Windows-only helpers shared by the DLL, the tools and (later) the app.
#
# This is the counterpart to core/: everything here is allowed to touch Win32, COM and
# Media Foundation, which is exactly why none of it belongs in core/. The split is not
# cosmetic -- core/ has to build on a Linux CI runner with no GPU.

add_library(rcwin-common STATIC
    src/hr.cpp
    src/nv12.cpp
    src/shm_ring.cpp
    src/test_pattern.cpp
)

target_include_directories(rcwin-common PUBLIC include)

target_compile_features(rcwin-common PUBLIC cxx_std_20)

target_link_libraries(rcwin-common PUBLIC
    shell32     # SHGetKnownFolderPath, for the log directory
    advapi32    # registry and the SDDL security descriptor for the Global\ section
    ole32       # CoTaskMemFree
)

target_compile_definitions(rcwin-common PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX                # else <windows.h> min/max macros break std::min/std::max
    WIN32_LEAN_AND_MEAN
)

target_compile_options(rcwin-common PRIVATE /W4 /permissive-)

# Same testing convention as core/: no GTest, no Catch, just a main() that returns
# non-zero. The Windows half needs to stay as easy to run as the portable half.
if(RC_BUILD_TESTS)
    add_executable(rcwin-common-tests tests/rcwin_test.cpp)
    target_link_libraries(rcwin-common-tests PRIVATE rcwin-common)
    target_compile_options(rcwin-common-tests PRIVATE /W4 /permissive-)
    add_test(NAME rcwin-common-tests COMMAND rcwin-common-tests)
endif()
