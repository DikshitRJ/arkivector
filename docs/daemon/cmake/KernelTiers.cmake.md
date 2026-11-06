# daemon/cmake/KernelTiers.cmake

## Purpose

Builds the pgvector distance kernels once for each x86-64 CPU tier. The runtime dispatch in `kernels/dispatch.c` then picks the best tier that the CPU supports.

## Tiers

| Tier | Compiler flag | Symbol suffix |
| :--- | :--- | :--- |
| baseline | `-march=x86-64` | none |
| v3 | `-march=x86-64-v3` | `_t3` |
| v4 | `-march=x86-64-v4` | `_t4` |

The kernel files are `src/pgvector/vector_kernels.c`, `halfvec_kernels.c` and `sparsevec_kernels.c`. The copied files are not edited. The renaming happens through `-include src/pgvector/av_tier.h` and the define `AV_TIER_SUFFIX=_t3` or `_t4`.

## Function

`av_add_kernel_tiers(<target>)` adds the baseline sources to the target. It also adds one object library per extra tier, and sets `AV_KERNEL_TIERS=1` on the target.

## Behavior

- Tiers are built only when the processor is `x86_64` or `AMD64`, and `AV_NATIVE` is OFF.
- With `AV_NATIVE=ON`, or on a non-x86-64 host, the kernels build once.

## Ownership and thread safety

Build-time only.

## Testing

`test/unit/test_kernels.c` checks every tier that is available against a double-precision reference.

## Notes

- Without the tier build, `dispatch.c` copies the baseline table into the other tables.
