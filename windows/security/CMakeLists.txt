# Production pairing, authenticated control, and optional media protection.

find_package(OpenSSL 3.5.7 EXACT QUIET COMPONENTS Crypto)

add_library(rcwin-security STATIC
    src/errors.cpp
    src/pairing_store.cpp
    src/rate_limiter.cpp
)

target_include_directories(rcwin-security PUBLIC include)
target_compile_features(rcwin-security PUBLIC cxx_std_20)
target_link_libraries(rcwin-security PRIVATE crypt32)

if(OpenSSL_FOUND AND OPENSSL_VERSION VERSION_EQUAL 3.5.7)
    target_sources(rcwin-security PRIVATE
        src/crypto.cpp
        src/spake2.cpp
    )
    target_link_libraries(rcwin-security PRIVATE OpenSSL::Crypto)
    target_compile_definitions(rcwin-security PUBLIC RCSECURITY_OPENSSL_AVAILABLE=1)
    message(STATUS "RemoteCam security enabled with OpenSSL ${OPENSSL_VERSION}")
else()
    target_sources(rcwin-security PRIVATE src/unavailable.cpp)
    target_compile_definitions(rcwin-security PUBLIC RCSECURITY_OPENSSL_AVAILABLE=0)
    if(OpenSSL_FOUND)
        message(WARNING
            "RemoteCam security requires exactly reviewed OpenSSL 3.5.7; found "
            "${OPENSSL_VERSION}. Production authentication will fail closed.")
    else()
        message(WARNING
            "OpenSSL 3.5.7 was not found. Production authentication will fail closed; "
            "no insecure crypto fallback is compiled.")
    endif()
endif()

rc_target_strict_warnings(rcwin-security)

if(RC_BUILD_TESTS)
    add_executable(rcwin-security-tests tests/security_test.cpp)
    target_link_libraries(rcwin-security-tests PRIVATE rcwin-security)
    rc_target_strict_warnings(rcwin-security-tests)
    add_test(NAME rcwin-security-tests COMMAND rcwin-security-tests)
endif()
