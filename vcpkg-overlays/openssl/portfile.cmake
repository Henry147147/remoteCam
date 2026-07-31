# The locked vcpkg baseline predates OpenSSL 3.5.7. Keep this overlay deliberately
# small: it pins the official release archive, reuses only the exact Windows support
# files from builtin-baseline 9e593bb18ea69cc5095e012465dcd675a822ed0d, and verifies
# their normalized hashes before building. Any baseline drift therefore fails closed.

if(EXISTS "${CURRENT_INSTALLED_DIR}/share/libressl/copyright" OR
   EXISTS "${CURRENT_INSTALLED_DIR}/share/boringssl/copyright")
    message(FATAL_ERROR
        "OpenSSL cannot coexist with LibreSSL or BoringSSL in the same vcpkg prefix.")
endif()

if(NOT VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_UWP OR
   NOT VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    message(FATAL_ERROR "RemoteCam's OpenSSL 3.5.7 overlay supports desktop Windows x64 only.")
endif()

set(_rc_builtin_openssl "${VCPKG_ROOT_DIR}/ports/openssl")
function(rc_assert_baseline_file relative_path expected_sha512)
    set(path "${_rc_builtin_openssl}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Pinned vcpkg OpenSSL support file is missing: ${path}")
    endif()
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA512 actual_sha512 "${contents}")
    if(NOT actual_sha512 STREQUAL expected_sha512)
        message(FATAL_ERROR
            "Pinned vcpkg OpenSSL support file changed: ${relative_path}. "
            "Use builtin-baseline 9e593bb18ea69cc5095e012465dcd675a822ed0d.")
    endif()
endfunction()

rc_assert_baseline_file("cmake-config.patch" "cf0cb488b6987f9471432c5bd6ee82c759971a8a14fc06dc85b237bb7b6b922ba0bf783ddf97936b6b0f126d3870bc4ef0d1613130f57fae639eb3a4fdbd4b58")
rc_assert_baseline_file("command-line-length.patch" "2baca5e495941684c7299dd05c97c177ecec593e13eeefdab2092843b5ec4b5b68b9ef127289616d8ea4341831ac957d3d8c5bbd5047184f0574e2eda73f8eff")
rc_assert_baseline_file("script-prefix.patch" "d479f469e50cf6ca060f8b33bd3f1691705abb7d6a5c56f65993256c7c290d3db3a9d1aacae58acf1face08685e58cef303ab28d2ad804512cd4375ce8069142")
rc_assert_baseline_file("windows/install-layout.patch" "e5e664f7faebe28614bb98571d57045a9ba2aea05cb8740ef3bacb5f0e4ae4ed5f3cb1ce52f2583f04fa98d3f45b5bbbc6dd699e279505ef0e9ed7edb5395b48")
rc_assert_baseline_file("windows/install-pdbs.patch" "ca4e5f2d23d16ab0fbd60b92f11e16ed6431be62dadafbdcd9e34b9fa434c0972d04042149e689f431661049cbb20c9023489023b0af2af682b0a6adc311e13c")
rc_assert_baseline_file("windows/install-programs.diff" "80bfed28be63dfdcb3ee35c78e9e5f38c030a7f6cae386085b1b2ca469a51d59414f394c5e5f031b3cd9c456e0e53c8d4e3eab592e170c28a960ccaa3a3e43f2")
rc_assert_baseline_file("windows/portfile.cmake" "d275a9450af5992f468769613d2f7070527d84d1235c1c816389ec7fce4a55b5d67552d591bde2990797778b5f1d32298474e64bf23f314199906cb978a12346")
rc_assert_baseline_file("install-pc-files.cmake" "d063fad26bb7650142d106e7aecf9ba8c1922f2c88f2da34715f4e568601177f51e39877109dec9aaf74336a33b60ad4c053c97ee04f072070522ab9b9495e8c")
rc_assert_baseline_file("usage" "7e1c492194bdab575ac1eee09238f97c4ba66aede2c20aad1aa8525abab40d0181eaee1a2ec8689d579ae73c0814bb3b1ef6b4556a65b192c5f7afd2569518b5")
rc_assert_baseline_file("vcpkg-cmake-wrapper.cmake.in" "e303d6b5139260501af10892024774b982bb0594b9bfa5e1c71dad1c01c70036dd3bbadf6f7095c997dbb31cc8c210d77266a055ebf2f9f1dbcd834a7aeaf19f")

# OpenSSL publishes SHA-256 a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
# for this release. vcpkg verifies the same official archive with SHA-512 below.
vcpkg_download_distfile(OPENSSL_SOURCE_ARCHIVE
    URLS "https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz"
    FILENAME "openssl-3.5.7.tar.gz"
    SHA512 de5351d2d532e1a3908a738f7d8aae448d32bc60bdb24808c556a24bc37a3f53daedf12b5d432eeb8c235e16939d842f908332ede8a447ca103ad1c493c820d7
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${OPENSSL_SOURCE_ARCHIVE}"
    PATCHES
        "${_rc_builtin_openssl}/cmake-config.patch"
        "${_rc_builtin_openssl}/command-line-length.patch"
        "${_rc_builtin_openssl}/script-prefix.patch"
        "${_rc_builtin_openssl}/windows/install-layout.patch"
        "${_rc_builtin_openssl}/windows/install-pdbs.patch"
        "${_rc_builtin_openssl}/windows/install-programs.diff"
)

vcpkg_list(SET CONFIGURE_OPTIONS
    enable-static-engine
    enable-capieng
    no-tests
    no-docs
)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_list(APPEND CONFIGURE_OPTIONS shared)
else()
    message(FATAL_ERROR "RemoteCam requires replaceable dynamic OpenSSL libraries.")
endif()
vcpkg_list(APPEND CONFIGURE_OPTIONS no-apps)

include("${_rc_builtin_openssl}/windows/portfile.cmake")
include("${_rc_builtin_openssl}/install-pc-files.cmake")

file(INSTALL "${_rc_builtin_openssl}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

set(OPENSSL_VERSION_MAJOR 3)
set(OPENSSL_VERSION_MINOR 5)
set(OPENSSL_VERSION_FIX 7)
configure_file(
    "${_rc_builtin_openssl}/vcpkg-cmake-wrapper.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/vcpkg-cmake-wrapper.cmake"
    @ONLY)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
