# Concepts

## Collections

A collection is the Arkivector equivalent of a pgvector table plus its index:

```sql
CREATE TABLE notes (embedding halfvec(768), topic text);
CREATE INDEX ON notes USING hnsw (embedding halfvec_cosine_ops) WITH (m = 16, ef_construction = 64);
```

It has a **column** (vector type and dimension), an **index** (`hnsw` or `none`, plus an operator class), a **schema tag** (default `arkivector.v1.<name>`), a list of **writers**, and optional declared **attribute types**. The daemon indexes every Arkiv entity that carries the schema tag and was created by one of the writers. See [Collections API](../api/collections.md).

Membership rules (`membership`): `creator` (default: `$creator ∈ writers`), `owner` (`$owner ∈ writers`), or `open` (anyone; vulnerable to spam, so results flag `open_collection: true`).

## Entities

An entity is one Arkiv entity in a collection: a vector plus an optional document and typed attributes. On chain it is a normal Arkiv entity with reserved attributes (`arkiv.schema`, `av.id`, `av.vec`, `av.doc`), so any Arkiv client can read it and nothing depends on Arkivector staying online. See [VectorBytes](../architecture/vectorbytes.md) for the byte layout.

Entities are addressed by their Arkiv **key** (`0x` + 64 hex) or by the **id** you give them. Writing the same id again updates the entity (upsert).

## Lifetime (TTL)

Arkiv entities expire. Every entity has an `expires_at_block`; after that block it disappears from search, exactly like on Arkiv. Set a collection `default_ttl` or a per-entity `ttl`: `{"days": 7}`, `{"seconds": 3600}` (even), `{"blocks": 1000}` or `{"expires_at_block": n}`, up to about 182 days. Extend with [`entities:extend`](../api/entities-delete-extend.md). Rabdi produces a block every 2 seconds.

## Blocks and consistency

The daemon applies the chain block by block. Every response carries `as_of_block`, the last block it has fully applied. A write returns the `block_number` it landed in; pass it as `min_block` to the next read (or write with `wait: "indexed"`) and you always see your own writes. The SDKs do this automatically with session consistency. See [Consistency](../api/consistency.md).

## Search

A search scores the query vector against the collection with a pgvector distance operator and returns the `k` nearest live entities, optionally restricted by a [filter](../api/filters.md):

| Operator | Distance | Score |
| :---: | :--- | :--- |
| `<->` | L2 (Euclidean) | `−distance` |
| `<#>` | negative inner product | `−distance` (= inner product) |
| `<=>` | cosine distance | `1 − distance` (= cosine similarity) |
| `<+>` | L1 (Manhattan) | `−distance` |
| `<~>` | Hamming (bit) | `−distance` |
| `<%>` | Jaccard (bit) | `−distance` |

Collections with an HNSW index answer large queries from the graph and small or very selective ones with an exact scan. See [Search and HNSW](../architecture/search.md).

## Vector types

| Type | Storage | Dimensions |
| :--- | :--- | :--- |
| `vector` | float32 | up to 16,000 |
| `halfvec` | float16 (recommended: half the bytes) | up to 16,000 |
| `sparsevec` | index/value pairs | up to 10⁹, ≤ 16,000 non-zeros |
| `bit` | packed bits | up to 64,000 |

See [Vectors](../api/vectors.md) for JSON forms and limits.

## Writers and signing

Writes are Arkiv transactions. Either the daemon signs them (a key in `ARKIV_PRIVATE_KEY`, an encrypted keystore, or a remote signer such as Web3Signer), or your own wallet does: [`entities:prepare`](../api/entities-prepare.md) returns unsigned transactions, you sign them (MetaMask, `eth_account`, viem, ethers, a KMS), and [`entities:submit`](../api/entities-submit.md) broadcasts them and waits until they are indexed. Either way the signing address must be in the collection's `writers`. See [Signing writes](signing.md).
