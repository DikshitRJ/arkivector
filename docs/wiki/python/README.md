# Python SDK

`arkivector` is a typed client for the Arkivector daemon: stdlib only (no runtime dependencies), Python 3.9+, thread-safe, with adapters for LangChain and LlamaIndex. It wraps the [REST API](../api/README.md) and adds the VectorBytes codec, filter builders, retries and read-your-writes.

```bash
pip install arkivector
```

You need a running daemon ([Installation](../install.md)). The client finds it through `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`).

## Quick start

```python
from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)   # reads see your own writes

client.create_collection(
    "notes",
    ("halfvec", 4),                                   # column type and dimension
    {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
    writers=["0xYourDaemonWalletAddress"],
    attributes={"topic": "str", "stars": "i32"},
    default_ttl={"days": 7},
)

client.upsert("notes", [
    EntityInput(id="cat", vector=[1, 0, 0, 0], document="Cats purr.", attributes={"topic": "pets", "stars": 5}),
    EntityInput(id="dog", vector=[0.9, 0.1, 0, 0], document="Dogs bark.", attributes={"topic": "pets", "stars": 4}),
    EntityInput(id="car", vector=[0, 0, 1, 0], document="Cars need fuel.", attributes={"topic": "vehicles", "stars": 3}),
])

res = client.search("notes", [1, 0.05, 0, 0], k=2, filter=F.eq("topic", "pets"), include=["attributes", "document"])
for hit in res.hits:
    print(hit.id, round(hit.distance, 4), hit.document)
# cat 0.0012 Cats purr.
# dog 0.0018 Dogs bark.
```

## Pages

| Page | Covers |
| :--- | :--- |
| [Client](client.md) | Constructor options, health, version, capabilities |
| [Collections](collections.md) | Create, list, get, follow, delete |
| [Writing](writing.md) | Upsert, extend, delete, prepare |
| [Searching](searching.md) | Search options, HNSW knobs, batch search |
| [Reading](reading.md) | Get, count, scroll |
| [Filters](filters.md) | The `F` builder and `where` strings |
| [Types](types.md) | Vector types, typed values `T`, `Ttl`, VectorBytes |
| [Errors](errors.md) | Exception classes |
| [LangChain and LlamaIndex](integrations.md) | Vector store adapters |

Every example in these pages was run against a daemon. Source: [`sdk/python`](https://github.com/DikshitRJ/arkivector/tree/main/sdk/python).
