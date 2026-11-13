# Qdrant compatibility

The daemon serves a subset of the Qdrant REST API under `/qdrant`, so tools that already speak Qdrant (LangChain and LlamaIndex Qdrant stores, Dify, Flowise, Haystack, n8n, the official `qdrant-client`) can point at Arkivector:

```
QDRANT_URL=http://127.0.0.1:8080/qdrant
```

```python
from qdrant_client import QdrantClient
from qdrant_client.models import Distance, PointStruct, VectorParams

client = QdrantClient(url="http://127.0.0.1:8080", prefix="qdrant")
client.create_collection("notes", vectors_config=VectorParams(size=4, distance=Distance.COSINE))
client.upsert("notes", points=[PointStruct(id=1, vector=[1, 0, 0, 0], payload={"page_content": "Cats purr.", "topic": "pets"})], wait=True)
hits = client.query_points("notes", query=[1, 0, 0, 0], limit=2, with_payload=True).points
client.delete("notes", points_selector=[1], wait=True)
```

Responses use Qdrant's envelope `{"result": …, "status": "ok", "time": <seconds>}`. Errors use `{"status": {"error": "<code>: <message>"}}` with the HTTP status of the Arkivector error code.

| Qdrant endpoint | Arkivector behaviour |
| :--- | :--- |
| `GET /` | Server info (`version` is the Qdrant API version the shapes follow) |
| `GET /collections` | List collections |
| `GET /collections/{c}` | Vector size and distance, `points_count` = live entities, `status` green when live |
| `PUT /collections/{c}` | Create a `halfvec` collection: `Cosine`/`Euclid`/`Dot`/`Manhattan` → `halfvec_cosine_ops`/`_l2_ops`/`_ip_ops`/`_l1_ops`. Open membership, `default_ttl` 30 days |
| `DELETE /collections/{c}` | Delete the collection (local index only) |
| `PUT /collections/{c}/points` | Upsert through the managed writer (needs a signer), see below |
| `POST /collections/{c}/points/delete` | Delete by `points` ids or by `filter` (needs a signer) |
| `POST /collections/{c}/points/search` | Search: `vector`, `limit`, `offset`, `filter`, `with_payload`, `with_vector`, `score_threshold`. Result: array of points |
| `POST /collections/{c}/points/query` | Same, with `query` (vector or `{"nearest": vector}`). Result: `{"points": [...]}` |
| `POST /collections/{c}/points/count` | Exact count, with an optional `filter` |

## Writes

Upserts and deletes go through the same managed writer as [`POST /v1/collections/{name}/entities`](entities-upsert.md), so they are real Arkiv transactions signed by the daemon's wallet.

- **`?wait=true`** waits until the change is mined **and** indexed (a following search sees it) and answers `status: "completed"`. Without it the transaction is broadcast and the answer is `status: "acknowledged"`.
- **Point ids** are `u64` or UUIDs. They become the entity id (`av.id`): a decimal string, or the lower-case hyphenated UUID. Search returns them in the same form.
- **Payload:** `page_content` (else `text`) becomes the entity document. Scalars become attributes: string → `str`, bool → `bool`, integer → `i32` (or `u64` when larger), float → `dec`; `null` is skipped. Values that cannot be attributes (nested objects and arrays, strings over 128 bytes, keys that are not Arkiv attribute names such as `_node_content`) are stored as the document, as one JSON object, when the payload has no text key. With a text key they are dropped and the response carries a `Warning` header.
- On search, the payload is rebuilt from the attributes plus the document: a JSON-object document is merged back key by key, any other document comes back as `page_content`.
- An upsert of an existing id patches the entity in place. Attributes missing from the new payload are not removed.
- Deleting ids that do not exist is not an error (like Qdrant). Deleting with an empty filter is refused.
- **Without a signer** (no `ARKIV_PRIVATE_KEY`) the daemon is read-only: upsert and delete return `403` with `write_disabled`, never a fake success.

## Filters

`must` → and, `should` → or, `must_not` → not(or), `match.value` → eq, `match.any` → or of eq, `match.except` → not(or of eq), `range` → comparisons, `is_empty`/`is_null` → not exists, `has_id` → id lookup. Integer and float payload values are stored as different types, so `range` matches both. Scores follow Qdrant: similarity for `Cosine` and `Dot`, distance for `Euclid` (and `score_threshold` is then a maximum distance).

**Not supported (→ `501`):** key paths with dots, `match.text`, geo and `values_count` conditions, `min_should`, named or multiple vectors, sparse vectors, quantization, snapshots, aliases, payload indexes, retrieve-by-id and scroll, and the recommend/discover/group APIs.
