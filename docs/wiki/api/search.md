# Search

`POST /v1/collections/{name}/search` returns the `k` live entities nearest to a query vector, optionally restricted by a filter.

```bash
curl -s $AV/v1/collections/notes/search -H 'content-type: application/json' -d '{
  "vector": [1, 0.05, 0, 0],
  "k": 2,
  "filter": { "eq": { "topic": "pets" } },
  "include": ["attributes", "document"],
  "explain": true
}'
```

```json
{
  "as_of_block": 15,
  "sync_state": "live",
  "open_collection": false,
  "results": [
    { "key": "0xc59528cf…c1b6", "id": "cat", "distance": 0.0012470364, "score": 0.9987529636,
      "owner": "0xf39fd6e5…2266", "creator": "0xf39fd6e5…2266", "expires_at_block": 302415,
      "attributes": { "stars": { "type": "i32", "value": 5 }, "topic": { "type": "str", "value": "pets" } },
      "document": "Cats purr." },
    { "key": "0xc69c75d5…1615", "id": "dog", "distance": 0.0018414261, "score": 0.9981585739, "...": "..." }
  ],
  "plan": { "strategy": "exact_flat", "exact": true, "candidates": 2, "selectivity": 0.67,
            "kernel": "x86-64-v4", "elapsed_ms": 0.21 }
}
```

## Request

| Field | Default | Meaning |
| :--- | :--- | :--- |
| `vector` or `vector_bytes` | | The query, exactly one of the two ([Vectors](vectors.md)) |
| `k` | 10 | 1–1000 results |
| `offset` | 0 | Skip the first `offset` results (`offset + k ≤ 1000`) |
| `operator` | the index's | `<->` `<#>` `<=>` `<+>` `<~>` `<%>`, valid for the column type. A different operator than the index runs an exact scan (like pgvector's sequential scan) |
| `filter` or `where` | none | Attribute filter, at most one of the two ([Filters](filters.md)) |
| `exact` | `false` | Force an exact scan (100% recall) |
| `ef_search` | 40 | HNSW search breadth, 1–1000 (pgvector `hnsw.ef_search`). Raised automatically to at least `offset + k` |
| `iterative_scan` | `relaxed_order` | `off`, `relaxed_order`, `strict_order` (pgvector `hnsw.iterative_scan`), used by filtered HNSW searches |
| `max_scan_tuples` | 20000 | 1–1,000,000. Cap on graph nodes an iterative scan visits |
| `max_distance` | none | Drop hits with `distance > max_distance` |
| `min_score` | none | Drop hits with `score < min_score` |
| `include` | `["attributes"]` | Any of `attributes`, `document`, `vector`. Giving the list replaces the default |
| `consistency` | none | `{"min_block": n, "timeout_ms": 4000}` ([Consistency](consistency.md)) |
| `explain` | `false` | Add the `plan` object |

## Response

| Field | Meaning |
| :--- | :--- |
| `as_of_block` | Block the results reflect |
| `sync_state` | `live`, `catch_up`, ... Results are still valid in other states, possibly older |
| `open_collection` | `true` for open-membership collections (anyone could have written them) |
| `results[]` | `key`, `id`, `distance`, `score`, `owner`, `creator`, `expires_at_block`, plus `attributes` / `document` / `vector_bytes` per `include` |
| `plan` | With `explain`: `strategy`, `exact`, `candidates`, `selectivity`, `kernel`, `elapsed_ms` |

## Distance and score

| Operator | `distance` | `score` |
| :---: | :--- | :--- |
| `<->` | L2 distance | `−distance` |
| `<#>` | negative inner product | `−distance` (the inner product) |
| `<=>` | cosine distance (1 − cos) | `1 − distance` (the cosine similarity) |
| `<+>` | L1 distance | `−distance` |
| `<~>` | Hamming distance | `−distance` |
| `<%>` | Jaccard distance | `−distance` |

Results are sorted by ascending distance; ties are broken by ascending key. Distances are pgvector's own kernels, within float32 rounding of the CPU's kernel tier.

## Exact and HNSW plans

| `plan.strategy` | When |
| :--- | :--- |
| `exact_flat` | Index method `none`; `exact: true`; a different operator than the index; small candidate sets (≤ 2048) or very selective filters; or as a fallback |
| `hnsw` | HNSW graph search, no filter |
| `hnsw_iterative` | HNSW with a filter, using pgvector 0.8's iterative scan and a filter recheck |

Guarantees:

1. Only entities live at `as_of_block` appear.
2. If at least `offset + k` live entities match the filter, exactly `k` hits come back: when a graph walk finds fewer, the daemon falls back to an exact scan.
3. With `exact` (or `plan.exact: true`), results equal brute-force pgvector results.
4. HNSW plans are approximate. The end-to-end tests hold recall@10 ≥ 0.9 at the defaults; raise `ef_search` for more.

See [Search and HNSW](../architecture/search.md) for how the planner decides.

## Errors

| Situation | Error |
| :--- | :--- |
| Both or neither of `vector` and `vector_bytes` | `invalid_argument` (`field: /vector`) |
| Both `filter` and `where` | `invalid_argument` (`field: /filter`) |
| Wrong dimension | `dimension_mismatch` |
| Operator not valid for the column | `invalid_argument` (`field: /operator`) |
| `ef_search`, `iterative_scan`, `max_scan_tuples` out of range | `invalid_argument` with `field` |
| Collection still bootstrapping, no `consistency` | `503 not_ready` with `Retry-After` |
