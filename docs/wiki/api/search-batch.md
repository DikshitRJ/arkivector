# Batch search

`POST /v1/collections/{name}/search:batch` runs up to 64 searches against **the same snapshot** (one `as_of_block`).

```bash
curl -s "$AV/v1/collections/notes/search:batch" -H 'content-type: application/json' -d '{
  "searches": [
    { "vector": [1, 0, 0, 0], "k": 1 },
    { "vector": [0, 0, 1, 0], "k": 1, "where": "stars >= 3" }
  ]
}'
```

```json
{
  "as_of_block": 15,
  "sync_state": "live",
  "responses": [
    { "results": [ { "key": "0xc595…c1b6", "id": "cat", "distance": 0.0, "score": 1.0, "...": "..." } ] },
    { "results": [ { "key": "0x3475…fe0f", "id": "car", "distance": 0.0, "score": 1.0, "...": "..." } ] }
  ]
}
```

| Field | Meaning |
| :--- | :--- |
| `searches` | 1–64 [search requests](search.md#request). A per-search `consistency` is ignored |
| `consistency` | Applies to the whole batch ([Consistency](consistency.md)) |

`responses` are in request order, each with `results` (and `plan` with `explain`). Batching saves round trips and guarantees that all answers come from one block.
