# The Windows frame pipeline. Decision logic is kept independent of a live GPU and is
# exercised by rcwin-platform-tests; the FFmpeg and D3D11 bindings compile here but
# still require the hardware matrix in windows/CLAUDE.md before they can be called
# verified.

add_library(rcwin-platform STATIC
    src/abr_controller.cpp
    src/d3d11_transform.cpp
    src/ffmpeg_d3d11_decoder.cpp
    src/shader_constants.cpp
    src/video_pipeline.cpp
)

target_include_directories(rcwin-platform PUBLIC include)
target_compile_features(rcwin-platform PUBLIC cxx_std_20)
target_link_libraries(rcwin-platform PUBLIC
    rc-core
    rcwin-common
    d3d11
    d3dcompiler
    dxgi
)
rc_target_strict_warnings(rcwin-platform)

if(RC_WITH_FFMPEG)
    find_package(FFMPEG REQUIRED)
    target_compile_definitions(rcwin-platform PRIVATE RC_WITH_FFMPEG=1)
    target_include_directories(rcwin-platform SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(rcwin-platform PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(rcwin-platform PRIVATE ${FFMPEG_LIBRARIES})
endif()

if(RC_BUILD_TESTS)
    add_executable(rcwin-platform-tests tests/platform_test.cpp)
    target_link_libraries(rcwin-platform-tests PRIVATE rcwin-platform)
    rc_target_strict_warnings(rcwin-platform-tests)
    add_test(NAME rcwin-platform-tests COMMAND rcwin-platform-tests)
endif()
