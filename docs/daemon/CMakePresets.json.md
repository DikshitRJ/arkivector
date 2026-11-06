# daemon/CMakePresets.json

## Purpose

Named configure, build and test presets for the daemon. Each preset maps to one build mode, such as debug, release or a sanitizer build.

## Presets

| Configure preset | Build type | Extra cache variables | Use |
| :--- | :--- | :--- | :--- |
| `dev` | Debug | none | Day-to-day build. Generator: Ninja. |
| `release` | RelWithDebInfo | none | Optimised build with debug info. |
| `asan` | Debug | `AV_ASAN=ON`, `AV_UBSAN=ON` | AddressSanitizer and UBSan. |
| `ubsan` | Debug | `AV_UBSAN=ON` | UBSan only. |
| `tsan` | RelWithDebInfo | clang, `AV_TSAN=ON` | ThreadSanitizer. gcc's TSan runtime fails at start with glibc 2.44. |
| `fuzz` | RelWithDebInfo | clang, `AV_FUZZ=ON`, `AV_ASAN=ON` | libFuzzer targets. |
| `release-static` | Release | `AV_STATIC=ON`, `AV_BUILD_TESTS=OFF` | Static build. Not yet implemented. |

All presets use `${sourceDir}/build/${presetName}` as the binary directory and set `AV_NATIVE=OFF`.

Build presets exist for `dev`, `release`, `asan`, `ubsan`, `tsan`, `fuzz` and `release-static`. Test presets exist for `dev`, `release`, `asan`, `ubsan` and `tsan`. The ASan test preset sets `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`. The TSan test preset sets `TSAN_OPTIONS=halt_on_error=1`.

## Ownership and thread safety

Build configuration only. No runtime state.

## Testing

Each preset is used by CI. Run `cmake --preset <name>` and `cmake --build --preset <name>`. Run `ctest --preset <name>` for the test presets.

## Notes

- `release-static` sets `AV_STATIC`. The top-level CMake file does not read that variable yet.
