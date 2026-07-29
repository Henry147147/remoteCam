# Qt remains dynamically linked: the project is Apache-2.0 and consumes Qt under
# LGPLv3. The parent only enters this directory after finding the required Qt modules.

qt_standard_project_setup(REQUIRES 6.2)

get_target_property(RC_QT_CORE_TYPE Qt6::Core TYPE)
if(RC_QT_CORE_TYPE STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR
        "RemoteCam must dynamically link Qt 6 to comply with the project's LGPL policy.")
endif()

qt_add_executable(rc-app
    main.cpp
    frame_producer.cpp
    frame_producer.h
)

qt_add_qml_module(rc-app
    URI RemoteCam
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES
        qml/Main.qml
)

target_link_libraries(rc-app PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    rcwin-common
)

target_compile_features(rc-app PRIVATE cxx_std_20)
target_compile_options(rc-app PRIVATE /W4 /permissive-)

set_target_properties(rc-app PROPERTIES
    OUTPUT_NAME RemoteCam
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)
