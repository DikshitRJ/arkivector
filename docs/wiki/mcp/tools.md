# Tools

Every tool result has `content` (one text block), `isError`, and on success `structuredContent` holding the response of the matching REST endpoint. Errors carry the REST [problem object](../api/errors.md) as text, e.g. `{"code": "dimension_mismatch", ...}`.

## `arkivector_list_collections`

No arguments. Runs [`GET /v1/collections`](../api/collections.md#list-get-v1collections).

Text:

```
2 collection(s)
- demo.docs: halfvec(8), index hnsw halfvec_cosine_ops, 200 live entities, sync live
- notes: halfvec(4), index hnsw halfvec_cosine_ops, 3 live entities, sync live
```

`structuredContent`: `{"collections": [Collection, ...]}` with column, index, declared attributes, stats and sync state.

## `arkivector_search`

Runs [`POST /v1/collections/{collection}/search`](../api/search.md).

| Argument | Type | Meaning |
| :--- | :--- | :--- |
| `collection` | string, required | Collection name |
| `vector` | number[] | Query embedding with the collection's dimension |
| `text` | string | Natural-language query; needs an embedder hook (not configured by default) |
| `k` | integer 1–50 | Results (default 10) |
| `where` | string | Arkiv filter, e.g. `"topic = str('pets')"` |
| `filter` | object | JSON filter, e.g. `{"gte": {"stars": 4}}` |
| `include_document` | boolean | Return documents |

```json
{"name": "arkivector_search",
 "arguments": {"collection": "notes", "vector": [1, 0.05, 0, 0], "k": 2,
               "where": "topic = str('pets')", "include_document": true}}
```

`structuredContent`:

```json
{"as_of_block": 3, "sync_state": "live", "open_collection": false,
 "results": [
   {"key": "0xc595…", "id": "cat", "distance": 0.0012, "score": 0.9988, "expires_at_block": 302403,
    "attributes": {"stars": {"type": "i32", "value": 5}, "topic": {"type": "str", "value": "pets"}},
    "document": "Cats purr."},
   {"key": "0xc69c…", "id": "dog", "distance": 0.0018, "score": 0.9982, "...": "..."}]}
```

## `arkivector_get`

Runs [`GET /v1/collections/{collection}/entities/{ref}`](../api/entities-get.md).

| Argument | Type | Meaning |
| :--- | :--- | :--- |
| `collection` | string, required | |
| `ref` | string, required | Entity id or key |
| `include_document` | boolean | Return the document |

`structuredContent`: `{"as_of_block": n, "entity": {...}}`.

## `arkivector_count`

Runs [`POST /v1/collections/{collection}/count`](../api/count.md).

| Argument | Type | Meaning |
| :--- | :--- | :--- |
| `collection` | string, required | |
| `where` / `filter` | string / object | Optional filter |

`structuredContent`: `{"as_of_block": n, "count": 2}`.

## `arkivector_upsert`

Listed only when the daemon has a signer. Runs [`POST /v1/collections/{collection}/entities`](../api/entities-upsert.md) and needs the `write` scope.

| Argument | Type | Meaning |
| :--- | :--- | :--- |
| `collection` | string, required | |
| `entities` | array, required, 1–100 | `{"id", "vector", "document", "attributes", "ttl"}`; an existing id is updated |
| `wait` | string | `indexed` (default: returns once searchable), `receipt`, or `none` |

```json
{"name": "arkivector_upsert",
 "arguments": {"collection": "notes",
               "entities": [{"id": "memory-42", "vector": [0.2, 0.1, 0.9, 0.0],
                             "document": "The user prefers refunds by bank transfer.",
                             "attributes": {"topic": "preferences"}, "ttl": {"days": 30}}]}}
```

`structuredContent`: `{"results": [{"id", "key", "op"}], "tx_hashes": [...], "block_number": n}`. Every call is an on-chain transaction that spends gas from the daemon's wallet.

## Annotations

Read tools carry `readOnlyHint: true`; `arkivector_upsert` carries `readOnlyHint: false`, `destructiveHint: false`, `idempotentHint: true`, so clients can auto-approve reads and ask before writes.
