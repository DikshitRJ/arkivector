# Searching

```ts
const res = await client.search("notes", {
  vector: [1, 0, 0, 0],          // number[], Float32Array, pgvector literal, or Vector/HalfVector/SparseVector/Bit
  k: 10,
  offset: 0,
  filter: F.gte("stars", 4),     // or: where: "stars >= 4"
  operator: "<=>",               // default: the index operator
  exact: false,                  // true: exact scan, 100% recall
  efSearch: 100,                 // HNSW breadth (1..1000, default 40)
  iterativeScan: "relaxed_order",// "off" | "relaxed_order" | "strict_order"
  maxScanTuples: 20000,
  maxDistance: 0.5,
  minScore: 0.2,
  include: ["attributes", "document"],
  consistency: { minBlock: 15, timeoutMs: 4000 },
  explain: true,
});
```

## Results

```ts
res.asOfBlock; res.syncState; res.openCollection; res.plan;
for (const hit of res.hits) {
  hit.id; hit.key; hit.distance; hit.score;
  hit.owner; hit.creator; hit.expiresAtBlock;
  hit.attributes;   // { topic: TypedValue { type: "str", value: "pets" } }
  hit.document;     // with include: ["document"]
  hit.vector;       // with include: ["vector"], decoded
}
```

`distance` is pgvector's distance for the operator; `score` is `1 − distance` for cosine and `−distance` otherwise ([Search API](../api/search.md#distance-and-score)).

## Exact or HNSW

HNSW collections answer large queries from the graph and small or very selective ones exactly; `res.plan.strategy` (with `explain: true`) says `exact_flat`, `hnsw` or `hnsw_iterative`. Raise `efSearch` for recall, or set `exact: true`.

## Batch search

```ts
const batch = await client.searchBatch("notes", [
  { vector: [1, 0, 0, 0], k: 1 },
  { vector: [0, 0, 1, 0], k: 1, where: "stars >= 3" },
]);
batch.results.map((r) => r.hits[0].id);   // ["cat", "car"]
```

Up to 64 searches, all answered from one block.
