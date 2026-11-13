# Prepare (sign with your own wallet)

`POST /v1/collections/{name}/entities:prepare` builds the Arkiv transactions for an upsert **without sending them**, so a browser wallet, a hardware wallet, a library such as `eth_account` / viem / ethers, or a KMS can sign. The daemon needs no key for this; it uses `from` as the would-be owner for the upsert-by-id logic. The overall flow is in [Signing writes](../getting-started/signing.md); the SDKs do prepare → sign → submit in one call.

```bash
curl -s "$AV/v1/collections/notes/entities:prepare" -H 'content-type: application/json' -d '{
  "from": "0xYourAddress",
  "entities": [ { "id": "fish", "vector": [0.8, 0, 0.2, 0], "document": "Fish swim.", "ttl": { "days": 7 } } ]
}'
```

```json
{
  "chain_id": 7794095,
  "from": "0x70997970C51812dc3A010C7d01b50e0d17dc79C8",
  "fees": { "max_fee_per_gas": "0x77359400", "max_priority_fee_per_gas": "0x5f5e100", "source": "fee_history" },
  "transactions": [
    { "type": 2, "chain_id": 7794095, "from": "0x70997970C51812dc3A010C7d01b50e0d17dc79C8",
      "to": "0x4400000000000000000000000000000000000044", "data": "0x49650044…",
      "value": "0x0", "nonce": 12, "gas_estimate": 412000, "gas": 412000,
      "max_fee_per_gas": "0x77359400", "max_priority_fee_per_gas": "0x5f5e100" }
  ],
  "results": [ { "id": "fish", "key": "0x5e1f…", "op": "create" } ]
}
```

| Field | Meaning |
| :--- | :--- |
| `from` | Address that will sign and own the entities. Must be in the collection's `writers` |
| `entities` | Same as [upsert](entities-upsert.md#request) |
| `transactions` | Unsigned EIP-1559 calls to the Arkiv contract, in order, each ready to sign: `type`, `chain_id`, `from`, `nonce`, `gas` (gas limit), and a fee suggestion in wei (`max_fee_per_gas`, `max_priority_fee_per_gas`, 0x-hex) |
| `fees` | The fee suggestion once more, with where it came from (`fee_history`, `gas_price` or `static`) |
| `results` | Predicted keys and operations |

The `type`, `chain_id`, `from`, `gas`, fee fields and `fees` are new in API 1.1. For a browser wallet, rename the keys to camelCase with hex quantities (`chainId`, `maxFeePerGas`, …) and pass the object to `eth_sendTransaction`; the SDKs do this for you (`tx.to_rpc()` in Python, `toRpcTransaction(tx)` in Node).

## Then

1. Sign each transaction with your wallet (you may change the fees), in order.
2. Hand them back with [`entities:submit`](entities-submit.md): `signed_transactions` if you have the raw signed bytes (the daemon broadcasts them), or `tx_hashes` if your wallet broadcast them itself. With `wait: "indexed"` the next search sees the entities, and the response has the real keys.

You can also broadcast to Arkiv yourself and read with `consistency.min_block` set to the receipt's block number ([Consistency](consistency.md)).

The `nonce` of the first transaction is the pending transaction count of `from`, and the keys are derived from its on-chain `entityNonce`, both read from the chain when you call prepare. Predicted keys are valid only if the transactions are mined in order, from `from`, with the given nonces, and nothing else creates entities from `from` in between; `entities:submit` returns the keys from the receipts. `gas` (= `gas_estimate`) is `eth_estimateGas` plus the daemon's `write.gas_margin_pct`.
