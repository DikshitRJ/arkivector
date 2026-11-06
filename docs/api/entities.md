# Entities

An **entity** is one Arkiv entity that belongs to a collection: one vector plus metadata. It is addressed by its Arkiv **key** (`0x` + 64 hex) or by its **external id** (`av.id`).

## 1. `Entity` object

| Field | Type | Meaning |
| :--- | :--- | :--- |
| `key` | EntityKey | Arkiv `$key` |
| `id` | string \| null | `av.id` (null if the writer omitted it) |
| `owner`, `creator` | Address | `$owner`, `$creator` |
| `created_at_block`, `updated_at_block`, `expires_at_block` | integer | Arkiv `$createdAt`, `$updatedAt`, `$expiresAt` |
| `attributes` | map name → TypedValue | User attributes only (`arkiv.schema` and `av.*` are never included) |
| `document` | string \| null | `av.doc` decoded as UTF-8. Present only when `include` has `document` |
| `vector_bytes` | base64 | `av.vec`. Present only when `include` has `vector` |

## 2. Point lookup: `GET /v1/collections/{name}/entities/{ref}`

- `ref` is a key, or a percent-encoded id.
- Query parameters: `include` (comma list, default `attributes`), `verify` (bool), `min_block` + `timeout_ms` ([consistency.md](consistency.md)).
- Response `{ "as_of_block": n, "entity": Entity, "verified"?: bool }`.
- Only **live** entities at `as_of_block` are returned. Expired, deleted, or quarantined → `404 entity_not_found`.
- If two live entities share an id (possible with direct writers), the one with the highest `updated_at_block` wins (ties broken by smallest key).

## 3. Managed upsert: `POST /v1/collections/{name}/entities`

Requires a configured signer (`writes_enabled: true`, capability `entities.write`). Otherwise `403 write_disabled`.

Request:

```json
{
  "entities": [
    { "id": "doc-900", "vector": [0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8],
      "document": "A new finance document.",
      "attributes": { "category": "finance", "score": { "type": "dec", "value": "4.5" } },
      "ttl": { "days": 30 } }
  ],
  "wait": "receipt",
  "timeout_ms": 60000
}
```

Semantics:
1. Each entity is validated (vector, attribute names/types/sizes, 128 KiB bytes budget).
2. **Upsert by id.** If a live entity with this `id` exists **and is owned by the signer**, the daemon sends a `PATCH` with only the changed attributes (`op: "patch"`). If nothing changed, it does nothing (`op: "noop"`). If no entity exists, it sends a `CREATE` (`op: "create"`). If one exists but is owned by someone else, the request fails with `409 not_owner` and nothing is sent.
3. `ttl` (or collection `default_ttl`) applies on create. On patch, `ttl` triggers an `EXTEND_EXPIRY` in the same transaction. Without `ttl`, a patch leaves the expiry unchanged.
4. Ops are packed into as few `execute(ops)` transactions as the daemon's tx size limit allows. **The request is atomic per transaction, not per request.** If transaction 2 of 3 reverts, transaction 1 stays applied. The response then lists only what succeeded, and the error is reported as `tx_reverted` with `revert_error`.
5. `wait`:
   - `none` → `202`, transactions broadcast, `block_number: null`, keys are **predicted**.
   - `receipt` (default) → `200` after all receipts. `block_number` = highest receipt block.
   - `indexed` → `200` after receipts **and** after the daemon's own index has published `block_number`. A following search sees the writes without `min_block`.

Response:

```json
{ "results": [ { "id": "doc-900", "key": "0x5e1f…3c2d", "op": "create" } ],
  "tx_hashes": ["0x7d1c…0504"], "block_number": 582160 }
```

`results` is in request order.

## 4. Prepare (client-side signing): `POST /v1/collections/{name}/entities:prepare`

For wallets that must sign themselves (browser wallets, KMS). Capability `entities.prepare`. **No signer needed on the daemon.**

Request: `{ "entities": [EntityInput…], "from": "0xYourAddress" }`. The upsert-by-id logic uses `from` as the would-be owner.

Response:

```json
{ "chain_id": 7794095,
  "transactions": [ { "to": "0x4400000000000000000000000000000000000044", "data": "0x49650044…",
                      "value": "0x0", "nonce": 12, "gas_estimate": 412000 } ],
  "results": [ { "id": "doc-900", "key": "0x5e1f…", "op": "create" } ] }
```

The client signs each transaction as EIP-1559 (it chooses fees), broadcasts it to Arkiv itself, and may then search with `consistency.min_block` = its receipt block. Predicted keys are valid only if the transactions are mined in order, with the given `nonce`, from `from`.

## 5. Delete and extend

| Endpoint | Body | Effect |
| :--- | :--- | :--- |
| `DELETE /v1/collections/{name}/entities/{ref}` | — | `DELETE` op for one entity |
| `POST /v1/collections/{name}/entities:delete` | exactly one of `refs` (≤ 1000), `filter`, `where` | `DELETE` ops for all matching live entities owned by the signer |
| `POST /v1/collections/{name}/entities:extend` | `ttl` + exactly one of `refs`, `filter`, `where` | `EXTEND_EXPIRY` ops |

Response `TxResponse { affected, tx_hashes, block_number }`. Entities not owned by the signer are skipped for filter-based calls (not counted in `affected`) and give `409 not_owner` for `refs`-based calls. Filter-based calls are capped at 10,000 entities. Beyond that the call fails with `invalid_argument`, before anything is sent.

## 6. Idempotency

All write endpoints accept `Idempotency-Key`. The daemon stores `(key, sha256(method+path+body)) → response` for 24 h:
- same key + same body → the stored response is returned, and nothing is re-sent;
- same key + different body → `409 idempotency_conflict`;
- a request still in flight with the same key → the second caller waits for the first result.

## 7. TTL (`Ttl` object)

Exactly one of:

| Key | Meaning | Mapping to Arkiv |
| :--- | :--- | :--- |
| `blocks` | Lifetime in blocks from inclusion | `expiresAt = 0, minLifetime = blocks` |
| `seconds` | Lifetime in seconds (even) | `minLifetime = seconds / 2` |
| `days` | Lifetime in days (fractional allowed) | `minLifetime = round(days × 43200)` |
| `expires_at_block` | Absolute block | `expiresAt = value, minLifetime = 0` |

Max lifetime is 7,884,000 blocks (~182.5 days). The relative forms are preferred because they don't lose lifetime to mempool delay.
