# Writing

Writes go on chain through the daemon's wallet, so the daemon needs a signer (`client.version().writes_enabled`).

## Upsert

```python
from arkivector import EntityInput, T, Ttl

result = client.upsert("notes", [
    EntityInput(
        id="cat",
        vector=[1, 0, 0, 0],
        document="Cats purr.",
        attributes={"topic": "pets", "stars": 5, "price": T.dec("4.50")},
        ttl=Ttl.days(30),
    ),
], wait="receipt")

for item in result.results:
    print(item.id, item.op, item.key)   # cat create 0xc595…
print(result.block_number, result.tx_hashes)
```

```python
client.upsert(collection, entities, wait="receipt", timeout_ms=60000, idempotency_key=None) -> UpsertResult
```

- `entities`: `EntityInput` objects or plain dicts with the same fields (`id`, `vector` or `vector_bytes`, `document`, `attributes`, `ttl`).
- **Upsert by id**: an existing `id` is patched (`op="patch"`); an unchanged one is a `noop` and sends nothing.
- `wait`: `"none"`, `"receipt"` (default) or `"indexed"` (returns once searchable).
- Attribute values are typed from the collection schema; use [`T`](types.md#typed-values) to be explicit.
- Up to 1000 entities per call; they are packed into as few transactions as fit.

## Extend and delete

```python
client.extend("notes", Ttl.days(30), refs=["cat"])     # TxResult(affected=1, tx_hashes=[...], block_number=...)
client.delete("notes", refs=["car"])
client.delete("notes", where="stars < 2")              # or filter=F.lt("stars", 2)
```

`refs` are ids or keys. Filter-based calls skip entities the wallet doesn't own.

## Sign with your own wallet

```python
prep = client.prepare("notes", [EntityInput(id="fish", vector=[0.8, 0, 0.2, 0])], from_address="0xYourAddress")
for tx in prep.transactions:
    print(tx.to, tx.nonce, tx.gas_estimate)   # sign as EIP-1559 and broadcast with your wallet
```

Then read with `min_block=<receipt block>`. See [Prepare](../api/entities-prepare.md).
