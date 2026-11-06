# daemon/cmake/Vendored.cmake

## Purpose

Builds the third-party libraries of the daemon from `daemon/third_party/`, and finds the system libraries that the daemon needs.

## Targets

| Target | Kind | Source |
| :--- | :--- | :--- |
| `av3p_yyjson` | static | `third_party/yyjson/yyjson.c` |
| `av3p_roaring` | static | `third_party/roaring/roaring.c` |
| `av3p_llhttp` | static | `llhttp/api.c`, `http.c`, `llhttp.c` |
| `av3p_lmdb` | static | `lmdb/mdb.c`, `midl.c` |
| `av3p_tomlc99` | static | `tomlc99/toml.c` |
| `av3p_secp256k1` | static | `secp256k1` v0.8.0 sources and two precomputed tables |
| `av3p_keccak` | static | `tiny_sha3/sha3.c` |
| `av3p_greatest` | interface | `greatest` headers |
| `av3p_uv` | interface | system libuv, through pkg-config |
| `av3p_curl` | interface | system libcurl, through pkg-config |
| `av3p_openssl` | interface | system OpenSSL (`OpenSSL::Crypto`) |

Vendored static libraries build with `-w` (no warnings) and export their include paths as `SYSTEM`.

## Version floors

| Library | Minimum | Reason |
| :--- | :--- | :--- |
| libuv | 1.48.0 | GHSA-f74f-cvh7-c6q6. |
| libcurl | 8.11 | WebSocket support. Warns below 8.23.0 for CURL-CVE-2026-92392 unless the distribution backported the fix. |

The configure step stops with an error when libuv or libcurl are missing or too old. It also stops when OpenSSL is missing.

## secp256k1 constants

`ECMULT_WINDOW_SIZE=15`, `COMB_BLOCKS=43` and `COMB_TEETH=6` must match the precomputed tables. Do not change one value without the others.

## Ownership and thread safety

Build-time only.

## Testing

The configure step checks the versions. No runtime test.

## Notes

- The file is included for every configuration, including D0 builds that do not use these libraries.
