# Submit (signed by your wallet)

`POST /v1/collections/{name}/entities:submit` is the second half of [signing with your own wallet](../getting-started/signing.md): it takes back the transactions [prepare](entities-prepare.md) built, after **you** signed them, broadcasts them, and waits until they are on chain and in the daemon's index. The daemon needs no key. Capability `entities.submit` (API 1.1); `write` scope when auth is on.

Give exactly one of:

```bash
# You signed the transactions (eth_account, viem, ethers, a KMS...): the daemon broadcasts them.
curl -s "$AV/v1/collections/notes/entities:submit" -H 'content-type: application/json' -d '{
  "signed_transactions": ["0x02f8728376edaf0c8405f5e10084773594008306496094440000…"],
  "wait": "indexed"
}'

# Your wallet already broadcast them (MetaMask eth_sendTransaction): the daemon only waits.
curl -s "$AV/v1/collections/notes/entities:submit" -H 'content-type: application/json' -d '{
  "tx_hashes": ["0x7d1c3b5a9e8f7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a392817060504"],
  "wait": "indexed"
}'
```

```json
{
  "tx_hashes": ["0x7d1c3b5a9e8f7d6c5b4a39281706f5e4d3c2b1a09f8e7d6c5b4a392817060504"],
  "block_number": 582160,
  "created_keys": ["0x5e1f0c0d9a8b7c6d5e4f3a2b1c0d9e8f7a6b5c4d3e2f1a0b9c8d7e6f5a4b3c2d"]
}
```

## Request

| Field | Default | Meaning |
| :--- | :--- | :--- |
| `signed_transactions` | | 1–256 raw signed EIP-1559 transactions (`0x02…`), in the order they must be mined |
| `tx_hashes` | | 1–256 hashes of transactions your wallet already broadcast |
| `wait` | `receipt` | `none` (return after broadcasting, `202`), `receipt` (after every receipt), `indexed` (after the daemon's index has them: a following search sees them) |
| `timeout_ms` | 60000 | Up to 600000 |

`Idempotency-Key` is accepted, as for every write.

## Response

| Field | Meaning |
| :--- | :--- |
| `tx_hashes` | One per transaction (computed by the daemon for `signed_transactions`) |
| `block_number` | Highest receipt block; `null` with `wait: "none"` |
| `created_keys` | Keys of the entities the transactions created (from the receipt logs), in order. Empty with `wait: "none"` |

## Checks

Before broadcasting **anything**, every signed transaction must: decode as a canonical signed EIP-1559 transaction with a low-s signature, use the daemon's chain id, call the Arkiv contract with `value` 0 and `execute(...)` calldata, and be signed by a writer of the collection (unless it is `open`).

| Problem | Status / code |
| :--- | :--- |
| Malformed, legacy (type 0) or wrong-chain transaction, other contract, bad JSON field | `422 invalid_argument` |
| Signer is not a writer of the collection | `403 forbidden` |
| The node rejects a transaction (nonce, funds, revert) | `409 tx_reverted` (earlier ones stay broadcast) |
| A receipt shows a revert | `409 tx_reverted` |
| No receipt within `timeout_ms` | `504 consistency_timeout` (retryable; submit the hashes again) |

A transaction the node already knows counts as accepted, so retrying a submit is safe. The daemon does not speed up a stuck transaction (it cannot re-sign it): sign a replacement with the same nonce and higher fees, and submit that.
