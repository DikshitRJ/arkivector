# REST API with curl

Everything Arkivector does is a JSON-over-HTTP call to the daemon, so you can use it from any language, or from a shell, without an SDK. This guide walks through the whole API with `curl`. The responses shown were captured from a real daemon.

The formal contract is [`spec/openapi.yaml`](https://github.com/DikshitRJ/arkivector/blob/main/spec/openapi.yaml). Field-by-field rules are in [`docs/api/`](https://github.com/DikshitRJ/arkivector/tree/main/docs/api).

## Basics

| What | How |
| :--- | :--- |
| Base URL | `http://127.0.0.1:8080` (wherever `arkivectord` listens) |
| Request bodies | `Content-Type: application/json` |
| Errors | `application/problem+json` with a stable `code` ([error codes](#errors)) |
| Auth | Off by default. With an API-key file configured, send `Authorization: Bearer <key>` |
| Consistency | Every data response carries `as_of_block`, the chain block it reflects |

```bash
export AV=http://127.0.0.1:8080
```

## Is it up?

```bash
curl -s $AV/healthz          # {"status":"ok"}
curl -s $AV/readyz           # {"ready":true,"collections":{"notes":"live"}}
curl -s $AV/v1/version
```

`/v1/version` tells you what this daemon can do:

```json
{ "daemon_version": "1.1.0", "api_version": "1.0.0", "pgvector_version": "0.8.7",
  "kernel_tier": "x86-64-v4", "chain_id": 7794095, "writes_enabled": true,
  "capabilities": ["collections.read", "search.exact", "search.hnsw", "entities.write", "mcp", "..."] }
```

`writes_enabled` is `true` only when the daemon has a signing key (`ARKIV_PRIVATE_KEY`). Without one, everything below that writes returns `403 write_disabled`, and reads still work.

## Collections

A collection is a pgvector-style column and index bound to an Arkiv schema tag. The daemon indexes every Arkiv entity that carries that tag and was created by one of the collection's `writers`.

### Create

```bash
curl -s $AV/v1/collections -H 'content-type: application/json' -d '{
  "name": "notes",
  "column": { "type": "halfvec", "dimensions": 4 },
  "index": { "method": "hnsw", "opclass": "halfvec_cosine_ops" },
  "writers": ["0xYourDaemonWalletAddress"],
  "attributes": { "topic": "str", "stars": "i32" },
  "default_ttl": { "days": 7 }
}'
```

```json
{ "name": "notes", "schema_tag": "arkivector.v1.notes",
  "column": { "type": "halfvec", "dimensions": 4 },
  "index": { "method": "hnsw", "opclass": "halfvec_cosine_ops", "with": { "m": 16, "ef_construction": 64 } },
  "writers": ["0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266"], "membership": "creator",
  "attributes": { "stars": "i32", "topic": "str" }, "default_ttl": { "days": 7.0 },
  "stats": { "live_entities": 0, "segments": 2, "quarantined": 0, "tombstones": 0 },
  "sync": { "name": "notes", "state": "init", "as_of_block": 0, "head_block": 0, "lag_blocks": 0, "last_error": null } }
```

| Field | Notes |
| :--- | :--- |
| `column.type` | `vector` (float32), `halfvec` (float16, half the gas, recommended), `sparsevec`, `bit` |
| `column.dimensions` | Your embedding model's dimension (e.g. 768, 1536) |
| `index.method` | `hnsw` (approximate, fast on large collections) or `none` (always exact) |
| `index.opclass` | Must match the column type: `halfvec_cosine_ops`, `vector_l2_ops`, `vector_ip_ops`, `sparsevec_l1_ops`, `bit_hamming_ops`, ... |
| `index.with` | `m` (2–100, default 16), `ef_construction` (4–1000, default 64) |
| `writers` | Addresses whose entities belong to the collection. **Put your daemon wallet's address here**, or your writes land on chain and never show up in search |
| `attributes` | Declared attribute types, used to type filter values |
| `default_ttl` | Lifetime for new entities: `{"days": 7}`, `{"seconds": 3600}`, `{"blocks": 1000}` or `{"expires_at_block": n}` |

### List, get, delete

```bash
curl -s $AV/v1/collections                    # {"collections":[...]}
curl -s $AV/v1/collections/notes              # one collection, with stats and sync state
curl -s -X DELETE $AV/v1/collections/notes    # stops syncing; on-chain data is untouched
```

### Follow a collection someone else published

```bash
curl -s "$AV/v1/collections:follow" -H 'content-type: application/json' \
  -d '{"manifest_key":"0x<64 hex>","name":"their-notes"}'
```

## Writing entities

An entity is one vector plus a document and typed attributes. The daemon builds the Arkiv transaction, signs it, sends it, and (with `wait: "indexed"`) returns only after the entity is searchable.

```bash
curl -s $AV/v1/collections/notes/entities -H 'content-type: application/json' -d '{
  "entities": [
    { "id": "cat", "vector": [1, 0, 0, 0],   "document": "Cats purr.",      "attributes": { "topic": "pets", "stars": 5 } },
    { "id": "dog", "vector": [0.9, 0.1, 0, 0], "document": "Dogs bark.",    "attributes": { "topic": "pets", "stars": 4 } },
    { "id": "car", "vector": [0, 0, 1, 0],   "document": "Cars need fuel.", "attributes": { "topic": "vehicles", "stars": 3 } }
  ],
  "wait": "indexed"
}'
```

```json
{ "results": [
    { "id": "cat", "key": "0xc59528cfc47a6a19b62d660a46e4ff652f0af203d5b8b40b60ff26fb6a18c1b6", "op": "create" },
    { "id": "dog", "key": "0xc69c75d5a92152b252c5cf980d38e7e1662e313ea51e0fb1f74b049182021615", "op": "create" },
    { "id": "car", "key": "0x34754cd9837d7c7e1e6617d99d769c9f1307a345d9ba4e2d3ab68fb091e0fe0f", "op": "create" } ],
  "tx_hashes": ["0x6ab78337f035eaf4453bca184486f9a213ce55cfda4e44ef2f7a829ebd1d5dae"],
  "block_number": 15 }
```

- **Upsert by id.** Sending the same `id` again patches only what changed (`"op": "patch"`), or does nothing (`"op": "noop"`).
- **`wait`**: `none` (return after broadcast, `202`), `receipt` (default: after the transaction is mined), `indexed` (after mined *and* searchable here).
- **`ttl`** per entity overrides the collection's `default_ttl`.
- **Idempotency.** Send `Idempotency-Key: <uuid>` and a retried request is answered from cache instead of being sent twice.

### Extend and delete

```bash
# keep "cat" alive for 30 more days
curl -s "$AV/v1/collections/notes/entities:extend" -H 'content-type: application/json' \
  -d '{"refs":["cat"],"ttl":{"days":30}}'
# {"affected":1,"tx_hashes":["0xa8bd…"],"block_number":17}

curl -s -X DELETE $AV/v1/collections/notes/entities/car
# {"affected":1,"tx_hashes":["0x76fb…"],"block_number":18}

# bulk: by refs, or by filter / where (only entities your wallet owns)
curl -s "$AV/v1/collections/notes/entities:delete" -H 'content-type: application/json' \
  -d '{"where":"stars < 2"}'
```

### Sign with your own wallet

`entities:prepare` returns unsigned transactions instead of sending them, for browser wallets, hardware wallets or a KMS. The daemon needs no key for this.

```bash
curl -s "$AV/v1/collections/notes/entities:prepare" -H 'content-type: application/json' \
  -d '{"from":"0xYourAddress","entities":[{"id":"fish","vector":[0.8,0,0.2,0],"ttl":{"days":7}}]}'
# {"chain_id":7794095,"transactions":[{"to":"0x4400…0044","data":"0x…","value":"0x0","nonce":12,"gas_estimate":…}],"results":[…]}
```

Sign each transaction as EIP-1559, broadcast it, then read with `consistency.min_block` set to its receipt block.

## Searching

```bash
curl -s $AV/v1/collections/notes/search -H 'content-type: application/json' -d '{
  "vector": [1, 0.05, 0, 0],
  "k": 2,
  "filter": { "eq": { "topic": "pets" } },
  "include": ["attributes", "document"]
}'
```

```json
{ "as_of_block": 15, "sync_state": "live", "open_collection": false,
  "results": [
    { "key": "0xc595…c1b6", "id": "cat", "distance": 0.001247, "score": 0.998753,
      "owner": "0xf39f…2266", "creator": "0xf39f…2266", "expires_at_block": 302415,
      "attributes": { "stars": { "type": "i32", "value": 5 }, "topic": { "type": "str", "value": "pets" } },
      "document": "Cats purr." },
    { "key": "0xc69c…1615", "id": "dog", "distance": 0.001841, "score": 0.998159, "...": "..." } ] }
```

| Field | Default | Meaning |
| :--- | :--- | :--- |
| `vector` or `vector_bytes` | | The query: a JSON array, a pgvector literal (`"[1,0,0,0]"`), or base64 VectorBytes |
| `k`, `offset` | 10, 0 | Ranks `offset+1 … offset+k` (`offset + k ≤ 1000`) |
| `filter` or `where` | none | Attribute filter ([below](#filters)) |
| `operator` | the index's | `<->` L2, `<#>` negative inner product, `<=>` cosine, `<+>` L1, `<~>` Hamming, `<%>` Jaccard. A different operator than the index forces an exact scan |
| `exact` | false | Skip HNSW and scan exactly (100% recall) |
| `ef_search` | 40 | HNSW search breadth (1–1000). Higher means better recall and slower search |
| `iterative_scan` | `relaxed_order` | Filtered HNSW searches: `off`, `relaxed_order`, `strict_order` (pgvector 0.8 iterative scans) |
| `max_scan_tuples` | 20000 | Cap on graph nodes visited by an iterative scan |
| `max_distance`, `min_score` | none | Drop hits beyond a threshold |
| `include` | `["attributes"]` | Any of `attributes`, `document`, `vector`. Listing it replaces the default |
| `consistency` | none | `{"min_block": n, "timeout_ms": 4000}`: wait until the daemon has synced block `n` |
| `explain` | false | Add a `plan` object showing how the query ran |

`distance` is pgvector's operator distance; `score` is `1 − distance` for cosine (the cosine similarity) and `−distance` otherwise. Results are sorted by distance, ties broken by key.

### HNSW and exact search

Collections with `"method": "hnsw"` keep a pgvector HNSW graph next to the exact data. The planner decides per query:

- small candidate sets (≤ 2048 entities, or very selective filters) are scanned **exactly**;
- larger ones use the **HNSW graph** (`plan.strategy: "hnsw"`, or `"hnsw_iterative"` with a filter);
- if a graph walk ever finds fewer than `k` matches, the query falls back to an exact scan, so filters never starve results.

Ask for the plan with `"explain": true`:

```json
"plan": { "strategy": "hnsw", "exact": false, "candidates": 3000, "selectivity": 1.0, "kernel": "x86-64-v4", "elapsed_ms": 0.74 }
```

Use `"exact": true` when you need guaranteed 100% recall.

### Batch search

Up to 64 searches against the same snapshot:

```bash
curl -s "$AV/v1/collections/notes/search:batch" -H 'content-type: application/json' -d '{
  "searches": [ { "vector": [1, 0, 0, 0], "k": 1 }, { "vector": [0, 0, 1, 0], "k": 1 } ]
}'
# {"as_of_block":15,"sync_state":"live","responses":[{"results":[{"id":"cat","distance":0.0,…}]},{"results":[{"id":"car",…}]}]}
```

## Reading without a vector

```bash
# one entity, by id or key
curl -s "$AV/v1/collections/notes/entities/cat?include=attributes,document"
# {"as_of_block":15,"entity":{"key":"0xc595…","id":"cat","created_at_block":15,"expires_at_block":302415,
#   "attributes":{"stars":{"type":"i32","value":5},"topic":{"type":"str","value":"pets"}},"document":"Cats purr."}}

# count matching entities
curl -s $AV/v1/collections/notes/count -H 'content-type: application/json' \
  -d "{\"where\":\"topic = str('pets')\"}"
# {"as_of_block":22,"count":2}

# page through everything, in key order
curl -s $AV/v1/collections/notes/scroll -H 'content-type: application/json' -d '{"limit":100}'
# {"as_of_block":15,"entities":[…],"next_cursor":"…"}   pass next_cursor back as "cursor"; null means done
```

## Filters

Two equivalent forms. The JSON tree is easiest to build in code; `where` is Arkiv's query language.

| JSON `filter` | `where` |
| :--- | :--- |
| `{"eq": {"topic": "pets"}}` | `topic = str('pets')` |
| `{"and": [{"gte": {"stars": 4}}, {"eq": {"published": true}}]}` | `stars >= 4 AND published = true` |
| `{"between": {"stars": [2, 4]}}` | `stars BETWEEN 2 AND 4` |
| `{"startswith": {"region": "eu-"}}` | `region STARTSWITH str('eu-')` |
| `{"not": {"exists": "archived"}}` | `NOT EXISTS(archived)` |
| `{"eq": {"$creator": "0x1111…"}}` | `$creator = addr(0x1111…)` |

Operators: `eq ne lt lte gt gte between startswith exists typeof and or not`. System fields: `$owner`, `$creator`, `$expiresAt`, `$key`. In `where`, bare integers are `i32`, `true`/`false` are booleans, and everything else uses a typed constructor: `str('x')`, `dec(4.5)`, `u64(1)`, `addr(0x…)`. Values for undeclared attributes can be typed explicitly in JSON: `{"type": "dec", "value": "4.5"}`.

## Read-your-writes

A write returns the `block_number` it landed in. Pass it to the next read and the daemon waits until it has indexed that block:

```bash
curl -s $AV/v1/collections/notes/search -H 'content-type: application/json' \
  -d '{"vector":[1,0,0,0],"k":3,"consistency":{"min_block":15,"timeout_ms":4000}}'
```

Or write with `"wait": "indexed"` and skip it. If the block is not reached in time you get `504 consistency_timeout` (safe to retry).

## Errors

Errors are RFC 9457 problem documents. Branch on `code`, never on `detail`:

```json
{ "type": "urn:arkivector:error:dimension_mismatch", "title": "Dimension mismatch", "status": 422,
  "detail": "expected 4 dimensions, not 2", "code": "dimension_mismatch", "retryable": false,
  "request_id": "01M78E2K5G7XQR7NJYZ7325RPD", "field": "/vector" }
```

| Code | Status | Typical cause |
| :--- | :---: | :--- |
| `bad_request` | 400 | Malformed JSON or an unknown field (typos are rejected, not ignored) |
| `unauthorized` / `forbidden` | 401 / 403 | Missing API key / key lacks the scope |
| `write_disabled` | 403 | The daemon has no signing key |
| `collection_not_found` / `entity_not_found` | 404 | |
| `collection_exists`, `not_owner`, `tx_reverted` | 409 | |
| `invalid_argument`, `invalid_vector`, `dimension_mismatch`, `invalid_filter` | 422 | `field` or `position` points at the problem |
| `rate_limited` | 429 | Retry after `Retry-After` |
| `not_ready` | 503 | Collection still bootstrapping (retryable) |
| `consistency_timeout` | 504 | `min_block` not reached in time (retryable) |

`retryable: true` means the same request may succeed later.

## Operations

| Endpoint | What |
| :--- | :--- |
| `GET /healthz` | Liveness (no auth) |
| `GET /readyz` | `200` when every collection is live, else `503` (no auth) |
| `GET /v1/version` | Versions and capabilities (no auth) |
| `GET /v1/sync` | Per-collection sync state, lag and last error |
| `GET /metrics` | Prometheus metrics (no auth) |
| `POST /mcp` | MCP server for AI agents (Model Context Protocol over Streamable HTTP) |
| `/qdrant/*` | Qdrant-compatible subset, so Qdrant integrations can point at Arkivector |
