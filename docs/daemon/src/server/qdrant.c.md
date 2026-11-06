# `daemon/src/server/qdrant.c`

Implements the Qdrant REST API v1.x compatible subset.

## Supported Endpoints
- `GET /qdrant/collections`: Lists collections with names.
- `GET /qdrant/collections/{name}`: Returns collection status, vector dimensions, and metric (`Cosine`, `Euclid`, `Dot`, `Manhattan`).
- `PUT /qdrant/collections/{name}`: Creates an open collection matching the vector dimensionality and metric distance.
- `DELETE /qdrant/collections/{name}`: Deletes collection from the registry and LMDB.
- `PUT /qdrant/collections/{name}/points`: Upserts points.
- `POST /qdrant/collections/{name}/points/search` & `/query`: Executes approximate or exact vector search, formatting hits into Qdrant envelope format `{result: [...], status: "ok", time: ...}`.
- `POST /qdrant/collections/{name}/points/count`: Returns total point count.
- `POST /qdrant/collections/{name}/points/delete`: Deletes points.
