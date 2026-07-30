# rcwin-common: Windows-only helpers shared by the DLL, the tools and the app.
#
# This is the counterpart to core/: everything here is allowed to touch Win32, COM and
# Media Foundation, which is exactly why none of it belongs in core/. The split is not
# cosmetic -- core/ has to build on a Linux CI runner with no GPU.
#
# Built twice, once against each C runtime. rc-vcam.dll is loaded by svchost.exe in
# Session 0, where nothing guarantees the VC runtime redistributable is installed and
# the DLL's own directory is not dependably on the import search path -- so it links
# the static CRT and carries no runtime dependency at all. Qt ships built against the
# dynamic CRT and rc-app must match it, and MSVC refuses to link the two together
# (LNK2038), so one library with one setting cannot serve both. Nothing crosses
# between the variants at runtime: the two CRTs meet only through the shared-memory
# ring, which is plain bytes.

set(RCWIN_COMMON_SOURCES
    src/hr.cpp
    src/nv12.cpp
    src/shm_ring.cpp
    src/test_pattern.cpp
)

foreach(rcwin_variant IN ITEMS rcwin-common rcwin-common-mt)
    add_library(${rcwin_variant} STATIC ${RCWIN_COMMON_SOURCES})

    target_include_directories(${rcwin_variant} PUBLIC include)

    target_compile_features(${rcwin_variant} PUBLIC cxx_std_20)

    target_link_libraries(${rcwin_variant} PUBLIC
        shell32     # SHGetKnownFolderPath, for the log directory
        advapi32    # registry and the SDDL security descriptor for the Global\ section
        ole32       # CoTaskMemFree
        onecore     # CreateFileMapping2 with least-privilege attach rights
    )

    target_compile_definitions(${rcwin_variant} PUBLIC
        UNICODE
        _UNICODE
        NOMINMAX                # else <windows.h> min/max macros break std::min/std::max
        WIN32_LEAN_AND_MEAN
    )

    rc_target_strict_warnings(${rcwin_variant})
endforeach()

set_property(TARGET rcwin-common-mt PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Same testing convention as core/: no GTest, no Catch, just a main() that returns
# non-zero. The Windows half needs to stay as easy to run as the portable half.
if(RC_BUILD_TESTS)
    add_executable(rcwin-common-tests tests/rcwin_test.cpp)
    target_link_libraries(rcwin-common-tests PRIVATE rcwin-common)
    rc_target_strict_warnings(rcwin-common-tests)
    add_test(NAME rcwin-common-tests COMMAND rcwin-common-tests)
endif()
