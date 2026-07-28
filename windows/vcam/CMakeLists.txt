# rc-vcam.dll -- the Media Foundation media source the Frame Server loads.
#
# This is the highest-risk component in the project (PLAN.md §1). It runs inside
# svchost.exe -k Camera as LOCAL SERVICE in Session 0, so it must not assume a console,
# a window station, an interactive user, or any of RemoteCam's other processes being
# alive.

add_library(rc-vcam SHARED
    dllmain.cpp
    frame_source.cpp
    media_source.cpp
    media_stream.cpp
    rc-vcam.def
)

target_link_libraries(rc-vcam PRIVATE
    rcwin-common
    mf
    mfplat
    mfuuid      # defines the MF_DEVICESTREAM_* attribute GUIDs
    ole32
    oleaut32
)

target_compile_options(rc-vcam PRIVATE /W4 /permissive-)

# The Frame Server looks up the file by the absolute path recorded in InprocServer32,
# so the output name has to match what rc-vcam-register.exe expects to find.
set_target_properties(rc-vcam PROPERTIES
    OUTPUT_NAME "rc-vcam"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
