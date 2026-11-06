# daemon/CMakeLists.txt

## Purpose

Top-level build file of the daemon. It defines the static libraries that the daemon and the tests use, and the `arkivectord` executable.

## Targets

| Target | Kind | Sources | Notes |
| :--- | :--- | :--- | :--- |
| `av_base` | static | `src/core/arena.c` | Allocator shared by pgcompat and our code. Strict warnings. |
| `av_pgcompat` | static | `src/pgcompat/src/{palloc,elog,fmgr,lwlock,misc}.c` (ours) and `{list,pairingheap,pg_prng,pg_bitutils,f2s,varbit_io}.c` (verbatim) | Upstream flags for the verbatim files only. Defines `ARKIVECTOR=1`. |
| `av_pgvector` | static | `src/pgvector/*.c` plus optional `hnswutils.c`, `hnswbuild_mem.c`, `av_hnsw_glue.c` | Upstream flags. Kernel tiers added by `KernelTiers.cmake`. |
| `av_core` | static | `src/version.c`, `src/log.c`, `src/codec/*.c`, `src/kernels/*.c` | Strict warnings. |
| `av_kernels_bench` | executable | `bench/kernels_bench.c` | Built when `AV_BUILD_BENCH` is ON. |
| `arkivectord` | executable | `src/main.c` | Strict warnings. |

## Options

| Option | Default | Effect |
| :--- | :---: | :--- |
| `AV_NATIVE` | OFF | Build the pgvector kernels once with `-march=native`. |
| `AV_BUILD_TESTS` | ON | Add `test/CMakeLists.txt`. |
| `AV_BUILD_BENCH` | ON | Build `av_kernels_bench`. |

Sanitizer and fuzz options come from `cmake/Sanitizers.cmake`.

## Behavior

- Source lists are filtered at configure time. A verbatim or optional file that does not exist is removed from its list. Missing files do not stop the configure step.
- The default build type is `RelWithDebInfo`.
- `pg_config.h` is generated from `src/pgcompat/include/pg_config.h.in` into the build directory. The probes check `cpuid.h`, `__get_cpuid`, `__get_cpuid_count`, `_xgetbv` and `__builtin_popcount`.
- The build includes `cmake/Warnings.cmake`, `cmake/Sanitizers.cmake`, `cmake/Vendored.cmake` and `cmake/KernelTiers.cmake`.
- `AV_REPO_ROOT` is passed to `av_core` as a compile definition.

## Ownership and thread safety

Build-time only. No runtime state.

## Testing

- Configure and build with `cmake --preset dev && cmake --build --preset dev`.
- `ctest --preset dev` runs the unit suites that `test/CMakeLists.txt` registers.
- CI builds the presets in `.github/workflows/daemon.yml`.

## Notes

- `Vendored.cmake` is included unconditionally. A D0 configure therefore needs libuv, libcurl and OpenSSL development files, and the `third_party/` sources, even though D0 code does not use them yet.
- The preset `release-static` sets `AV_STATIC=ON`. No option named `AV_STATIC` exists in this file or in the included cmake modules. The preset has no effect until D6.
