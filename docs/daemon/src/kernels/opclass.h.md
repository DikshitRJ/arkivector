# daemon/src/kernels/opclass.h

## Purpose

Registry of the 14 pgvector operator classes. Each entry maps a class name and operator to the distance function the API returns, and to the distance function that HNSW uses inside the index.

## Type

`av_opclass` has these fields:
- `name` (for example `halfvec_cosine_ops`), `type`, `op` (for example `<=>`).
- `distance`: the API-facing distance. The square root is applied for L2.
- `index_distance`: the distance inside HNSW, for example squared L2 or negative inner product.
- `normalize`: true for cosine. Values are L2-normalised at insert and for queries.
- `metric` and `index_metric`.

## Public API

| Function | Purpose |
| :--- | :--- |
| `const av_opclass *av_opclass_find(const char *name)` | Look up by class name. |
| `const av_opclass *av_opclass_for_operator(av_vtype t, const char *op)` | Look up by type and operator. |
| `const av_opclass *av_opclass_at(size_t i)` | Entry `i`, or NULL past the end. |
| `size_t av_opclass_count(void)` | Number of entries. 14. |
| `av_pgfn av_opclass_index_pgfn(const av_opclass *oc)` | The pgvector function for `index_distance`. |
| `av_pgfn av_opclass_norm_pgfn(const av_opclass *oc)` | The norm function for cosine. NULL otherwise. |

## Behavior

The distance functions never raise an error. A dimension mismatch returns NaN. Callers must check dimensions first.

## Ownership and thread safety

The registry is static and read-only. Reentrant.

## Testing

`test/unit/test_kernels.c`, test `opclass_registry_matches_pgvector`.
