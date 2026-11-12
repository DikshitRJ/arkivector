# Node.js / TypeScript SDK

`@dikshitrj/arkivector` is a typed client for the Arkivector daemon: ESM, zero runtime dependencies, Node 18+ (and any runtime with `fetch`), with a LangChain.js vector store. It wraps the [REST API](../api/README.md) and adds the VectorBytes codec, filter builders, retries and read-your-writes.

```bash
echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
echo "//npm.pkg.github.com/:_authToken=<GITHUB_TOKEN with read:packages>" >> .npmrc
npm install @dikshitrj/arkivector
```

The package is published to GitHub Packages, which needs a token (classic, `read:packages`) even for public packages. Without a token, install the tarball attached to each [release](https://github.com/DikshitRJ/arkivector/releases): `npm install https://github.com/DikshitRJ/arkivector/releases/download/v1.1.0/arkivector-node-1.1.0.tgz`.

You need a running daemon ([Installation](../getting-started/installation.md)). The client finds it through `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`).

## Quick start

```ts
import { Arkivector, F } from "@dikshitrj/arkivector";

const client = new Arkivector({ sessionConsistency: true });

await client.createCollection({
  name: "notes",
  column: { type: "halfvec", dimensions: 4 },
  index: { method: "hnsw", opclass: "halfvec_cosine_ops" },
  writers: ["0xYourDaemonWalletAddress"],
  attributes: { topic: "str", stars: "i32" },
  defaultTtl: { days: 7 },
});

await client.upsert("notes", [
  { id: "cat", vector: [1, 0, 0, 0], document: "Cats purr.", attributes: { topic: "pets", stars: 5 } },
  { id: "dog", vector: [0.9, 0.1, 0, 0], document: "Dogs bark.", attributes: { topic: "pets", stars: 4 } },
  { id: "car", vector: [0, 0, 1, 0], document: "Cars need fuel.", attributes: { topic: "vehicles", stars: 3 } },
]);

const res = await client.search("notes", {
  vector: [1, 0.05, 0, 0], k: 2, filter: F.eq("topic", "pets"), include: ["attributes", "document"],
});
for (const hit of res.hits) console.log(hit.id, hit.distance.toFixed(4), hit.document);
// cat 0.0012 Cats purr.
// dog 0.0018 Dogs bark.
```

## Pages

| Page | Covers |
| :--- | :--- |
| [Client](client.md) | Options, health, version, capabilities |
| [Collections](collections.md) | Create, list, get, follow, delete |
| [Writing](writing.md) | Upsert, extend, delete, prepare |
| [Searching](searching.md) | Search options, HNSW knobs, batch search |
| [Reading](reading.md) | Get, count, scroll |
| [Filters](filters.md) | The `F` builder and `where` strings |
| [Types](types.md) | Vector types, `T`, `Ttl`, VectorBytes |
| [Errors](errors.md) | Error classes |
| [LangChain.js](integrations.md) | Vector store adapter |

Request and response fields are camelCase (`expiresAtBlock`, `asOfBlock`); snake_case keys are accepted in requests too. Every example in these pages was run against a daemon. Source: [`sdk/node`](https://github.com/DikshitRJ/arkivector/tree/main/sdk/node).
