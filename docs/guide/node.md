# Node.js / TypeScript SDK

`@dikshitrj/arkivector` is a typed client for the Arkivector daemon. It is ESM-only, has zero runtime dependencies, runs on Node 18+ (and anywhere with `fetch`), and includes a LangChain.js vector store.

```bash
echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
echo "//npm.pkg.github.com/:_authToken=<GITHUB_TOKEN with read:packages>" >> .npmrc
npm install @dikshitrj/arkivector
```

The package is published to GitHub Packages: the first `.npmrc` line tells npm where to find the `@dikshitrj` scope, and the second gives it a GitHub token (classic token with `read:packages`), which GitHub Packages requires even for public packages. Each [GitHub Release](https://github.com/DikshitRJ/arkivector/releases) also attaches the package as `arkivector-node-<version>.tgz` (`npm install ./arkivector-node-<version>.tgz`).

You need a running daemon ([install guide](https://github.com/DikshitRJ/arkivector#1-run-the-daemon)). The client finds it through `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`).

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
  vector: [1, 0.05, 0, 0],
  k: 2,
  filter: F.eq("topic", "pets"),
  include: ["attributes", "document"],
});
for (const hit of res.hits) console.log(hit.id, hit.distance.toFixed(4), hit.document);
// cat 0.0012 Cats purr.
// dog 0.0018 Dogs bark.
```

## The client

```ts
new Arkivector({
  url,                 // default: process.env.ARKIVECTOR_URL ?? "http://127.0.0.1:8080"
  apiKey,              // default: process.env.ARKIVECTOR_API_KEY (Bearer token)
  timeoutMs: 10_000,
  maxRetries: 3,       // idempotent calls and retryable errors
  sessionConsistency: false, // read-your-writes: reads wait for the block of your last write
  idempotency: true,   // Idempotency-Key on every write, so retries are safe
  fetch,               // custom fetch implementation (tests, proxies, edge runtimes)
});
```

`Client` is an alias of `Arkivector`. Request and response fields are camelCase; snake_case keys are accepted too.

| Method | What |
| :--- | :--- |
| `version()` | Daemon/API/pgvector versions, `writesEnabled`, `capabilities` |
| `hasCapability(name)` | e.g. `"search.hnsw"`, `"entities.write"`, `"mcp"` |
| `health()`, `ready()`, `syncStatus()` | Liveness, readiness, sync lag |

## Collections

```ts
await client.createCollection({ name, column, index, writers, attributes, defaultTtl, schemaTag, membership, model, mode });
await client.listCollections();
await client.getCollection("notes");            // stats.liveEntities, sync.state, ...
await client.followCollection({ manifestKey: "0x…", name: "their-notes" });
await client.deleteCollection("notes");         // local index only
```

`index.method` is `"hnsw"` (approximate search on large collections, with exact fallback) or `"none"` (always exact). `index.with` takes `m` and `ef_construction`. **Put the daemon wallet's address in `writers`**: only entities created by a writer are indexed.

## Writing

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

await client.extend("notes", Ttl.days(30), { refs: ["cat"] });
await client.delete("notes", { ref: "car" });
await client.delete("notes", { where: "stars < 2" });   // only entities your wallet owns
```

Prepare unsigned transactions for a user's own wallet (MetaMask, WalletConnect, a hardware wallet):

```ts
const prep = await client.prepare("notes", [{ id: "fish", vector: [0.8, 0, 0.2, 0] }], { from: "0xUserAddress" });
// prep.transactions: [{ to, data, value, nonce, gasEstimate }] -> sign as EIP-1559 and send
```

## Searching

```ts
const res = await client.search("notes", {
  vector: [1, 0, 0, 0],      // number[], Float32Array, pgvector literal, or Vector/HalfVector/SparseVector/Bit
  k: 10,
  offset: 0,
  filter: F.gte("stars", 4), // or: where: "stars >= 4"
  operator: "<=>",           // default: the index operator
  exact: false,              // true: exact scan, 100% recall
  efSearch: 100,             // HNSW breadth (1..1000)
  iterativeScan: "relaxed_order", // "off" | "relaxed_order" | "strict_order"
  include: ["attributes", "document"],
  explain: true,             // res.plan.strategy: "exact_flat" | "hnsw" | "hnsw_iterative"
});
res.asOfBlock; res.syncState; res.plan;
res.hits[0].id; res.hits[0].distance; res.hits[0].score; res.hits[0].attributes;
```

Batch, count, scroll and point reads:

```ts
const batch = await client.searchBatch("notes", [{ vector: [1, 0, 0, 0], k: 1 }, { vector: [0, 0, 1, 0], k: 1 }]);
batch.results.map((r) => r.hits[0].id);              // ["cat", "car"]

(await client.count("notes", { filter: F.eq("topic", "pets") })).count;

for await (const entity of client.scroll("notes", { where: "stars >= 4" })) console.log(entity.id);

const e = await client.get("notes", "cat", { include: ["attributes", "document"] });
```

## Filters

```ts
F.eq("topic", "pets");
F.and(F.gte("stars", 4), F.eq("published", true));
F.or(F.eq("topic", "pets"), F.eq("topic", "birds"));
F.not(F.exists("archived"));
F.between("stars", 2, 4);
F.startswith("region", "eu-");
F.eq("$creator", T.addr("0x1111111111111111111111111111111111111111"));
```

Or a `where` string in Arkiv's query language: `"stars >= 4 AND topic = str('pets')"`.

## Errors

All API errors extend `ApiError` (`code`, `status`, `retryable`, `requestId`, `problem`):

```ts
import { NotFoundError } from "@dikshitrj/arkivector";
try {
  await client.get("notes", "nope");
} catch (e) {
  if (e instanceof NotFoundError) console.log(e.code); // entity_not_found
}
```

Categories: `TransportError`, `AuthError`, `NotFoundError`, `ConflictError`, `ValidationError`, `UnavailableError`, `NotImplementedByDaemonError`, plus `VectorBytesError` for local codec errors. The mapping from error codes is the same as in the [REST guide](api.md#errors).

## LangChain.js

```ts
import { ArkivectorVectorStore } from "@dikshitrj/arkivector/langchain";

const store = new ArkivectorVectorStore(embeddings, { client, collection: "notes" });
await store.addDocuments([{ pageContent: "Cats purr.", metadata: { topic: "pets" } }]);
const docs = await store.similaritySearch("which animal purrs?", 1);
```

`embeddings` is any LangChain embeddings object (`embedDocuments`, `embedQuery`).
