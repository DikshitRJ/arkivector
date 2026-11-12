# @dikshitrj/arkivector

Node.js and TypeScript client for [Arkivector](https://github.com/DikshitRJ/arkivector), pgvector for the [Arkiv](https://arkiv.network) chain: store embeddings as Arkiv entities and search them with pgvector's distance operators, HNSW and attribute filters.

ESM, zero runtime dependencies, Node 18+. Includes a LangChain.js vector store (`@dikshitrj/arkivector/langchain`).

```bash
echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
echo "//npm.pkg.github.com/:_authToken=<GITHUB_TOKEN with read:packages>" >> .npmrc
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

**Documentation:** [Node.js SDK guide](https://github.com/DikshitRJ/arkivector/blob/main/docs/guide/node.md) · [REST API](https://github.com/DikshitRJ/arkivector/blob/main/docs/guide/api.md) · [wiki](https://github.com/DikshitRJ/arkivector/wiki)

MIT licensed.
