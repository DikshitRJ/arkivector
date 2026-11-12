# Searching

```python
res = client.search(
    "notes", [1, 0.05, 0, 0],
    k=10, offset=0,
    filter=None, where=None,          # attribute filter (one of the two)
    operator=None,                    # "<->", "<#>", "<=>", "<+>", "<~>", "<%>"
    exact=False,                      # True: exact scan, 100% recall
    ef_search=None,                   # HNSW breadth, 1..1000 (default 40)
    iterative_scan=None,              # "off" | "relaxed_order" | "strict_order"
    max_scan_tuples=None,
    max_distance=None, min_score=None,
    include=("attributes",),          # add "document", "vector"
    min_block=None, timeout_ms=None,  # read-your-writes
    explain=False,                    # fills res.plan
)
```

The query vector can be a list of floats, a pgvector literal (`"[1,0,0,0]"`), or a [typed vector](types.md#vector-types) (`Vector`, `HalfVector`, `SparseVector`, `Bit`).

## Results

```python
res.as_of_block, res.sync_state, res.open_collection, res.plan
for hit in res.hits:
    hit.id, hit.key, hit.distance, hit.score
    hit.owner, hit.creator, hit.expires_at_block
    hit.attributes   # {"topic": TypedValue("str", "pets"), ...}
    hit.document     # with include=["document", ...]
    hit.vector       # with include=["vector", ...], decoded
```

`distance` is pgvector's distance for the operator; `score` is `1 − distance` for cosine and `−distance` otherwise. See [Search API](../api/search.md#distance-and-score).

## Exact or HNSW

Collections with an HNSW index answer large queries from the graph and small or very selective ones exactly. `explain=True` shows which (`res.plan["strategy"]`: `exact_flat`, `hnsw`, `hnsw_iterative`). Raise `ef_search` for better recall, or pass `exact=True` for 100%.

```python
res = client.search("notes", q, k=20, filter=F.eq("topic", "pets"), ef_search=200, iterative_scan="strict_order", explain=True)
```

## Batch search

Up to 64 searches against one snapshot:

```python
batch = client.search_batch("notes", [
    {"vector": [1, 0, 0, 0], "k": 1},
    {"vector": [0, 0, 1, 0], "k": 1, "where": "stars >= 3"},
])
[r.hits[0].id for r in batch.results]        # ['cat', 'car']
batch.as_of_block
```
