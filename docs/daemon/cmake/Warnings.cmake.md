# daemon/cmake/Warnings.cmake

## Purpose

Defines the two compiler warning sets of the daemon. Our code gets a strict set. Copied upstream code gets the pgvector flags.

## Functions

| Function | Effect |
| :--- | :--- |
| `av_strict(<target>)` | Adds `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -Wformat=2 -Wno-unused-parameter` to the target. |
| `av_upstream(<target>)` | Adds the pgvector Makefile flags to the target: `-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -ffp-contract=fast -fno-strict-aliasing -fwrapv`, plus three warning suppressions. |
| `av_upstream_sources(<files...>)` | Sets the upstream flags on individual source files only. |

## Ownership and thread safety

Build-time only.

## Testing

Indirect. A warning in our code fails the build because of `-Werror`. Copied code must build without `-Werror`.

## Notes

- The upstream set does not include `-Werror`. This lets copied code build unchanged.
