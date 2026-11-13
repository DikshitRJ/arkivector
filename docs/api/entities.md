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

Requires a configured signer (`writes_enabled: true`, capability `entities.write`): an environment key, an encrypted keystore, or a remote signer (see `GET /v1/version` → `signer`). Otherwise `403 write_disabled`; use [client-side signing](#4-client-side-signing-prepare--sign--submit) instead.

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

## 4. Client-side signing: prepare → sign → submit

For apps where **each user signs with their own wallet** (browser wallets such as MetaMask, Rabby or WalletConnect, hardware wallets, `eth_account`, viem, ethers, a KMS). **No signer is needed on the daemon.** The daemon builds the transactions, the client signs them however it likes, and the daemon broadcasts them (or watches the ones the wallet broadcast) and waits until they are indexed.

```
SDK/app ── prepare ──▶ daemon            (unsigned EIP-1559 txs + predicted keys)
SDK/app ── sign ─────▶ wallet / KMS      (or hand the txs to a browser and let MetaMask sign)
SDK/app ── submit ───▶ daemon ──▶ Arkiv  (signed txs, or the hashes the wallet broadcast)
```

### 4.1 Prepare: `POST /v1/collections/{name}/entities:prepare`

Capability `entities.prepare`. Request: `{ "entities": [EntityInput…], "from": "0xYourAddress" }`. The upsert-by-id logic uses `from` as the would-be owner. `from` must be a writer of the collection (unless `membership = "open"`) or the daemon will not index the result.

Response (fields marked *1.1* were added by CCR-0001):

```json
{ "chain_id": 7794095,
  "from": "0x70997970C51812dc3A010C7d01b50e0d17dc79C8",
  "fees": { "max_fee_per_gas": "0x77359400", "max_priority_fee_per_gas": "0x5f5e100", "source": "fee_history" },
  "transactions": [ { "type": 2, "chain_id": 7794095, "from": "0x7099…79C8",
                      "to": "0x4400000000000000000000000000000000000044", "data": "0x49650044…",
                      "value": "0x0", "nonce": 12, "gas_estimate": 412000, "gas": 412000,
                      "max_fee_per_gas": "0x77359400", "max_priority_fee_per_gas": "0x5f5e100" } ],
  "results": [ { "id": "doc-900", "key": "0x5e1f…", "op": "create" } ] }
```

| Field | Meaning |
| :--- | :--- |
| `transactions[]` | Unsigned EIP-1559 transactions, in the order they must be mined. Each one is complete: sign it as is, or rename the keys to camelCase and pass it to a wallet's `eth_sendTransaction` (the SDKs have helpers) |
| `type`, `chain_id`, `from` *1.1* | Always `2`, the daemon's Arkiv chain id, and the request's `from` |
| `gas` *1.1* | Gas limit to sign with: `eth_estimateGas` + `write.gas_margin_pct` (same as `gas_estimate`) |
| `max_fee_per_gas`, `max_priority_fee_per_gas` *1.1* | 0x-hex wei. The fee the managed writer would use now (`eth_feeHistory` → `eth_gasPrice` → static). The client may replace them |
| `fees` *1.1* | The same fee suggestion once per response, with its `source` |
| `nonce` | Pending transaction count of `from` (first tx), then +1 per tx |
| `results` | Predicted keys and operations, in request order |

Predicted keys are valid only if the transactions are mined in order, with the given `nonce`, from `from`, and nothing else creates entities from `from` in between. `entities:submit` with `wait` receipt/indexed returns the real keys from the receipts.

### 4.2 Submit: `POST /v1/collections/{name}/entities:submit` *(1.1, CCR-0001)*

Capability `entities.submit`. Accepts `Idempotency-Key`. Exactly one of:

| Field | Use when |
| :--- | :--- |
| `signed_transactions` | The client signed the transactions (raw `0x02…` hex, 1–256, in order). The daemon broadcasts them with `eth_sendRawTransaction` |
| `tx_hashes` | The wallet already broadcast them (MetaMask and most browser wallets only offer `eth_sendTransaction`, which signs **and** sends). The daemon only waits |

Plus `wait` (`none` / `receipt` (default) / `indexed`, same meaning as for upsert) and `timeout_ms` (default 60,000, max 600,000).

Checks on `signed_transactions`, all done **before anything is broadcast**: each must decode as a canonical signed EIP-1559 transaction with a low-s signature, its chain id must be the daemon's, `to` must be the Arkiv contract, `value` must be 0, the calldata must be an `execute(...)` call, and the recovered sender must be a writer of the collection (unless it is `open`). Violations → `422 invalid_argument` (`403 forbidden` for a sender that is not a writer). A node rejection is `409 tx_reverted`; transactions before the rejected one stay broadcast. "already known" counts as accepted, so a retried submit is harmless. For `tx_hashes` the receipt must be a call to the Arkiv contract from a writer.

Response:

```json
{ "tx_hashes": ["0x7d1c…0504"], "block_number": 582160,
  "created_keys": ["0x5e1f…3c2d"] }
```

`created_keys` lists the keys of the `EntityCreated` logs of the receipts, in order (empty with `wait: "none"`). `block_number` is the highest receipt block, `null` with `wait: "none"` (status `202`). With `wait: "indexed"` a following search sees the writes without `min_block`. The daemon does not replace a stuck client-signed transaction (it cannot re-sign); the client resubmits with higher fees.

## 5. Delete and extend

| Endpoint | Body | Effect |
| :--- | :--- | :--- |
| `DELETE /v1/collections/{name}/entities/{ref}` | — | `DELETE` op for one entity |
| `POST /v1/collections/{name}/entities:delete` | exactly one of `refs` (≤ 1000), `filter`, `where` | `DELETE` ops for all matching live entities owned by the signer |
| `POST /v1/collections/{name}/entities:extend` | `ttl` + exactly one of `refs`, `filter`, `where` | `EXTEND_EXPIRY` ops |

These use the managed signer. With a user wallet, build the `DELETE` / `EXTEND_EXPIRY` ops with the Arkiv SDK, sign them, and pass the signed transactions or hashes to `entities:submit` to wait for indexing.

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
