# daemon/src/kernels/dispatch.h

## Purpose

CPU tier detection, and the table of pgvector distance functions per value type and metric.

## Types

- `av_metric`: `AV_METRIC_L2` (with square root), `L2SQ`, `IP`, `NEG_IP`, `COSINE`, `L1`, `HAMMING`, `JACCARD`, `NORM`, and `AV_METRIC_COUNT`.
- `av_pgfn`: `unsigned long (*)(av_fcinfo)`, the SQL-callable pgvector function. Call it through `DirectFunctionCall*`.

## Public API

| Function | Purpose |
| :--- | :--- |
| `void av_kernels_init(void)` | Detect the tier and fill the tables. Idempotent. Uses `pthread_once`. |
| `const char *av_kernel_tier(void)` | The active tier: `x86-64`, `x86-64-v3`, `x86-64-v4`, `aarch64`, `native` or `generic`. |
| `const char *av_kernel_tier_supported(void)` | The highest tier that the CPU and OS support. |
| `av_pgfn av_kernel_fn(av_vtype type, av_metric metric)` | The function for a pair, or NULL when pgvector has none. |
| `av_pgfn av_kernel_fn_tier(const char *tier, av_vtype type, av_metric metric)` | The function for a named tier. For tests. |

## Environment

`AV_KERNEL_TIER` can lower the active tier to `x86-64` or `x86-64-v3`. See `dispatch.c` for the exact rule.

## Ownership and thread safety

- Call `av_kernels_init()` before the first search or index build. The other functions call it as well.
- The tables are written once, inside `pthread_once`. Reads after that are safe from any thread.

## Testing

`test/unit/test_kernels.c` checks every available tier against a double-precision reference for dimensions 1 to 16000.
