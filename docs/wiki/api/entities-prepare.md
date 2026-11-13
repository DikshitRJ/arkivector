# Prepare (sign with your own wallet)

`POST /v1/collections/{name}/entities:prepare` builds the Arkiv transactions for an upsert **without sending them**, so a browser wallet, a hardware wallet or a KMS can sign. The daemon needs no key for this; it uses `from` as the would-be owner for the upsert-by-id logic.

```bash
curl -s "$AV/v1/collections/notes/entities:prepare" -H 'content-type: application/json' -d '{
  "from": "0xYourAddress",
  "entities": [ { "id": "fish", "vector": [0.8, 0, 0.2, 0], "document": "Fish swim.", "ttl": { "days": 7 } } ]
}'
```

```json
{
  "chain_id": 7794095,
  "transactions": [
    { "to": "0x4400000000000000000000000000000000000044", "data": "0x49650044…",
      "value": "0x0", "nonce": 12, "gas_estimate": 412000 }
  ],
  "results": [ { "id": "fish", "key": "0x5e1f…", "op": "create" } ]
}
```

| Field | Meaning |
| :--- | :--- |
| `from` | Address that will sign and own the entities. Must be in the collection's `writers` |
| `entities` | Same as [upsert](entities-upsert.md#request) |
| `transactions` | Unsigned calls to the Arkiv contract, in order |
| `results` | Predicted keys and operations |

## Then

1. Sign each transaction as EIP-1559 with your wallet (you choose the fees) and broadcast it to Arkiv.
2. Read with `consistency.min_block` set to the receipt's block number ([Consistency](consistency.md)).

Predicted keys are valid only if the transactions are mined in order, from `from`, with the given nonces.
