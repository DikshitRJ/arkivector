# @dikshitrj/arkivector

Node.js and TypeScript client for [Arkivector](https://github.com/DikshitRJ/arkivector), pgvector for the [Arkiv](https://arkiv.network) chain: store embeddings as Arkiv entities and search them with pgvector's distance operators, HNSW and attribute filters.

ESM, zero runtime dependencies, Node 18+. Includes a LangChain.js vector store (`@dikshitrj/arkivector/langchain`).

```bash
echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
read -rs NODE_AUTH_TOKEN && export NODE_AUTH_TOKEN   # GitHub token with read:packages
echo '//npm.pkg.github.com/:_authToken=${NODE_AUTH_TOKEN}' >> .npmrc   # a reference, not the token
npm install @dikshitrj/arkivector
```

The SDK talks to the `arkivectord` daemon (Docker image `ghcr.io/dikshitrj/arkivectord`, or the Linux binary from the [releases](https://github.com/DikshitRJ/arkivector/releases)). It finds it through `ARKIVECTOR_URL`, default `http://127.0.0.1:8080`.

```ts
import { Arkivector, F } from "@dikshitrj/arkivector";

const client = new Arkivector({ sessionConsistency: true });

await client.createCollection({
  name: "notes",
  column: { type: "halfvec", dimensions: 4 },
  index: { method: "hnsw", opclass: "halfvec_cosine_ops" },
  writers: ["0xYourDaemonWalletAddress"],
  attributes: { topic: "str" },
  defaultTtl: { days: 7 },
});
await client.upsert("notes", [
  { id: "cat", vector: [1, 0, 0, 0], document: "Cats purr.", attributes: { topic: "pets" } },
  { id: "car", vector: [0, 0, 1, 0], document: "Cars need fuel.", attributes: { topic: "vehicles" } },
]);

const res = await client.search("notes", { vector: [1, 0.05, 0, 0], k: 1, filter: F.eq("topic", "pets") });
console.log(res.hits[0].id, res.hits[0].distance); // cat 0.0012...
```

**Sign with your own wallet** (no key on the daemon): the daemon prepares the transactions, the wallet signs them, and the daemon broadcasts and indexes them.

```ts
// MetaMask / any EIP-1193 provider, a viem WalletClient, an ethers Signer, or (tx) => rawTx | txHash
await client.upsertSigned("notes", [{ id: "fish", vector: [0, 1, 0, 0] }], { signer: window.ethereum });
```

Or step by step: `client.prepare(...)`, convert with `toRpcTransaction` / `toViemTransaction` / `toEthersTransaction`, sign, then `client.submit(collection, { signedTransactions } | { txHashes })`. Guide: [Signing writes](https://arkivector.dikshitrj.in/docs/getting-started/signing).

**Documentation:** [Node.js SDK](https://arkivector.dikshitrj.in/docs/node) · [REST API](https://arkivector.dikshitrj.in/docs/api) · [MCP server](https://arkivector.dikshitrj.in/docs/mcp) · [website](https://arkivector.dikshitrj.in) · [wiki](https://github.com/DikshitRJ/arkivector/wiki)

MIT licensed.
