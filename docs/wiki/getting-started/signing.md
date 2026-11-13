# Signing writes

Every write is an Arkiv transaction, and someone has to sign it. Arkivector gives you two places to do that, and you can mix them per request:

| | Who holds the key | Use it for |
| :--- | :--- | :--- |
| **Your own wallet** (client-side signing) | The user, in MetaMask / Rabby / WalletConnect / a hardware wallet, or your service, in `eth_account` / viem / ethers / a KMS | Apps where **each user owns their data**, and anyone who doesn't want a key on the daemon. The daemon needs no key at all |
| **The daemon's signer** (managed writes) | The daemon: an environment variable, an encrypted keystore, or a remote signer (Web3Signer, Clef) | Single-tenant servers, ingestion jobs, the MCP and Qdrant write tools |

Either way the signing address must be in the collection's `writers` (unless the collection is `open`), or the daemon won't index what it writes.

## Your own wallet: prepare → sign → submit

```
your app ── prepare ──▶ daemon            unsigned EIP-1559 transactions + predicted keys
your app ── sign ─────▶ wallet / KMS      (or send them to a browser and let MetaMask sign)
your app ── submit ───▶ daemon ──▶ Arkiv  signed transactions, or the hashes the wallet broadcast
```

1. [`entities:prepare`](../api/entities-prepare.md) builds the transactions for `from` (the user's address): calldata, nonce, gas limit and a fee suggestion. They are plain JSON, so you can sign them in the same process or ship them to a frontend.
2. Sign them however you like. A wallet that can only **send** (MetaMask's `eth_sendTransaction`) broadcasts them itself and gives you hashes; a library or KMS gives you raw signed transactions.
3. [`entities:submit`](../api/entities-submit.md) takes either back. The daemon checks raw transactions (right chain, the Arkiv contract, a writer of the collection) before broadcasting them, waits for the receipts, waits until its index has them (`wait: "indexed"`), and returns the real entity keys.

The SDKs wrap all three steps in one call.

### Python

```python
from eth_account import Account          # pip install eth-account (the SDK itself has no dependencies)
from arkivector import Client, eth_account_signer

acct = Account.from_key(user_key)
client = Client("http://127.0.0.1:8080")
res = client.upsert_signed("notes", [{"id": "fish", "vector": [0.8, 0, 0.2, 0], "document": "Fish swim."}],
                           from_address=acct.address, sign=eth_account_signer(acct))
print(res.results[0].key, res.block_number)   # mined key, searchable now
```

`sign` is any callable that returns the raw signed transaction (bytes, hex, or an `eth_account` result); pass `send=` instead for a wallet that broadcasts itself and returns the hash. Step by step, and the browser hand-off, are in [Python → Writing](../python/writing.md#sign-with-your-own-wallet). Runnable: `sdk/python/examples/sign_with_wallet.py`.

### Node.js and the browser

```ts
import { Arkivector } from "@dikshitrj/arkivector";

const client = new Arkivector({ url: "http://127.0.0.1:8080" });
// MetaMask / any EIP-1193 provider: asks for the account, switches chain, signs and sends.
const res = await client.upsertSigned("notes", [{ id: "fish", vector: [0.8, 0, 0.2, 0] }], { signer: window.ethereum });
```

`signer` can be an EIP-1193 provider (`window.ethereum`, WalletConnect, Rabby), a viem `WalletClient`, an ethers `Signer`, or a function `(tx) => rawTx | txHash`. To keep the SDK on your server, send `prepared.transactions.map(toRpcTransaction)` to the page, call `ethereum.request({ method: "eth_sendTransaction", params: [tx] })` there, and post the hashes back to `client.submit(collection, { txHashes })`. Details in [Node.js → Writing](../node/writing.md#sign-with-your-own-wallet). Runnable page: `sdk/node/examples/wallet_server.ts`.

### Things to know

- **Keys.** Prepare predicts entity keys from the wallet's on-chain entity nonce. Submit with `wait: "receipt"` or `"indexed"` returns the keys from the receipts, which are always right; the SDK helpers return those.
- **Fees.** Each transaction carries the fee the daemon would pay now (`eth_feeHistory`, then `eth_gasPrice`). Wallets may let the user change it. The daemon never replaces a stuck client-signed transaction (it can't re-sign): sign again with higher fees and submit.
- **Gas.** The signing address pays gas, so it needs a little GLM on Arkiv.
- **Updates and deletes.** `upsert_signed` / `upsertSigned` patch existing ids owned by the wallet like a managed upsert does. For deletes and TTL extensions with a user wallet, build the ops with the Arkiv SDK, sign them, and use `entities:submit` (with `tx_hashes` or `signed_transactions`) to wait for the daemon's index.
- **Scope.** With API keys on, prepare and submit need the `write` scope, like other entity writes. If a browser calls the daemon directly with a `write` key and the daemon *also* has a managed signer, that key can use the signer too: run user-facing daemons with `kind = "none"`, or keep the key on your backend and let the page talk to the wallet only.

## The daemon's signer

Configure it under `[write.signer]` ([Configuration](configuration.md#signer)). `GET /v1/version` then reports `writes_enabled: true` and `"signer": {"kind": "...", "address": "0x..."}`, never the key.

### Environment variable (default)

```bash
read -rs ARKIV_PRIVATE_KEY && export ARKIV_PRIVATE_KEY
```

With no `[write.signer]` section (`kind = "auto"`) the daemon signs with `ARKIV_PRIVATE_KEY` when it is set and is read-only otherwise. Simple, but the key sits in the process environment.

### Encrypted keystore

A Web3 Secret Storage v3 JSON file (what geth, Foundry's `cast wallet import`, MetaMask exports and `eth_account.Account.encrypt` produce), unlocked at startup with a passphrase from a file or a systemd credential:

```toml
[write.signer]
kind = "keystore"
keystore_path = "/etc/arkivector/signer.json"
passphrase_credential = "keystore-pass"   # $CREDENTIALS_DIRECTORY/keystore-pass (systemd)
# passphrase_file = "/run/secrets/keystore-pass"   # or a file (first line is used)
```

```ini
# arkivectord.service
[Service]
LoadCredentialEncrypted=keystore-pass:/etc/credstore.encrypted/keystore-pass
```

```bash
cast wallet import arkivector --interactive          # or: geth account new
systemd-creds encrypt --name=keystore-pass - /etc/credstore.encrypted/keystore-pass
```

scrypt and pbkdf2 keystores are supported (scrypt up to N = 2²⁰). A wrong passphrase stops startup with `keystore password incorrect (MAC mismatch)`. The decrypted key is wiped from memory when the daemon exits.

### Remote signer (the daemon never holds the key)

Point the daemon at a [Web3Signer](https://docs.web3signer.consensys.io/) (eth1 mode), geth/Clef, or any JSON-RPC service that implements `eth_signTransaction`. A KMS works through Web3Signer.

```toml
[write.signer]
kind = "remote"
remote_url = "http://127.0.0.1:9000"
remote_address = "0xYourSignerAddress"       # optional: otherwise eth_accounts must return exactly one
# remote_method = "account_signTransaction"  # for Clef
# remote_timeout_ms = 10000
```

For every transaction the daemon sends the full transaction object, then **decodes the signed transaction it gets back** and rejects it unless every field is exactly what it asked for and the signature recovers to the configured address. A signer that is down or refuses gives `403 write_disabled` with the signer's message; nothing is broadcast. Run the signer on localhost or a private network: the daemon does not authenticate to it.

## Which should I use?

- Users bring their own wallets → **client-side signing**, and start the daemon with `kind = "none"` (or no key) so it can't sign anything itself.
- One service writes everything → a **managed signer**; prefer `keystore` or `remote` over a raw environment variable in production.
- Both → configure a managed signer for your own jobs; users still sign their own data with prepare + submit.

See also: [Security](../operations/security.md#signing-keys), [Write path](../architecture/write-path.md).
