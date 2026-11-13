# Qdrant compatibility

The daemon serves a subset of the Qdrant REST API under `/qdrant`, so tools that already speak Qdrant (LangChain and LlamaIndex Qdrant stores, Dify, Flowise, Haystack, n8n) can point at Arkivector:

```
QDRANT_URL=http://127.0.0.1:8080/qdrant
```

Responses use Qdrant's envelope `{"result": …, "status": "ok", "time": <seconds>}`.

```bash
curl -s $AV/qdrant/collections/notes/points/search -H 'content-type: application/json' \
  -d '{"vector": [1, 0, 0, 0], "limit": 2, "with_payload": true}'
```

| Qdrant endpoint | Arkivector behaviour |
| :--- | :--- |
| `GET /collections` | List collections |
| `GET /collections/{c}` | Vector size and distance, `points_count` = live entities, `status` green when live |
| `PUT /collections/{c}` | Create a `halfvec` collection: `Cosine`/`Euclid`/`Dot`/`Manhattan` → `halfvec_cosine_ops`/`_l2_ops`/`_ip_ops`/`_l1_ops` |
| `DELETE /collections/{c}` | Delete the collection (local index only) |
| `POST /collections/{c}/points/search` | Search: `vector`, `limit`, `offset`, `filter`, `with_payload`, `with_vector`, `score_threshold` |
| `POST /collections/{c}/points/query` | Same as search |
| `POST /collections/{c}/points` | Retrieve by ids |
| `POST /collections/{c}/points/scroll` | Scroll |
| `POST /collections/{c}/points/count` | Exact count |

Filters map `must` → and, `should` → or, `must_not` → not, `match.value` → eq, `match.any` → or of eq, `range` → comparisons, `is_empty` → not exists. Scores follow Qdrant: similarity for `Cosine` and `Dot`, distance for `Euclid`.

**Limitations:** compatibility is **search-only** today, and search hits carry ids and scores but no payload yet (`payload` is null even with `with_payload`); `PUT …/points` (upsert) and `POST …/points/delete` return success without writing. Write through the [REST API](entities-upsert.md) or an SDK. Named or multiple vectors, sparse vectors, quantization, snapshots, aliases and the recommend/discover/group APIs return `501`.
