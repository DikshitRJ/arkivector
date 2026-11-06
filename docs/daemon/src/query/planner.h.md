# daemon/src/query/planner.h

## Purpose

Declares the vector query planning interface and strategy types for Arkivector (Phase D3 baseline, extended in Phase D4). It selects the search execution strategy based on collection state, candidate bitmap cardinality, and requested parameters, populating an execution plan structure for query runners and `explain` output.

## Key Types and Functions

| Symbol | Role |
| :--- | :--- |
| `av_plan_strategy` | Enumeration of vector search strategies: `AV_STRAT_EXACT_FLAT`, `AV_STRAT_HNSW`, `AV_STRAT_HNSW_ITERATIVE`, `AV_STRAT_MIXED`, `AV_STRAT_REMOTE_PREFILTER`, `AV_STRAT_REMOTE_POSTFILTER`. |
| `av_query_plan` | Plan representation containing selected `strategy`, `exact` boolean, `candidates` count, `selectivity` ratio, active `kernel` tier name, and planning/execution `elapsed_ms`. |
| `av_plan_strategy_name` | Maps an `av_plan_strategy` enum value to its canonical string representation (e.g. `"exact_flat"`). |
| `av_plan_query` | Formulates the execution plan for a vector search query. |

### Function Signature

```c
av_status av_plan_query(const av_collection *col, const void *query, const av_opclass *oc,
                        bool force_exact, const roaring_bitmap_t *cand, size_t live_total,
                        av_query_plan *out, av_err *e);
```

## Memory Ownership

- **Output Struct**: Caller owns the `av_query_plan *out` pointer.
- **Strings**: `out->kernel` points to a static string owned by the kernel subsystem (`av_kernel_tier()`); no heap memory is allocated by the planner.

## Invariants

- In Phase D3, `out->strategy` is unconditionally set to `AV_STRAT_EXACT_FLAT` and `out->exact` is `true`.
- `out->selectivity` is clamped to the range $[0.0, 1.0]$. If `live_total == 0`, selectivity defaults to `1.0`.
- `out->candidates` matches the cardinality of `cand` (or `live_total` when `cand == NULL`).

## Error Handling

- Always initializes `out` via `memset`.
- Returns `AV_OK` on successful planning.

## Thread Safety

- The query planner is a pure, stateless function.
- Multiple query worker threads can plan queries concurrently without locking.
