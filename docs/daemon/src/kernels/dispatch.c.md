# daemon/src/kernels/dispatch.c

## Purpose

Implements the tier detection and the function tables in `dispatch.h`.

## Detection

The CPU checks follow pgvector's `SupportsCpuFeature` and `SupportsAvx512Popcount`. Leaf 1 and leaf 7 bits are read with `cpuid`. The OS state is read with `_xgetbv(0)`.

- Tier v3 needs FMA, MOVBE, POPCNT, AVX, F16C, BMI1, AVX2, BMI2 and LZCNT. It also needs XMM and YMM state enabled.
- Tier v4 needs AVX-512 F, DQ, CD, BW and VL. It also needs opmask and ZMM state enabled.
- Without XSAVE, the level is baseline.

## Tables

`FILL` writes the function for each (type, metric) pair into a table. There are three tables: baseline, `_t3` and `_t4`. The `_t3` and `_t4` tables exist only when `AV_KERNEL_TIERS` is defined. Otherwise they copy the baseline table.

The bit metrics and norms are not tiered.

## Override

When `AV_KERNEL_TIER` is set:
- `x86-64` selects the baseline table.
- `x86-64-v3` selects the v3 table only when the CPU supports v3.

`x86-64-v4` is not handled by the code. Setting it has no effect. The header comment lists it as valid. This is a known gap.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `init_once` | Call `HalfvecInit` and `BitvecInit` from pgvector. Fill the tables. Pick the active tier. |
| `av_kernel_fn` | Return the entry for the active table. |
| `av_kernel_fn_tier` | Return the entry for a named tier, when the CPU can run it. |

## Ownership and thread safety

- The tables and names are static. They are written inside `pthread_once`.
- `av_kernel_fn` and `av_kernel_fn_tier` return pointers into static tables. Callers must not free them.

## Testing

`test/unit/test_kernels.c` runs each available tier. `bench/kernels_bench.c` reports the per-tier timings.
