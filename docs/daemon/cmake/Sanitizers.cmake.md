# daemon/cmake/Sanitizers.cmake

## Purpose

Defines the sanitizer and fuzzer options. The options apply to every target in the build.

## Options

| Option | Default | Effect |
| :--- | :---: | :--- |
| `AV_ASAN` | OFF | `-fsanitize=address -fno-omit-frame-pointer`. |
| `AV_UBSAN` | OFF | `-fsanitize=undefined -fno-sanitize-recover=undefined`, plus `-fno-sanitize=shift-base`. Clang also gets `-fno-sanitize=function`. |
| `AV_TSAN` | OFF | `-fsanitize=thread -fno-omit-frame-pointer`. |
| `AV_FUZZ` | OFF | Adds `-fsanitize=fuzzer-no-link`. Requires clang. |

`AV_ASAN` and `AV_TSAN` cannot be ON together. The configure step stops with an error if both are set.

The flags apply with `add_compile_options` and `add_link_options`, so they reach every target.

## Behavior

- `shift-base` is excluded because pgvector's `HalfToFloat4` in `halfutils.h` shifts a promoted value. The result is correct on gcc and clang. The copied file cannot be edited.

## Ownership and thread safety

Build-time only.

## Testing

Run the `asan`, `ubsan` and `tsan` presets. The `fuzz` preset builds the libFuzzer targets.

## Notes

- The Clang-only `-fno-sanitize=function` exclusion covers copied kernels that cast function pointers.
