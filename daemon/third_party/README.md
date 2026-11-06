# daemon/third_party

Vendored third-party C sources for `arkivectord`. They are built by `daemon/cmake/Vendored.cmake`
with their own minimal flags (`-w`, no `-Werror`). Exact versions, URLs and sha256 sums are in
[`VENDORED.lock`](VENDORED.lock). Each directory keeps its upstream licence file.
All licences are permissive (MIT, Apache-2.0/MIT, OpenLDAP, ISC). No GPL or LGPL code.

| Dir | CMake target | What and why |
| :--- | :--- | :--- |
| `yyjson/` | `av3p_yyjson` | Fast JSON reader/writer. Parses RPC and HTTP JSON. |
| `roaring/` | `av3p_roaring` | CRoaring amalgamation (`roaring.c/.h`). Compressed bitmaps for attribute filters. |
| `llhttp/` | `av3p_llhttp` | HTTP/1.1 parser (generated C from the `release/*` tag) for the REST server. |
| `lmdb/` | `av3p_lmdb` | Embedded transactional key-value store for the entity store. Links Threads. |
| `tomlc99/` | `av3p_tomlc99` | TOML parser for the config file. |
| `secp256k1/` | `av3p_secp256k1` | ECDSA signing and recovery (module `recovery`) for Arkiv transactions. Built from `secp256k1.c` plus the two precomputed tables with `ENABLE_MODULE_RECOVERY=1 ECMULT_WINDOW_SIZE=15 COMB_BLOCKS=43 COMB_TEETH=6` (ECMULT_GEN_KB=86). Tests, benchmarks and other modules were removed from the tree. |
| `tiny_sha3/` | `av3p_keccak` | Unmodified `sha3.c/h`. We add Keccak-256 padding (0x01) in our own code. |
| `greatest/` | `av3p_greatest` (INTERFACE) | Single-header unit test framework. |

System packages (not vendored, found with `pkg-config`, configure fails with a clear message if missing):
`libuv` -> `av3p_uv`, `libcurl >= 8.11` (WebSocket) -> `av3p_curl`, OpenSSL libcrypto -> `av3p_openssl`.

Every static target exports its include directory as `SYSTEM PUBLIC`, so the strict
warnings of our own code do not fire on third-party headers.

## How to update a library

1. Pick the new release or commit. Download the official archive into a scratch directory
   (never commit archives).
2. Record `sha256sum` of the downloaded artifact.
3. Replace only the files listed in the `artifact` field of the entry. Keep the upstream `LICENSE`.
4. Update version, url, sha256 in `VENDORED.lock`.
5. For secp256k1: if you change `ECMULT_WINDOW_SIZE` or the comb size, you must use the
   matching `precomputed_ecmult*.c` files and update the defines in `Vendored.cmake`.
6. Build with gcc and clang and run the unit and integration tests.
