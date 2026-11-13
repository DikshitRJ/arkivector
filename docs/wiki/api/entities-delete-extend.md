# Delete and extend

Remove entities from the chain, or extend their lifetime. Both send Arkiv transactions, so they need a signer and the `write` scope. Only entities owned by the daemon's wallet can be changed. For entities owned by a user's wallet, sign the `DELETE` / `EXTEND_EXPIRY` transactions with that wallet (e.g. with the Arkiv SDK) and pass them to [submit](entities-submit.md) to wait for the index.

## Delete one: `DELETE /v1/collections/{name}/entities/{ref}`

```bash
curl -s -X DELETE $AV/v1/collections/notes/entities/car
```

```json
{"affected": 1, "tx_hashes": ["0x76fb03d779486be6135c34ffa6c6b184d7cf302084fa1e5caf135f1225a6da80"], "block_number": 18}
```

## Delete many: `POST /v1/collections/{name}/entities:delete`

Exactly one of `refs` (≤ 1000 ids or keys), `filter` or `where`:

```bash
curl -s "$AV/v1/collections/notes/entities:delete" -H 'content-type: application/json' \
  -d '{"where": "stars < 2"}'
```

```json
{"affected": 0, "tx_hashes": [], "block_number": 0}
```

Filter-based deletes skip entities the wallet does not own (they are not counted in `affected`) and are capped at 10,000 matches; above that the call fails before sending anything. `refs`-based deletes fail with `409 not_owner` for a foreign entity.

## Extend: `POST /v1/collections/{name}/entities:extend`

`ttl` plus exactly one of `refs`, `filter` or `where`:

```bash
curl -s "$AV/v1/collections/notes/entities:extend" -H 'content-type: application/json' \
  -d '{"refs": ["cat"], "ttl": {"days": 30}}'
```

```json
{"affected": 1, "tx_hashes": ["0xa8bdf53968e38fb2298bc64e55e3293a10eae81f11efb0ceaf40e7f0ee0befe7"], "block_number": 17}
```

## TTL forms

| Key | Meaning |
| :--- | :--- |
| `blocks` | Lifetime in blocks from inclusion |
| `seconds` | Lifetime in seconds (even; blocks are 2 s on Rabdi) |
| `days` | Lifetime in days (fractional allowed) |
| `expires_at_block` | Absolute expiry block |

The maximum lifetime is 7,884,000 blocks (about 182.5 days). Relative forms are preferred: they don't lose lifetime to mempool delay.

## Response

`TxResponse`: `affected` (entities changed), `tx_hashes`, `block_number` (use as `min_block` to read the result). Both endpoints accept `Idempotency-Key` like [upserts](entities-upsert.md#idempotency).
