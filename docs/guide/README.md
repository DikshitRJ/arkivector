# Arkivector documentation

Arkivector is pgvector for [Arkiv](https://arkiv.network). A small daemon, `arkivectord`, syncs entities from the Arkiv chain, indexes their vectors with pgvector's own code (exact scans and HNSW), and serves vector search over REST. The SDKs are thin clients of that daemon.

| Guide | For |
| :--- | :--- |
| [REST API with curl](api.md) | Any language, shell scripts, or just trying it out |
| [Python SDK](python.md) | `pip install arkivector` |
| [Node.js / TypeScript SDK](node.md) | `npm install @dikshitrj/arkivector` |

## Five-minute tour

1. **Run the daemon** (Docker, any OS):

   ```bash
   docker run -d --name arkivectord -p 127.0.0.1:8080:8080 -v arkivector-data:/data \
     -e ARKIVECTOR_ARKIV_ACCESS_KEY="<Arkiv Hub access key>" \
     -e ARKIV_PRIVATE_KEY="<wallet private key, for writes>" \
     ghcr.io/dikshitrj/arkivectord:latest
   curl http://127.0.0.1:8080/healthz
   ```

   A Linux binary is attached to every [release](https://github.com/DikshitRJ/arkivector/releases). Without `ARKIV_PRIVATE_KEY` the daemon is read-only. Get test GLM from the [faucet](https://hub.arkiv.network/faucet) and an access key from [Arkiv Hub](https://hub.arkiv.network/access-keys).

2. **Create a collection**: a vector column and an index, bound to the wallets whose entities belong to it.
3. **Upsert entities**: vectors, documents and typed attributes become Arkiv entities on chain.
4. **Search**: nearest neighbours with pgvector's distance operators, combined with attribute filters.

Each guide walks through these steps for its client.

## Concepts

- **Collections** map to a pgvector table plus index: `halfvec(768)` with `hnsw (halfvec_cosine_ops)`, say. The daemon follows every Arkiv entity that carries the collection's schema tag and was created by one of its `writers`.
- **Entities** live on chain. The daemon's index can always be rebuilt from Arkiv; nothing depends on Arkivector staying online.
- **Search** uses pgvector's kernels. Small or heavily filtered candidate sets are scanned exactly; large ones go through the HNSW graph, with exact fallback when a filter is very selective.
- **`as_of_block`**: every response says which chain block it reflects. Pass `min_block` (or use session consistency in the SDKs) to read your own writes.
- **Entity lifetime**: Arkiv entities expire. Set `default_ttl` on the collection or `ttl` per entity, and extend when needed.

## Reference

- API contract: [`spec/openapi.yaml`](https://github.com/DikshitRJ/arkivector/blob/main/spec/openapi.yaml) and [`docs/api/`](https://github.com/DikshitRJ/arkivector/tree/main/docs/api)
- Daemon configuration: [`daemon/arkivector.example.toml`](https://github.com/DikshitRJ/arkivector/blob/main/daemon/arkivector.example.toml)
- Architecture: [`ARCHITECTURE.md`](https://github.com/DikshitRJ/arkivector/blob/main/ARCHITECTURE.md)
