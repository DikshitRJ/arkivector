# daemon/src/query/planner.c

## Purpose

Implements the query planner declared in `planner.h`. In Phase D3, it provides the baseline exact flat scan planner, computing filter selectivity and assigning the active hardware kernel tier.

## Key Types and Functions

| Function / Type | Role |
| :--- | :--- |
| `av_plan_strategy_name` | Translates `av_plan_strategy` enum values into static lowercase strings matching OpenAPI spec values: `"exact_flat"`, `"hnsw"`, `"hnsw_iterative"`, `"mixed"`, `"remote_prefilter"`, `"remote_postfilter"`. |
| `av_plan_query` | Populates `av_query_plan` with `AV_STRAT_EXACT_FLAT`, calculates candidate count from `cand` bitmap, derives selectivity against `live_total`, queries `av_kernel_tier()`, and zeroes elapsed time. |

## Memory Ownership

- **Static Strings**: String returned by `av_plan_strategy_name` and assigned to `out->kernel` are static string literals.
- **Zero Allocation**: `av_plan_query` performs no heap allocations.

## Invariants

- **Strategy Enforcement**: In D3, always selects `AV_STRAT_EXACT_FLAT` with `exact = true`.
- **Selectivity Bounds**: When `live_total > 0`, `selectivity = (double)candidates / (double)live_total`; otherwise `1.0`.

## Error Handling

- Always zeroes destination struct with `memset`.
- Returns `AV_OK`.

## Thread Safety

- Reentrant and thread-safe. Reads immutable collection parameters and candidate bitmap.
