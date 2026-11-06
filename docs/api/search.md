# Search, Batch Search, Scroll, Count

## 1. `POST /v1/collections/{name}/search`

### Request (`SearchRequest`)

| Field | Default | Rule |
| :--- | :--- | :--- |
| `vector` / `vector_bytes` | — | Exactly one ([vectors.md](vectors.md)) |
| `k` | 10 | 1–1000 |
| `offset` | 0 | `offset + k ≤ 1000`. Results are ranks `offset+1 … offset+k` |
| `operator` | opclass operator | One of `<->` `<#>` `<=>` `<+>` `<~>` `<%>`, valid for the column type. If it differs from the index opclass operator, the daemon runs an **exact scan** (as pgvector falls back to a sequential scan) |
| `filter` / `where` | none | At most one ([filters.md](filters.md)) |
| `ef_search` | daemon config (40) | 1–1000. pgvector `hnsw.ef_search` |
| `iterative_scan` | `relaxed_order` | `off` \| `relaxed_order` \| `strict_order`. pgvector `hnsw.iterative_scan`. Used only on filtered approximate plans |
| `max_scan_tuples` | 20000 | pgvector `hnsw.max_scan_tuples` |
| `max_distance` | none | Drop hits with `distance > max_distance` |
| `min_score` | none | Drop hits with `score < min_score` |
| `exact` | false | Force an exact scan (100% recall) |
| `include` | `["attributes"]` | Any of `attributes`, `document`, `vector` |
| `consistency` | none | `{min_block, timeout_ms}` ([consistency.md](consistency.md)) |
| `explain` | false | Include `plan` |

### Response (`SearchResponse`)

```json
{ "as_of_block": 582151, "sync_state": "live", "open_collection": false,
  "results": [ { "key": "0x0b4f…", "id": "doc-040", "distance": 0.1182, "score": 0.8818,
                 "owner": "0x1111…", "creator": "0x1111…", "expires_at_block": 10000000,
                 "attributes": { "category": { "type": "str", "value": "finance" } } } ],
  "plan": { "strategy": "exact_flat", "candidates": 46, "selectivity": 0.23, "exact": true,
            "kernel": "halfvec_f16c", "elapsed_ms": 0.21, "segments": [ … ] } }
```

### Guarantees

1. `results` is sorted by ascending `distance`. Ties are broken by ascending `key`.
2. Only entities live at `as_of_block` appear (not deleted, `expires_at_block > as_of_block`, not quarantined).
3. **Filtered completeness:** if at least `offset + k` live entities match the filter, the response contains exactly `k` hits. A filter can never starve the result, because the daemon falls back to an exact scan over the matching set.
4. With `exact: true` (or `plan.exact: true`), results equal brute-force pgvector results: the same distances (within float rounding of the active kernel tier) and the same order.
5. Without `exact`, recall is approximate (target ≥ 0.95 recall@10 at defaults).

### Distance and score

| Operator | `distance` | `score` |
| :---: | :--- | :--- |
| `<=>` | 1 − cos | 1 − distance (= cos) |
| `<#>` | −(u·v) | −distance (= u·v) |
| `<->`, `<+>`, `<~>`, `<%>` | as defined | −distance |

### Plan strategies

| `strategy` | Meaning |
| :--- | :--- |
| `hnsw` | Graph search without a user filter |
| `exact_flat` | Exact scan over the (filtered) candidate set |
| `hnsw_iterative` | Graph search with pgvector iterative scan and filter recheck |
| `mixed` | Segments used different strategies |
| `remote_prefilter` / `remote_postfilter` | Lite mode, with filters evaluated on Arkiv |

`plan` is diagnostic. SDKs expose it as an opaque or loosely typed object and MUST NOT branch on it.

## 2. `POST /v1/collections/{name}/search:batch`

`{ "searches": [SearchRequest × ≤ 64], "consistency"?: {…} }`. Every search runs against **the same snapshot**. Per-search `consistency` is ignored in a batch, and only the top-level one applies. Response: `{ "as_of_block", "sync_state", "responses": [ { "results", "plan"? } ] }` in request order. If one search is invalid, the whole batch fails with the error of the first invalid search, and `field` points into `/searches/<i>/…`.

## 3. `POST /v1/collections/{name}/scroll`

Enumerates live entities matching a filter, in ascending **key** order.

- Request: `filter` | `where` (optional), `limit` (1–1000, default 100), `cursor` (from the previous page), `include`, `consistency`.
- Response: `{ "as_of_block", "entities": [Entity], "next_cursor": string | null }`. `null` means the end.
- Cursors are opaque (SDKs MUST NOT parse them). They are valid for 1 h. Each page reflects the snapshot at its own `as_of_block`. Because ordering is by immutable key, an entity is never returned twice and an entity that exists for the whole scroll is never skipped.

## 4. `POST /v1/collections/{name}/count`

`{ "filter" | "where" (optional), "consistency"? }` → `{ "as_of_block", "count" }`. Exact count of live matching entities.

## 5. Errors specific to search

| Situation | Error |
| :--- | :--- |
| Both `vector` and `vector_bytes`, or neither | `invalid_argument` (`field: "/vector"`) |
| Both `filter` and `where` | `invalid_argument` (`field: "/filter"`) |
| Wrong dimensions | `dimension_mismatch` |
| Operator invalid for the column type | `invalid_argument` (`field: "/operator"`) |
| Collection still bootstrapping, without `consistency` | `503 not_ready` (with `Retry-After`) |
