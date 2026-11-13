# Collections

A collection binds a pgvector-style column and index to an Arkiv schema tag and a set of writers. The daemon syncs every matching entity from the chain and indexes it. Creating, following and deleting collections needs the `admin` scope when auth is on.

## Create: `POST /v1/collections`

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

`201 Created`:

```json
{
  "name": "notes",
  "schema_tag": "arkivector.v1.notes",
  "column": { "type": "halfvec", "dimensions": 4 },
  "index": { "method": "hnsw", "opclass": "halfvec_cosine_ops", "with": { "m": 16, "ef_construction": 64 } },
  "writers": ["0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266"],
  "membership": "creator",
  "attributes": { "stars": "i32", "topic": "str" },
  "default_ttl": { "days": 7.0 },
  "model": null, "manifest_key": null, "open": false, "mode": "full",
  "stats": { "live_entities": 0, "segments": 2, "quarantined": 0, "tombstones": 0 },
  "sync": { "name": "notes", "state": "init", "as_of_block": 0, "head_block": 0, "lag_blocks": 0, "last_error": null }
}
```

### Request fields

| Field | Required | Default | Rule |
| :--- | :---: | :--- | :--- |
| `name` | yes | | `^[a-z][a-z0-9._-]{0,62}$`, unique per daemon |
| `column.type` | yes | | `vector`, `halfvec`, `sparsevec` or `bit` |
| `column.dimensions` | yes | | Within the type's limits ([Vectors](vectors.md)) |
| `index.method` | yes | | `hnsw`, or `none` (always exact) |
| `index.opclass` | yes | | Must match the column type (table below) |
| `index.with.m` | | 16 | 2–100 |
| `index.with.ef_construction` | | 64 | 4–1000 and ≥ 2·m |
| `schema_tag` | | `arkivector.v1.<name>` | Value of `arkiv.schema` on chain (≤ 128 bytes) |
| `writers` | | `[]` | Addresses whose entities belong to the collection. Required unless `membership` is `open` |
| `membership` | | `creator` | `creator` (`$creator ∈ writers`), `owner` (`$owner ∈ writers`), `open` (anyone; writers must be empty) |
| `attributes` | | `{}` | Attribute name → type (`bool i32 u64 u256 dec str addr key bytes`). Types filter values; undeclared attributes are still stored and filterable with typed values |
| `default_ttl` | | `null` | Lifetime for new entities without a `ttl`. If null, every write must carry one |
| `model` | | `null` | Free text, e.g. `openai/text-embedding-3-small`, for your own bookkeeping |
| `mode` | | `full` | `full` mirrors all attributes; `lite` mirrors only vectors and system fields |
| `publish_manifest` | | `false` | Publish the definition as an on-chain manifest entity others can [follow](#follow-post-v1collectionsfollow) |

### Operator classes

| Column | Opclasses (operator) |
| :--- | :--- |
| `vector` | `vector_l2_ops` (`<->`), `vector_ip_ops` (`<#>`), `vector_cosine_ops` (`<=>`), `vector_l1_ops` (`<+>`) |
| `halfvec` | `halfvec_l2_ops`, `halfvec_ip_ops`, `halfvec_cosine_ops`, `halfvec_l1_ops` |
| `sparsevec` | `sparsevec_l2_ops`, `sparsevec_ip_ops`, `sparsevec_cosine_ops`, `sparsevec_l1_ops` |
| `bit` | `bit_hamming_ops` (`<~>`), `bit_jaccard_ops` (`<%>`) |

The opclass sets the default search operator; a search can use any operator valid for the column type (with an exact scan).

Errors: `collection_exists` (409), `invalid_argument` (422).

## List: `GET /v1/collections`

```bash
curl -s $AV/v1/collections
```

```json
{"collections": [ { "name": "demo.docs", "...": "..." }, { "name": "notes", "...": "..." } ]}
```

Sorted by name; each item has the same shape as the create response.

## Get: `GET /v1/collections/{name}`

```bash
curl -s $AV/v1/collections/notes
```

Returns one collection with current `stats` (`live_entities`, `segments`, `quarantined`, `tombstones`) and `sync` state. Errors: `collection_not_found` (404).

## Delete: `DELETE /v1/collections/{name}`

```bash
curl -s -X DELETE $AV/v1/collections/notes     # 204 No Content
```

Stops syncing and deletes the local index. **Data on chain is not touched**; recreate the collection to index it again.

## Follow: `POST /v1/collections:follow`

Index a collection someone else published, from its on-chain manifest:

```bash
curl -s "$AV/v1/collections:follow" -H 'content-type: application/json' \
  -d '{"manifest_key": "0x<64 hex>", "name": "their-notes"}'
```

Or follow a definition directly: `{"definition": { ...same fields as create... }}`. Returns `201` and the collection. Errors: `entity_not_found` (manifest missing or expired), `invalid_argument`, `collection_exists`.

## Membership and trust

Only entities that carry the schema tag **and** pass the membership rule are indexed, so a stranger cannot inject vectors into your collection by reusing your tag. `open` collections accept anyone; their search responses set `open_collection: true` as a warning.
