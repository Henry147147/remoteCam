# rcwin-net: the TCP receiver the phone connects to.
#
# Framing and message parsing come from rc-core and are shared byte-for-byte with the
# iOS client; this target is only the socket, the threads and the lifetime. That split
# is what lets the protocol be tested on a Linux runner with no Winsock at all.

add_library(rcwin-net STATIC
    src/bonjour_service.cpp
    src/tcp_listener.cpp
)

target_include_directories(rcwin-net PUBLIC include)

target_compile_features(rcwin-net PUBLIC cxx_std_20)

target_link_libraries(rcwin-net PUBLIC
    rc-core
    rcwin-common
    dnsapi
    ws2_32
)

rc_target_strict_warnings(rcwin-net)

# Real sockets over loopback, not a mock. The bugs this layer actually has -- partial
# sends, headers split across reads, a peer that vanishes mid-frame -- are precisely the
# ones a mocked socket cannot have.
if(RC_BUILD_TESTS)
    add_executable(rcwin-net-tests tests/tcp_listener_test.cpp)
    target_link_libraries(rcwin-net-tests PRIVATE rcwin-net)
    rc_target_strict_warnings(rcwin-net-tests)
    add_test(NAME rcwin-net-tests COMMAND rcwin-net-tests)
endif()
