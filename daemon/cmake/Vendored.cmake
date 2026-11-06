# Vendored.cmake: third-party libraries for arkivectord.
#
# Included from daemon/CMakeLists.txt. Expects AV_THIRD_PARTY_DIR to point to daemon/third_party
# (defaults to ${CMAKE_CURRENT_SOURCE_DIR}/third_party). Pinned versions, URLs and sha256 sums are
# in third_party/VENDORED.lock.
#
# Targets (all names are av3p_*):
#   STATIC:    av3p_yyjson av3p_roaring av3p_llhttp av3p_lmdb av3p_tomlc99 av3p_secp256k1 av3p_keccak
#   INTERFACE: av3p_greatest av3p_uv av3p_curl av3p_openssl
#
# Vendored sources build with their own minimal flags: no -Werror, warnings off (-w). Include
# directories are exported as SYSTEM so our strict warnings do not fire on third-party headers.

if(NOT DEFINED AV_THIRD_PARTY_DIR)
  set(AV_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
endif()

find_package(Threads REQUIRED)

# av3p_add_static(<target> <include-dir> <sources...>)
function(av3p_add_static target incdir)
  add_library(${target} STATIC ${ARGN})
  target_include_directories(${target} SYSTEM PUBLIC "${incdir}")
  target_compile_options(${target} PRIVATE -w)
  set_target_properties(${target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_EXTENSIONS ON
    C_CLANG_TIDY ""
    C_CPPCHECK "")
endfunction()

set(_tp "${AV_THIRD_PARTY_DIR}")

av3p_add_static(av3p_yyjson "${_tp}/yyjson" "${_tp}/yyjson/yyjson.c")

av3p_add_static(av3p_roaring "${_tp}/roaring" "${_tp}/roaring/roaring.c")
target_link_libraries(av3p_roaring PRIVATE Threads::Threads)

av3p_add_static(av3p_llhttp "${_tp}/llhttp"
  "${_tp}/llhttp/api.c" "${_tp}/llhttp/http.c" "${_tp}/llhttp/llhttp.c")

av3p_add_static(av3p_lmdb "${_tp}/lmdb" "${_tp}/lmdb/mdb.c" "${_tp}/lmdb/midl.c")
target_link_libraries(av3p_lmdb PUBLIC Threads::Threads)

av3p_add_static(av3p_tomlc99 "${_tp}/tomlc99" "${_tp}/tomlc99/toml.c")

# libsecp256k1 v0.8.0: compiled directly (secp256k1.c plus the two precomputed tables).
# ECMULT_WINDOW_SIZE=15 and COMB_BLOCKS=43/COMB_TEETH=6 (= ECMULT_GEN_KB 86) match the
# precomputed tables shipped in src/precomputed_ecmult*.c. Do not change one without the other.
av3p_add_static(av3p_secp256k1 "${_tp}/secp256k1/include"
  "${_tp}/secp256k1/src/secp256k1.c"
  "${_tp}/secp256k1/src/precomputed_ecmult.c"
  "${_tp}/secp256k1/src/precomputed_ecmult_gen.c")
target_include_directories(av3p_secp256k1 PRIVATE "${_tp}/secp256k1" "${_tp}/secp256k1/src")
target_compile_definitions(av3p_secp256k1 PRIVATE
  ENABLE_MODULE_RECOVERY=1
  ECMULT_WINDOW_SIZE=15
  COMB_BLOCKS=43
  COMB_TEETH=6)

# tiny_sha3 (unmodified). Keccak padding (0x01 instead of 0x06) is added later in our own code.
av3p_add_static(av3p_keccak "${_tp}/tiny_sha3" "${_tp}/tiny_sha3/sha3.c")

add_library(av3p_greatest INTERFACE)
target_include_directories(av3p_greatest SYSTEM INTERFACE "${_tp}/greatest")

# System libraries (found with pkg-config).
find_package(PkgConfig REQUIRED)

pkg_check_modules(AV_LIBUV QUIET IMPORTED_TARGET libuv)
if(NOT AV_LIBUV_FOUND)
  message(FATAL_ERROR "arkivectord needs the libuv development package, but pkg-config could not find 'libuv'. Install it (Debian/Ubuntu: libuv1-dev, Fedora/openSUSE: libuv-devel, macOS: brew install libuv) and re-run cmake.")
endif()
# Security floor (dependency audit 2026-11-06): GHSA-f74f-cvh7-c6q6 fixed in 1.48.0.
if(AV_LIBUV_VERSION VERSION_LESS "1.48.0")
  message(FATAL_ERROR "arkivectord needs libuv >= 1.48.0 (GHSA-f74f-cvh7-c6q6), found ${AV_LIBUV_VERSION}.")
endif()
add_library(av3p_uv INTERFACE)
target_link_libraries(av3p_uv INTERFACE PkgConfig::AV_LIBUV)

pkg_check_modules(AV_LIBCURL QUIET IMPORTED_TARGET libcurl)
if(NOT AV_LIBCURL_FOUND)
  message(FATAL_ERROR "arkivectord needs the libcurl development package (>= 8.11, for WebSocket support), but pkg-config could not find 'libcurl'. Install it (Debian/Ubuntu: libcurl4-openssl-dev, Fedora/openSUSE: libcurl-devel, macOS: brew install curl) and re-run cmake.")
endif()
if(AV_LIBCURL_VERSION VERSION_LESS "8.11")
  message(FATAL_ERROR "arkivectord needs libcurl >= 8.11 for WebSocket support, but found ${AV_LIBCURL_VERSION}. Install a newer libcurl and re-run cmake.")
endif()
# Security floor (dependency audit 2026-11-06): CURL-CVE-2026-92392 (High) affects
# 8.12..8.22 except the patched 8.14.3, 8.16.2 and 8.20.2 releases. Distro builds may
# carry backports, so this is a warning; release builds pin 8.23.0 (D6).
if(AV_LIBCURL_VERSION VERSION_LESS "8.23.0" AND NOT AV_LIBCURL_VERSION VERSION_EQUAL "8.14.3"
   AND NOT AV_LIBCURL_VERSION VERSION_EQUAL "8.16.2" AND NOT AV_LIBCURL_VERSION VERSION_EQUAL "8.20.2")
  message(WARNING "libcurl ${AV_LIBCURL_VERSION} may be affected by CURL-CVE-2026-92392; use >= 8.23.0 unless your distribution backported the fix.")
endif()
add_library(av3p_curl INTERFACE)
target_link_libraries(av3p_curl INTERFACE PkgConfig::AV_LIBCURL)

# OpenSSL (libcrypto) for keystore decryption.
find_package(OpenSSL QUIET)
if(NOT OPENSSL_FOUND)
  message(FATAL_ERROR "arkivectord needs the OpenSSL (libcrypto) development files for keystore decryption. Install them (Debian/Ubuntu: libssl-dev, Fedora: openssl-devel, openSUSE: libopenssl-devel, macOS: brew install openssl) and re-run cmake.")
endif()
add_library(av3p_openssl INTERFACE)
target_link_libraries(av3p_openssl INTERFACE OpenSSL::Crypto)

unset(_tp)
