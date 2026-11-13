# Quick start

Five minutes from a running daemon to a filtered vector search. Pick a client; all three do the same thing. You need a daemon with a signer ([Installation](installation.md)) and its wallet address.

## curl

```bash
AV=http://127.0.0.1:8080
WALLET=0xYourDaemonWalletAddress

# 1. a collection: 4-dimensional halfvec column, HNSW cosine index
curl -s $AV/v1/collections -H 'content-type: application/json' -d '{
  "name": "notes",
  "column": {"type": "halfvec", "dimensions": 4},
  "index": {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
  "writers": ["'$WALLET'"],
  "attributes": {"topic": "str"},
  "default_ttl": {"days": 7}
}'

# 2. three entities, written on chain; returns once they are searchable
curl -s $AV/v1/collections/notes/entities -H 'content-type: application/json' -d '{
  "entities": [
    {"id": "cat", "vector": [1, 0, 0, 0],     "document": "Cats purr.",      "attributes": {"topic": "pets"}},
    {"id": "dog", "vector": [0.9, 0.1, 0, 0], "document": "Dogs bark.",      "attributes": {"topic": "pets"}},
    {"id": "car", "vector": [0, 0, 1, 0],     "document": "Cars need fuel.", "attributes": {"topic": "vehicles"}}
  ],
  "wait": "indexed"
}'

# 3. nearest neighbours among pets
curl -s $AV/v1/collections/notes/search -H 'content-type: application/json' \
  -d '{"vector": [1, 0.05, 0, 0], "k": 2, "filter": {"eq": {"topic": "pets"}}, "include": ["document"]}'
```

The search returns `cat` (distance 0.0012) and `dog` (0.0018).

## Python

```python
from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)
client.create_collection("notes", ("halfvec", 4), {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
                         writers=["0xYourDaemonWalletAddress"], attributes={"topic": "str"}, default_ttl={"days": 7})
client.upsert("notes", [
    EntityInput(id="cat", vector=[1, 0, 0, 0], document="Cats purr.", attributes={"topic": "pets"}),
    EntityInput(id="dog", vector=[0.9, 0.1, 0, 0], document="Dogs bark.", attributes={"topic": "pets"}),
    EntityInput(id="car", vector=[0, 0, 1, 0], document="Cars need fuel.", attributes={"topic": "vehicles"}),
])
for hit in client.search("notes", [1, 0.05, 0, 0], k=2, filter=F.eq("topic", "pets")).hits:
    print(hit.id, round(hit.distance, 4))
```

## Node.js

```ts
import { Arkivector, F } from "@dikshitrj/arkivector";

const client = new Arkivector({ sessionConsistency: true });
await client.createCollection({
  name: "notes", column: { type: "halfvec", dimensions: 4 },
  index: { method: "hnsw", opclass: "halfvec_cosine_ops" },
  writers: ["0xYourDaemonWalletAddress"], attributes: { topic: "str" }, defaultTtl: { days: 7 },
});
await client.upsert("notes", [
  { id: "cat", vector: [1, 0, 0, 0], document: "Cats purr.", attributes: { topic: "pets" } },
  { id: "dog", vector: [0.9, 0.1, 0, 0], document: "Dogs bark.", attributes: { topic: "pets" } },
  { id: "car", vector: [0, 0, 1, 0], document: "Cars need fuel.", attributes: { topic: "vehicles" } },
]);
const res = await client.search("notes", { vector: [1, 0.05, 0, 0], k: 2, filter: F.eq("topic", "pets") });
console.log(res.hits.map((h) => [h.id, h.distance]));
```

## With real embeddings

Replace the toy vectors with your embedding model's output and set `dimensions` to its size (768, 1024, 1536, ...). `halfvec` halves the on-chain bytes (and gas) of `vector` with almost no loss in recall. Use the same model for documents and queries.

## Try it without a chain

`daemon/tools/dev-up.sh up` starts a local mock Arkiv node with 200 demo entities and a daemon on `:8080` (see [Contributing](../development/README.md)). Add `ARKIV_PRIVATE_KEY=<any test key>` to enable writes against the mock.

Next: [Concepts](concepts.md), or the full [REST API](../api/README.md).
