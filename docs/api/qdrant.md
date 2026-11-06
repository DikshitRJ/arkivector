# Qdrant-Compatible Subset (`/qdrant/*`)

Daemon-only (capability `qdrant`, phase D6). Arkivector SDKs do **not** use it. It exists so that existing Qdrant integrations (LangChain `QdrantVectorStore`, LlamaIndex `QdrantVectorStore`, Dify, Flowise, Haystack, n8n) work when pointed at `http://<daemon>:8080/qdrant`.

The target is the **Qdrant REST API v1.x** request/response shapes for the endpoints below. Responses use Qdrant's envelope `{"result": …, "status": "ok", "time": <seconds>}`. Errors use `{"status": {"error": "<message>"}, "time": …}` with the HTTP status from the Arkivector error.

| Qdrant endpoint | Mapping |
| :--- | :--- |
| `GET /collections` | List: `{"collections": [{"name"}]}` |
| `GET /collections/{c}` | `config.params.vectors = {size: column.dimensions, distance}`, `points_count = stats.live_entities`, `status = green` if live else `yellow` |
| `PUT /collections/{c}` | Create: `vectors.size` → dims, `distance` `Cosine`/`Euclid`/`Dot`/`Manhattan` → `halfvec_cosine_ops`/`halfvec_l2_ops`/`halfvec_ip_ops`/`halfvec_l1_ops` (column `halfvec`), writers from daemon config `qdrant.default_writers` |
| `DELETE /collections/{c}` | Delete |
| `PUT /collections/{c}/points` | Upsert: `id` (u64 or UUID) → `av.id` (decimal or UUID string). `vector` → vector. `payload` scalars → attributes (string → str, bool → bool, integer → i32 if it fits else u64, float → dec). Payload key `page_content` or `text` → `document`. Other nested JSON → `document` as JSON if no text key exists, else dropped with a warning header |
| `POST /collections/{c}/points/search` | Search: `vector`, `limit` → k, `offset`, `filter`, `with_payload`, `with_vector`, `score_threshold` → `min_score` |
| `POST /collections/{c}/points/query` | Same as search (`query` = vector) |
| `POST /collections/{c}/points` | Retrieve by `ids` |
| `POST /collections/{c}/points/scroll` | Scroll (`offset` = cursor) |
| `POST /collections/{c}/points/count` | Count (`exact` ignored, always exact) |
| `POST /collections/{c}/points/delete` | Delete by `points` ids or `filter` |

Filter mapping: `must` → `and`, `should` → `or`, `must_not` → `not(or(…))`, `match.value` → `eq`, `match.any` → `or` of `eq`, `range {gt,gte,lt,lte}` → comparisons, `is_empty` → `not exists`. Payload `key` paths with dots are not supported (→ 501).

Score: Qdrant `Cosine` and `Dot` scores are similarities, which is our `score`. For `Euclid`, Qdrant returns the distance, so we return `distance` in `score` for that metric to stay compatible.

Unsupported (→ `501`): named/multiple vectors, sparse vectors, quantization config, sharding, snapshots, aliases, payload indexes (all attributes are already indexed), recommend/discover/group APIs.
