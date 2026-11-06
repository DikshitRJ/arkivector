# Collections

A **collection** is a pgvector-style table + index definition bound to an Arkiv schema tag and a writer set. The daemon syncs every entity that matches it from Arkiv and indexes it locally.

Equivalent pgvector DDL:

```sql
CREATE TABLE demo_docs (embedding halfvec(8), category text, score numeric, published bool, rank int);
CREATE INDEX ON demo_docs USING hnsw (embedding halfvec_cosine_ops) WITH (m = 16, ef_construction = 64);
```

## 1. Definition fields (`CollectionCreate`)

| Field | Required | Default | Rule |
| :--- | :---: | :--- | :--- |
| `name` | ✅ | — | Local name, unique per daemon. See [conventions.md §6](conventions.md#6-names). |
| `schema_tag` | | `arkivector.v1.<name>` | Value of the `arkiv.schema` attribute on chain (≤ 128 bytes). |
| `column.type` | ✅ | — | `vector` \| `halfvec` \| `bit` \| `sparsevec` |
| `column.dimensions` | ✅ | — | Within the type limits ([vectors.md](vectors.md)). |
| `index.method` | ✅ | — | `hnsw`, or `none` (exact scan only). |
| `index.opclass` | ✅ | — | MUST match `column.type`: `vector_*_ops` for vector, `halfvec_*_ops` for halfvec, `sparsevec_*_ops` for sparsevec, `bit_hamming_ops`/`bit_jaccard_ops` for bit. Mismatch → `invalid_argument`. |
| `index.with.m` | | 16 | 2–100 (pgvector range) |
| `index.with.ef_construction` | | 64 | 4–1000, and ≥ 2·m (pgvector rule) |
| `writers` | | `[]` | Allowlisted addresses. Required unless `membership = "open"`. |
| `membership` | | `creator` | `creator`: `$creator ∈ writers`. `owner`: `$owner ∈ writers`. `open`: any writer (writers must be empty). |
| `attributes` | | `{}` | Map name → type (`bool i32 u64 u256 dec str addr key bytes`). Types shorthand values and JSON filters. Undeclared attributes are still mirrored and filterable with typed values. |
| `default_ttl` | | `null` | Used by managed upserts without `ttl`. If null, the upsert MUST carry `ttl`. |
| `publish_manifest` | | false | Publish an on-chain manifest entity (needs signer and capability `collections.manifest`). |
| `model` | | null | Free text, e.g. `openai/text-embedding-3-small`. Informational, but SDK adapters SHOULD warn on mismatch. |
| `mode` | | `full` | `full` mirrors all attributes. `lite` mirrors only vectors and system fields (filters on other attributes go to Arkiv, which is slower). |

## 2. Endpoints

### `POST /v1/collections` (scope admin)
Creates the definition and starts sync (state `bootstrap`). Returns `201` + `Collection`.
Errors: `collection_exists`, `invalid_argument`, `write_disabled` (publish_manifest without signer), `not_implemented` (publish_manifest before D5).

### `POST /v1/collections:follow` (scope admin)
Body is either `{ "manifest_key": "0x…", "name"?: "local-name" }` (the daemon reads the manifest entity from Arkiv) or `{ "definition": CollectionCreate }`. Returns `201` + `Collection`.
Errors: `entity_not_found` (manifest missing or expired), `invalid_argument` (malformed manifest), `collection_exists`.

### `GET /v1/collections` (scope read)
`{ "collections": [Collection, …] }` sorted by name.

### `GET /v1/collections/{name}` (scope read)
`Collection`. Errors: `collection_not_found`.

### `DELETE /v1/collections/{name}` (scope admin)
Stops sync and deletes local index files. **On-chain data is not touched.** Returns `204`.

## 3. `Collection` response

All `CollectionCreate` fields (with defaults resolved), plus:

| Field | Meaning |
| :--- | :--- |
| `manifest_key` | Key of the on-chain manifest, or null |
| `open` | True when `membership = open` (poisoning risk; SDKs SHOULD surface a warning) |
| `stats.live_entities` | Live entities at `sync.as_of_block` |
| `stats.segments` | Index segments |
| `stats.quarantined` | Matching entities rejected for invalid `av.vec` |
| `stats.tombstones` | Dead entries awaiting compaction |
| `sync` | `SyncStatus` ([consistency.md](consistency.md)) |

## 4. On-chain layout of member entities (informative)

SDKs that write **directly** to Arkiv (not through the daemon) MUST produce this layout, or the daemon won't index the entity:

| Attribute | Type id | Content |
| :--- | :--- | :--- |
| `arkiv.schema` | `0x88` (indexed str) | `schema_tag` |
| `av.vec` | `0x07` (bytes) | VectorBytes of exactly `column.type` / `column.dimensions` |
| `av.id` | `0x88` (indexed str) | External id |
| `av.doc` | `0x07` (bytes) | Optional UTF-8 document |
| user attributes | any, ≤ 29 | Metadata. Index on chain (`| 0x80`) only if other Arkiv clients need to filter on them |

The entity must be created by an address in `writers` (`membership = creator`).
