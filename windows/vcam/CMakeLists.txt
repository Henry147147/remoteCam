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
    rcwin-common-mt
    mf
    mfplat
    mfuuid      # defines the MF_DEVICESTREAM_* attribute GUIDs
    ole32
    oleaut32
)

rc_target_strict_warnings(rc-vcam)

# The Frame Server looks up the file by the absolute path recorded in InprocServer32,
# so the output name has to match what rc-vcam-register.exe expects to find.
#
# Static CRT: see windows/common/CMakeLists.txt. A missing msvcp140.dll here is a
# camera that registers and enumerates and then never delivers a frame, diagnosable
# only from rc-vcam.log -- so the dependency does not get to exist.
set_target_properties(rc-vcam PROPERTIES
    OUTPUT_NAME "rc-vcam"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

# Flat install root: rc-vcam-register.exe resolves rc-vcam.dll next to itself, and the
# path it writes into InprocServer32 is absolute, so the two must land together.
# Naming only RUNTIME keeps the import library out of the package.
install(TARGETS rc-vcam RUNTIME DESTINATION ".")
