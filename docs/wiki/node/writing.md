# Writing

`upsert`, `extend` and `delete` go on chain through the daemon's wallet; the daemon needs a signer (`(await client.version()).writesEnabled`). To have each user sign with their own wallet instead (no key on the daemon), see [Sign with your own wallet](#sign-with-your-own-wallet).

## Upsert

```ts
import { T, Ttl } from "@dikshitrj/arkivector";

const out = await client.upsert(
  "notes",
  [{ id: "cat", vector: [1, 0, 0, 0], document: "Cats purr.",
     attributes: { topic: "pets", stars: 5, price: T.dec("4.50") }, ttl: Ttl.days(30) }],
  { wait: "receipt" },          // "none" | "receipt" | "indexed"
);
out.results;      // [{ id: "cat", key: "0xc595…", op: "create" }]
out.blockNumber;  // block the transaction landed in
out.txHashes;
```

Options: `wait`, `timeoutMs`, `idempotencyKey`. Entity fields: `id`, `vector` or `vectorBytes`, `document`, `attributes`, `ttl`. An existing `id` is patched (`op: "patch"`); an unchanged one is a `noop`. Up to 1000 entities per call.

## Extend and delete

```ts
await client.extend("notes", Ttl.days(30), { refs: ["cat"] });   // TxResult { affected, txHashes, blockNumber }
await client.delete("notes", { ref: "car" });                    // one entity
await client.delete("notes", { refs: ["a", "b"] });
await client.delete("notes", { where: "stars < 2" });            // or filter: F.lt("stars", 2)
```

Filter-based calls skip entities the wallet doesn't own.

## Sign with your own wallet

The daemon builds the transactions, the wallet signs them, the daemon broadcasts them (or watches the ones the wallet broadcast) and waits until they are indexed. No key on the daemon. Overview: [Signing writes](../getting-started/signing.md).

### One call

```ts
const res = await client.upsertSigned("notes", [{ id: "fish", vector: [0.8, 0, 0.2, 0] }], {
  signer: window.ethereum,      // or a viem WalletClient, an ethers Signer, or (tx) => rawTx | txHash
  wait: "indexed",              // default
});
res.results;                     // [{ id: "fish", key: "0x…", op: "create" }]: mined keys
```

Options: `signer` (required), `from` (needed only when the signer can't tell its address, e.g. a function), `wait`, `timeoutMs`, `idempotencyKey`.

| `signer` | What happens |
| :--- | :--- |
| EIP-1193 provider (`window.ethereum`, WalletConnect, Rabby, Frame) | Asks for the account, switches the wallet to the Arkiv chain (`wallet_switchEthereumChain`), sends each transaction with `eth_sendTransaction` (the user approves), then submits the hashes |
| viem `WalletClient` | Local accounts sign (`signTransaction`) and the daemon broadcasts; JSON-RPC accounts send |
| ethers `Signer` (`Wallet`, `JsonRpcSigner`) | `signTransaction`, falling back to `sendTransaction` for browser signers |
| `(tx) => string \| Uint8Array` | Return the raw signed transaction (`0x02…`) or, if you broadcast it yourself, the 32-byte hash. Pass `from` |
| `{ signTransaction(tx) }` / `{ sendTransaction(tx) }` | Same, as an object |

### Step by step

```ts
import { toRpcTransaction, toViemTransaction, toEthersTransaction } from "@dikshitrj/arkivector";

const prep = await client.prepare("notes", entities, { from: address });
const tx = prep.transactions[0];
// { type: 2, chainId, from, to, data, value, nonce, gas, maxFeePerGas, maxPriorityFeePerGas, gasEstimate }

const raw = await viemWallet.signTransaction({ ...toViemTransaction(tx), account: viemWallet.account, chain: null });
const sub = await client.submit("notes", { signedTransactions: [raw], wait: "indexed" });
sub.txHashes; sub.blockNumber; sub.createdKeys;
```

`client.submit(collection, { signedTransactions } | { txHashes }, …)` takes exactly one of the two; `wait` defaults to `"receipt"`. The converters return what each library expects: `toRpcTransaction` (hex strings, for `eth_sendTransaction`), `toViemTransaction` and `toEthersTransaction` (bigints). They have no dependencies and work in a browser bundle.

### Keep the SDK on the server

```ts
// server
const prep = await client.prepare("notes", entities, { from: userAddress });
res.json(prep.transactions.map(toRpcTransaction));
// browser
const hashes = [];
for (const tx of txs) hashes.push(await ethereum.request({ method: "eth_sendTransaction", params: [tx] }));
// server
await client.submit("notes", { txHashes: hashes, wait: "indexed" });
```

Runnable page: `sdk/node/examples/wallet_server.ts`. See [Prepare](../api/entities-prepare.md) and [Submit](../api/entities-submit.md).
