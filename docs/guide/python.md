# Python SDK

`arkivector` is a thin, fully typed client for the Arkivector daemon. It has no runtime dependencies (stdlib only), supports Python 3.9+, and ships adapters for LangChain and LlamaIndex.

```bash
pip install arkivector
```

You need a running daemon ([install guide](https://github.com/DikshitRJ/arkivector#1-run-the-daemon)). The client finds it through `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`).

## Quick start

```python
from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)

client.create_collection(
    "notes",
    ("halfvec", 4),                                   # column type and dimension
    {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
    writers=["0xYourDaemonWalletAddress"],            # the daemon's signing address
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

`session_consistency=True` makes every read wait for the block of your last write, so you always see your own writes.

## The client

```python
Client(
    url=None,                  # default: $ARKIVECTOR_URL or http://127.0.0.1:8080
    api_key=None,              # default: $ARKIVECTOR_API_KEY (sent as a Bearer token)
    timeout=10.0,              # seconds
    max_retries=3,             # retries for idempotent calls and retryable errors
    session_consistency=False, # read-your-writes
    idempotency=True,          # send an Idempotency-Key with every write (makes retries safe)
)
```

The client is thread-safe and reuses connections. `client.close()` releases them (or use it as a context manager).

| Method | What |
| :--- | :--- |
| `version()` | `VersionInfo` (daemon/api/pgvector versions, `writes_enabled`, `capabilities`) |
| `has_capability(name)` | Whether the daemon supports a feature, e.g. `"search.hnsw"` |
| `health()`, `ready()`, `sync_status()` | Liveness, readiness per collection, sync lag |

## Collections

```python
col = client.create_collection(
    name, column, index, *,
    schema_tag=None, writers=None, membership="creator",
    attributes=None, default_ttl=None, publish_manifest=False, model=None, mode="full",
)
client.list_collections()          # [Collection]
client.get_collection("notes")     # Collection (stats, sync state)
client.follow_collection(manifest_key="0x…", name="their-notes")
client.delete_collection("notes")  # local index only; on-chain data stays
```

- `column`: `("halfvec", 768)`, `ColumnDef("vector", 1536)` or `{"type": ..., "dimensions": ...}`.
- `index`: `{"method": "hnsw", "opclass": "halfvec_cosine_ops", "with": {"m": 16, "ef_construction": 64}}`, or `{"method": "none", ...}` for exact-only.
- `writers`: only entities created by these addresses are indexed. Include your daemon's wallet address.

## Writing

```python
from arkivector import EntityInput, T, Ttl

result = client.upsert("notes", [
    EntityInput(
        id="cat",
        vector=[1, 0, 0, 0],
        document="Cats purr.",
        attributes={"topic": "pets", "stars": 5, "price": T.dec("4.50")},
        ttl=Ttl.days(30),
    ),
], wait="receipt")

for item in result.results:
    print(item.id, item.op, item.key)   # cat create 0xc595…
print(result.block_number, result.tx_hashes)
```

- **Upsert by id**: an existing `id` is patched (`op="patch"`), an unchanged one is a `noop`.
- **`wait`**: `"none"`, `"receipt"` (default), or `"indexed"` (returns once the entity is searchable).
- **Attributes**: plain values are typed from the collection schema; use `T.str/i32/u64/u256/dec/bool/addr/key/bytes` to be explicit.
- **TTL**: `Ttl.days(7)`, `Ttl.seconds(3600)`, `Ttl.blocks(1000)`, `Ttl.at_block(n)` (max ~182 days).

```python
client.extend("notes", Ttl.days(30), refs=["cat"])   # TxResult(affected=1, ...)
client.delete("notes", refs=["car"])
client.delete("notes", where="stars < 2")            # only entities your wallet owns
```

### Signing with your own wallet

`prepare` returns unsigned transactions (no key on the daemon needed):

```python
prep = client.prepare("notes", [EntityInput(id="fish", vector=[0.8, 0, 0.2, 0])], from_address="0xYourAddress")
for tx in prep.transactions:
    print(tx.to, tx.nonce, tx.data[:20])   # sign as EIP-1559 and broadcast with your wallet
```

Then read with `min_block=<receipt block>`.

## Searching

```python
res = client.search(
    "notes", vector,
    k=10, offset=0,
    filter=None, where=None,          # attribute filter (one of the two)
    operator=None,                    # "<->", "<#>", "<=>", "<+>", "<~>", "<%>"
    exact=False,                      # True: exact scan, 100% recall
    ef_search=None,                   # HNSW breadth, 1..1000 (default 40)
    iterative_scan=None,              # "off" | "relaxed_order" | "strict_order"
    max_scan_tuples=None,
    max_distance=None, min_score=None,
    include=("attributes",),          # add "document", "vector"
    min_block=None, timeout_ms=None,  # read-your-writes
    explain=False,                    # res.plan shows exact_flat / hnsw / hnsw_iterative
)
res.as_of_block, res.sync_state, res.plan
for hit in res.hits:
    hit.id, hit.key, hit.distance, hit.score, hit.attributes, hit.document
```

`vector` can be a list of floats, a pgvector literal string (`"[1,0,0,0]"`), or one of the typed values `Vector`, `HalfVector`, `SparseVector`, `Bit`.

Collections with an HNSW index answer large queries from the graph and small or very selective ones with an exact scan; `explain=True` tells you which. Pass `exact=True` when recall must be 100%.

### Batch, count, scroll, get

```python
batch = client.search_batch("notes", [{"vector": [1, 0, 0, 0], "k": 1}, {"vector": [0, 0, 1, 0], "k": 1}])
[r.hits[0].id for r in batch.results]               # ['cat', 'car']

client.count("notes", filter=F.eq("topic", "pets")).count   # 2

for entity in client.scroll("notes", where="stars >= 4"):   # auto-paginates in key order
    print(entity.id, entity.attributes)

e = client.get("notes", "cat", include=["attributes", "document"])
e.document, e.expires_at_block
```

## Filters

`F` builds the JSON filter tree:

```python
from arkivector import F, T

F.eq("topic", "pets")
F.gte("stars", 4) & F.eq("published", True)        # and
F.eq("topic", "pets") | F.eq("topic", "birds")     # or
~F.exists("archived")                              # not
F.between("stars", 2, 4)
F.startswith("region", "eu-")
F.eq("$creator", T.addr("0x1111111111111111111111111111111111111111"))
```

Or pass a `where` string in Arkiv's query language: `where="stars >= 4 AND topic = str('pets')"`.

## Errors

Every API error is an `ApiError` with `code`, `status`, `retryable`, `request_id` and `problem` (the raw problem document). Catch the category you care about:

```python
from arkivector import NotFoundError, ValidationError, UnavailableError, AuthError, ConflictError

try:
    client.get("notes", "nope")
except NotFoundError as e:
    print(e.code)            # entity_not_found
```

| Exception | Codes |
| :--- | :--- |
| `TransportError` | No HTTP response (connection refused, timeout) |
| `AuthError` | `unauthorized`, `forbidden`, `write_disabled` |
| `NotFoundError` | `collection_not_found`, `entity_not_found` |
| `ConflictError` | `collection_exists`, `not_owner`, `tx_reverted`, `idempotency_conflict` |
| `ValidationError` | `bad_request`, `invalid_*`, `dimension_mismatch`, `payload_too_large` |
| `UnavailableError` | `rate_limited`, `not_ready`, `consistency_timeout`, `arkiv_rpc_error`, `timeout` |
| `NotImplementedByDaemon` | The daemon lacks the capability (checked before sending) |

## LangChain and LlamaIndex

```python
from arkivector import Client
from arkivector.langchain import ArkivectorVectorStore

store = ArkivectorVectorStore(client=Client(), collection="notes", embedding=my_embeddings)
store.add_texts(["Cats purr.", "Dogs bark."], metadatas=[{"topic": "pets"}, {"topic": "pets"}])
docs = store.similarity_search("which animal purrs?", k=1)
```

```python
from arkivector.llama_index import ArkivectorVectorStore
vector_store = ArkivectorVectorStore(client=Client(), collection="notes")
```

Install `langchain-core` or `llama-index-core` yourself; the SDK does not depend on them.

## VectorBytes

Vectors are stored on chain in the VectorBytes v1 format (a 4-byte header plus pgvector's in-memory layout). The SDK encodes and decodes it for you; to do it by hand:

```python
from arkivector import HalfVector, vectorbytes_decode_any
b = HalfVector([1, 0, 0, 0]).to_vectorbytes()
vectorbytes_decode_any(b)          # HalfVector([1.0, 0.0, 0.0, 0.0])
```
