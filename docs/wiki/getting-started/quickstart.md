# Quick start

Your first collection, writes and searches. This page assumes the daemon is running with a wallet key and you know the wallet's address: if not, do [Install and get started](../install.md) first. Pick a client; the steps are the same in all three.

## 1. Create a collection, write, search

### curl

```bash
AV=http://127.0.0.1:8080
WALLET=0xYourDaemonWalletAddress

# a collection: 4-dimensional halfvec column, HNSW cosine index
curl -s $AV/v1/collections -H 'content-type: application/json' -d '{
  "name": "notes",
  "column": {"type": "halfvec", "dimensions": 4},
  "index": {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
  "writers": ["'$WALLET'"],
  "attributes": {"topic": "str"},
  "default_ttl": {"days": 7}
}'

# three entities, written on chain; returns once they are searchable
curl -s $AV/v1/collections/notes/entities -H 'content-type: application/json' -d '{
  "entities": [
    {"id": "cat", "vector": [1, 0, 0, 0],     "document": "Cats purr.",      "attributes": {"topic": "pets"}},
    {"id": "dog", "vector": [0.9, 0.1, 0, 0], "document": "Dogs bark.",      "attributes": {"topic": "pets"}},
    {"id": "car", "vector": [0, 0, 1, 0],     "document": "Cars need fuel.", "attributes": {"topic": "vehicles"}}
  ],
  "wait": "indexed"
}'

# nearest neighbours among pets
curl -s $AV/v1/collections/notes/search -H 'content-type: application/json' \
  -d '{"vector": [1, 0.05, 0, 0], "k": 2, "filter": {"eq": {"topic": "pets"}}, "include": ["document"]}'
```

The search returns `cat` (distance 0.0012) and `dog` (0.0018).

### Python

```python
from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)  # reads always see your own writes
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

### Node.js

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

## 2. Use real embeddings

Swap the toy vectors for an embedding model's output and set `dimensions` to its size. Use the same model for documents and queries. With [sentence-transformers](https://www.sbert.net) (384 dimensions):

```python
from sentence_transformers import SentenceTransformer
from arkivector import Client, EntityInput

model = SentenceTransformer("all-MiniLM-L6-v2")
client = Client(session_consistency=True)
client.create_collection("kb", ("halfvec", 384), {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
                         writers=["0xYourDaemonWalletAddress"], default_ttl={"days": 30},
                         model="sentence-transformers/all-MiniLM-L6-v2")

docs = {"refunds": "Refunds are paid by bank transfer within 5 days.", "shipping": "We ship worldwide in 3 days."}
client.upsert("kb", [EntityInput(id=k, vector=model.encode(t).tolist(), document=t) for k, t in docs.items()])

q = model.encode("how do I get my money back?").tolist()
print(client.search("kb", q, k=1, include=["document"]).hits[0].document)
```

`halfvec` halves the on-chain bytes (and gas) of `vector` with almost no loss in recall. For LangChain or LlamaIndex pipelines, see [Python integrations](../python/integrations.md) and [LangChain.js](../node/integrations.md).

## 3. Watch the planner

Small collections are scanned exactly. Once a collection grows past a few thousand vectors, searches go through the HNSW graph. Ask for the plan to see which ran, and tune it:

```bash
curl -s $AV/v1/collections/kb/search -H 'content-type: application/json' \
  -d '{"vector": [...], "k": 10, "explain": true, "ef_search": 100}'
# "plan": {"strategy": "hnsw", "exact": false, ...}
```

`"exact": true` forces a 100% recall scan; `ef_search` (1–1000) trades speed for recall; `iterative_scan` controls filtered HNSW searches. See [Search](../api/search.md#exact-and-hnsw-plans).

## 4. Give an agent memory

Point Claude Code (or any MCP client) at the daemon and the agent can search and store memories itself:

```bash
claude mcp add --transport http arkivector http://127.0.0.1:8080/mcp
```

Other clients and the tool reference: [MCP server](../mcp/setup.md).

## Try it without a chain

`daemon/tools/dev-up.sh up` (from a source checkout) starts a local mock Arkiv node with 200 demo entities and a daemon on `:8080`. Prefix it with `ARKIV_PRIVATE_KEY=<any test key>` to enable writes against the mock. See [Contributing](../development/README.md).

Next: [Concepts](concepts.md), the full [REST API](../api/README.md), or the [Python](../python/README.md) and [Node.js](../node/README.md) SDK guides.
