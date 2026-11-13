# arkivector

Python client for [Arkivector](https://github.com/DikshitRJ/arkivector), pgvector for the [Arkiv](https://arkiv.network) chain: store embeddings as Arkiv entities and search them with pgvector's distance operators, HNSW and attribute filters.

Stdlib only, fully typed, Python 3.9+. Includes LangChain and LlamaIndex vector stores.

```bash
pip install arkivector
```

The SDK talks to the `arkivectord` daemon (Docker image `ghcr.io/dikshitrj/arkivectord`, or the Linux binary from the [releases](https://github.com/DikshitRJ/arkivector/releases)). It finds it through `ARKIVECTOR_URL`, default `http://127.0.0.1:8080`.

```python
from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)  # reads your own writes

client.create_collection(
    "notes",
    ("halfvec", 4),
    {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
    writers=["0xYourDaemonWalletAddress"],
    attributes={"topic": "str"},
    default_ttl={"days": 7},
)
client.upsert(
    "notes",
    [
        EntityInput(
            id="cat", vector=[1, 0, 0, 0], document="Cats purr.", attributes={"topic": "pets"}
        ),
        EntityInput(
            id="car",
            vector=[0, 0, 1, 0],
            document="Cars need fuel.",
            attributes={"topic": "vehicles"},
        ),
    ],
)

res = client.search("notes", [1, 0.05, 0, 0], k=1, filter=F.eq("topic", "pets"))
print(res.hits[0].id, res.hits[0].distance)  # cat 0.0012...
```

**Documentation:** [Python SDK](https://arkivector.dikshitrj.in/docs/python) · [REST API](https://arkivector.dikshitrj.in/docs/api) · [MCP server](https://arkivector.dikshitrj.in/docs/mcp) · [website](https://arkivector.dikshitrj.in) · [wiki](https://github.com/DikshitRJ/arkivector/wiki)

MIT licensed.
