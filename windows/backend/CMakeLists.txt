add_library(rcwin-backend STATIC
    src/session_controller.cpp
)

target_include_directories(rcwin-backend PUBLIC include)
target_compile_features(rcwin-backend PUBLIC cxx_std_20)
target_link_libraries(rcwin-backend PUBLIC rcwin-net rcwin-security)
rc_target_strict_warnings(rcwin-backend)

if(RC_BUILD_TESTS)
    add_executable(rcwin-backend-tests tests/session_controller_test.cpp)
    target_link_libraries(rcwin-backend-tests PRIVATE rcwin-backend rcfakephone)
    rc_target_strict_warnings(rcwin-backend-tests)
    add_test(NAME rcwin-backend-tests COMMAND rcwin-backend-tests)
endif()
