# Writing

Writes go on chain through the daemon's wallet; the daemon needs a signer (`(await client.version()).writesEnabled`).

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

```ts
const prep = await client.prepare("notes", [{ id: "fish", vector: [0.8, 0, 0.2, 0] }], { from: "0xUserAddress" });
for (const tx of prep.transactions) {
  // { to, data, value, nonce, gasEstimate }: sign as EIP-1559 with MetaMask, WalletConnect, a hardware wallet...
}
```

Then read with `consistency: { minBlock: <receipt block> }`. See [Prepare](../api/entities-prepare.md).
