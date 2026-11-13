# Collections

Create, list, follow and delete collections from Node.js. A collection is a vector column, an index and the wallets whose entities it indexes.

```ts
const col = await client.createCollection({
  name: "notes",
  column: { type: "halfvec", dimensions: 768 },
  index: { method: "hnsw", opclass: "halfvec_cosine_ops", with: { m: 16, ef_construction: 64 } },
  writers: ["0xYourDaemonWalletAddress"],
  attributes: { topic: "str", stars: "i32" },
  defaultTtl: { days: 7 },
  model: "openai/text-embedding-3-small",
});
col.name; col.column.type; col.stats.liveEntities; col.sync.state;
```

| Option | Meaning |
| :--- | :--- |
| `column` | `{ type: "vector" \| "halfvec" \| "sparsevec" \| "bit", dimensions }` |
| `index` | `{ method: "hnsw" \| "none", opclass, with? }` ([opclasses](../api/collections.md#operator-classes)) |
| `writers` | Addresses whose entities belong to the collection. **Include your daemon's wallet** |
| `membership` | `"creator"` (default), `"owner"`, `"open"` |
| `attributes` | Declared attribute types |
| `defaultTtl` | `{ days: 7 }`, `Ttl.days(7)`, ... |
| `schemaTag`, `model`, `mode`, `publishManifest` | See [Collections API](../api/collections.md#request-fields) |

```ts
await client.listCollections();                                   // Collection[]
await client.getCollection("notes");                              // Collection
await client.followCollection({ manifestKey: "0x…", name: "their-notes" });
await client.deleteCollection("notes");                           // local index only
```
