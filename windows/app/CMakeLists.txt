# Qt remains dynamically linked: the project is Apache-2.0 and consumes Qt under
# LGPLv3. The parent only enters this directory after finding the required Qt modules.

qt_standard_project_setup(REQUIRES 6.2)
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

get_target_property(RC_QT_CORE_TYPE Qt6::Core TYPE)
if(RC_QT_CORE_TYPE STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR
        "RemoteCam must dynamically link Qt 6 to comply with the project's LGPL policy.")
endif()

qt_add_executable(rc-app
    bonjour_advertiser.cpp
    bonjour_advertiser.h
    main.cpp
    frame_producer.cpp
    frame_producer.h
    session_status.cpp
    session_status.h
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
    rcwin-backend
    rcwin-common
    dnsapi
)

target_compile_features(rc-app PRIVATE cxx_std_20)
rc_target_strict_warnings(rc-app)

set_target_properties(rc-app PROPERTIES
    OUTPUT_NAME RemoteCam
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS rc-app RUNTIME DESTINATION ".")

# Same Qt UI and same rcwin-backend, with an insecure trust policy compiled only into
# this executable. It is a build-tree test host and is intentionally never installed.
if(RC_BUILD_TESTS)
    qt_add_executable(rc-e2e
        bonjour_advertiser.cpp
        bonjour_advertiser.h
        main.cpp
        frame_producer.cpp
        frame_producer.h
        session_status.cpp
        session_status.h
    )
    qt_add_resources(rc-e2e e2e_qml
        PREFIX "/"
        FILES qml/Main.qml
    )
    target_compile_definitions(rc-e2e PRIVATE RC_APP_E2E_HOST=1)
    target_link_libraries(rc-e2e PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
        rcwin-backend
        rcwin-common
        dnsapi
    )
    target_compile_features(rc-e2e PRIVATE cxx_std_20)
    rc_target_strict_warnings(rc-e2e)
    set_target_properties(rc-e2e PROPERTIES
        OUTPUT_NAME RemoteCam-E2E
        WIN32_EXECUTABLE TRUE
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# Qt's runtime is only staged when a package is actually being built: windeployqt runs
# on every `cmake --install` otherwise, which is slow and pointless for a dev tree.
if(RC_BUILD_INSTALLER)
    if(Qt6_VERSION VERSION_LESS 6.5)
        message(FATAL_ERROR
            "RC_BUILD_INSTALLER needs Qt 6.5 or newer for qt_generate_deploy_qml_app_script; "
            "found ${Qt6_VERSION}. Ordinary builds still work with 6.2.")
    endif()

    # The deploy script reads these at install time and defaults them to bin/ and lib/.
    # The package is flat, so say so before the script is generated.
    set(CMAKE_INSTALL_BINDIR ".")
    set(CMAKE_INSTALL_LIBDIR ".")

    qt_generate_deploy_qml_app_script(
        TARGET rc-app
        OUTPUT_SCRIPT rc_app_deploy_script
        NO_TRANSLATIONS
        # Qt Quick uses the D3D11 RHI here and all shipped shaders are precompiled.
        # Avoid bundling unused Mesa/DXC fallbacks and QML debugger plugins in a
        # production installer; this also keeps their unrelated licence surfaces out.
        DEPLOY_TOOL_OPTIONS
            --no-opengl-sw
            --no-system-dxc-compiler
            --skip-plugin-types qmltooling
    )

    # Qt's defaults put plugins under plugins/ and QML under qml/, but at run time Qt
    # searches for them relative to the executable's own directory -- platforms/ and
    # the module directories, unprefixed. Deploying to the defaults produces an
    # executable that dies on startup with "no Qt platform plugin could be
    # initialized". install(CODE) runs in the same scope as the script below.
    install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
    install(CODE "set(QT_DEPLOY_LIB_DIR \".\")")
    install(CODE "set(QT_DEPLOY_PLUGINS_DIR \".\")")
    install(CODE "set(QT_DEPLOY_QML_DIR \".\")")
    install(SCRIPT "${rc_app_deploy_script}")
endif()
