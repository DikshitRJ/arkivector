# Writing

`upsert`, `extend` and `delete` go on chain through the daemon's wallet, so the daemon needs a signer (`client.version().writes_enabled`). To have each user sign with their own wallet instead (no key on the daemon), see [Sign with your own wallet](#sign-with-your-own-wallet).

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

The daemon builds the transactions, you sign them, the daemon broadcasts them and waits until they are indexed. No key on the daemon. Overview: [Signing writes](../getting-started/signing.md).

### One call

```python
from eth_account import Account                  # pip install eth-account (only for this example)
from arkivector import eth_account_signer

acct = Account.from_key(user_key)
res = client.upsert_signed("notes", [EntityInput(id="fish", vector=[0.8, 0, 0.2, 0])],
                           from_address=acct.address, sign=eth_account_signer(acct))
res.results[0].key, res.block_number            # mined key; searchable now (wait="indexed")
```

```python
client.upsert_signed(collection, entities, from_address, sign=None, send=None,
                     wait="indexed", timeout_ms=60000, idempotency_key=None) -> UpsertResult
```

Give exactly one of:

- `sign(tx) -> raw`: returns the raw signed transaction as `bytes`, `0x…` hex, or an object with `raw_transaction` (what `eth_account` returns). The daemon broadcasts it. Use it for `eth_account`, a hardware wallet library, an HSM or a KMS.
- `send(tx) -> tx_hash`: broadcasts the transaction itself and returns the hash, e.g. `web3_sender(w3)` for a node that holds the key.

`tx` is a `PreparedTransaction`. `wait` defaults to `"indexed"`, and the returned keys come from the receipts.

### Step by step

```python
prep = client.prepare("notes", entities, from_address=acct.address)   # PrepareResult
tx = prep.transactions[0]
tx.chain_id, tx.nonce, tx.gas, tx.max_fee_per_gas, tx.max_priority_fee_per_gas   # all set, wei
tx.max_fee_per_gas = 3 * 10**9                    # change the fees if you like

signed = [acct.sign_transaction(t.to_eth_account()).raw_transaction for t in prep.transactions]
sub = client.submit("notes", signed_transactions=signed, wait="indexed")
sub.tx_hashes, sub.block_number, sub.created_keys
```

- `tx.to_eth_account()`: dict for `eth_account` / web3.py (`chainId`, `maxFeePerGas`, … as integers).
- `tx.to_rpc()`: dict for JSON-RPC `eth_sendTransaction` (hex quantities): what a browser wallet expects.
- `client.submit(collection, signed_transactions=None, tx_hashes=None, wait="receipt", timeout_ms=60000)`: exactly one of the two lists. Returns `SubmitResult(tx_hashes, block_number, created_keys)`.

### Sign in the browser (MetaMask)

Keep the SDK on your server and let the user's browser wallet sign:

```python
# 1. server: build the transactions for the user's address and send them to the page as JSON
prep = client.prepare("notes", entities, from_address=user_address)
payload = [tx.to_rpc() for tx in prep.transactions]
```

```js
// 2. browser: the wallet signs and broadcasts; post the hashes back to the server
const hashes = [];
for (const tx of payload) hashes.push(await ethereum.request({ method: "eth_sendTransaction", params: [tx] }));
```

```python
# 3. server: wait until the daemon has indexed them
client.submit("notes", tx_hashes=hashes, wait="indexed")
```

Runnable: `sdk/python/examples/sign_with_wallet.py`. See [Prepare](../api/entities-prepare.md) and [Submit](../api/entities-submit.md).
