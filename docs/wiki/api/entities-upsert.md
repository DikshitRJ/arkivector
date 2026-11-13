# Upsert entities

`POST /v1/collections/{name}/entities` writes entities to the Arkiv chain through the daemon's wallet. Needs a signer (`writes_enabled: true`, otherwise `403 write_disabled`) and the `write` scope when auth is on.

```bash
curl -s $AV/v1/collections/notes/entities -H 'content-type: application/json' -d '{
  "entities": [
    { "id": "cat", "vector": [1, 0, 0, 0],     "document": "Cats purr.",      "attributes": { "topic": "pets", "stars": 5 } },
    { "id": "dog", "vector": [0.9, 0.1, 0, 0], "document": "Dogs bark.",      "attributes": { "topic": "pets", "stars": 4 } },
    { "id": "car", "vector": [0, 0, 1, 0],     "document": "Cars need fuel.", "attributes": { "topic": "vehicles", "stars": 3 },
      "ttl": { "days": 30 } }
  ],
  "wait": "indexed"
}'
```

```json
{
  "results": [
    { "id": "cat", "key": "0xc59528cfc47a6a19b62d660a46e4ff652f0af203d5b8b40b60ff26fb6a18c1b6", "op": "create" },
    { "id": "dog", "key": "0xc69c75d5a92152b252c5cf980d38e7e1662e313ea51e0fb1f74b049182021615", "op": "create" },
    { "id": "car", "key": "0x34754cd9837d7c7e1e6617d99d769c9f1307a345d9ba4e2d3ab68fb091e0fe0f", "op": "create" }
  ],
  "tx_hashes": ["0x6ab78337f035eaf4453bca184486f9a213ce55cfda4e44ef2f7a829ebd1d5dae"],
  "block_number": 15
}
```

## Request

| Field | Default | Meaning |
| :--- | :--- | :--- |
| `entities` | | 1–1000 entities (below) |
| `wait` | `receipt` | `none`: return after broadcast (`202`, keys predicted from the wallet's on-chain `entityNonce`, `block_number: null`). `receipt`: after the transaction is mined; keys come from the `EntityCreated` logs. `indexed`: after it is mined **and** searchable on this daemon |
| `timeout_ms` | 60000 | How long to wait for receipts or indexing |

Entity fields:

| Field | Meaning |
| :--- | :--- |
| `id` | Your identifier (1–128 bytes). Used for upsert-by-id. Must not look like a key (`0x` + 64 hex) |
| `vector` or `vector_bytes` | The embedding, as a JSON array / pgvector literal or base64 VectorBytes ([Vectors](vectors.md)). Must match the column type and dimension |
| `document` | Optional UTF-8 text stored with the vector (`av.doc`) |
| `attributes` | Name → value. Plain values are typed from the collection's `attributes`; or pass `{"type": "dec", "value": "4.5"}`. Up to 29 attributes; strings ≤ 128 bytes |
| `ttl` | `{"days": n}`, `{"seconds": n}` (even), `{"blocks": n}` or `{"expires_at_block": n}`; defaults to the collection's `default_ttl` |

The vector, document and `bytes` attributes of one entity share a 128 KiB budget.

## Response

| Field | Meaning |
| :--- | :--- |
| `results` | One item per input, in order: `id`, `key`, `op` |
| `op` | `create` (new), `patch` (changed attributes or vector), `noop` (nothing changed, nothing sent) |
| `tx_hashes` | Transactions sent |
| `block_number` | Highest block the transactions landed in. Use it as `min_block` on the next read ([Consistency](consistency.md)) |

## Upsert semantics

- If a live entity with this `id` exists **and is owned by the daemon's wallet**, only the changed attributes are patched. If it is owned by someone else: `409 not_owner`, nothing is sent.
- `ttl` on a patch also extends the expiry in the same transaction; without `ttl` the expiry is unchanged.
- Entities are packed into as few transactions as fit. Atomicity is per transaction: if one of several transactions reverts, the earlier ones stay applied, the response lists only what succeeded, and the error is `tx_reverted` with `revert_error`.

## Idempotency

Send `Idempotency-Key: <uuid>`. Within 24 hours, the same key with the same body returns the stored response without sending anything; the same key with a different body is `409 idempotency_conflict`. The SDKs send one automatically, which makes retries safe.

## Errors

`write_disabled` (403), `not_owner`, `tx_reverted`, `idempotency_conflict` (409), `invalid_argument`, `invalid_vector`, `dimension_mismatch` (422), `payload_too_large` (413). See [Errors](errors.md).
