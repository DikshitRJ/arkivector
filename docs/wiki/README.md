# Arkivector documentation

Arkivector is **pgvector for [Arkiv](https://arkiv.network)**. A small daemon, `arkivectord`, syncs entities from the Arkiv chain, indexes their vectors with pgvector's own code (exact scans and HNSW), and serves vector search over REST and MCP. The Python and Node.js SDKs are thin clients of that daemon.

Website: [arkivector.dikshitrj.in](https://arkivector.dikshitrj.in) · Source: [github.com/DikshitRJ/arkivector](https://github.com/DikshitRJ/arkivector) · Releases: [GitHub Releases](https://github.com/DikshitRJ/arkivector/releases)

## Start here

| If you want to | Read |
| :--- | :--- |
| Install the daemon, a wallet and an SDK | [Install and get started](install.md) |
| Store and search vectors in five minutes | [Quick start](getting-started/quickstart.md) |
| Understand collections, entities, blocks and TTLs | [Concepts](getting-started/concepts.md) |
| Call it from any language or a shell | [REST API](api/README.md) |
| Use Python | [Python SDK](python/README.md) |
| Use Node.js or TypeScript | [Node.js SDK](node/README.md) |
| Give an AI agent memory | [MCP server](mcp/README.md) |
| Know how it works inside | [Architecture](architecture/README.md) |
| Run it in production | [Deployment](operations/deployment.md), [Security](operations/security.md) |
| Hack on it | [Contributing](development/README.md) |

## What it does

- Similarity search over Arkiv entities with all four pgvector types (`vector`, `halfvec`, `sparsevec`, `bit`) and all six distance operators (`<->`, `<#>`, `<=>`, `<+>`, `<~>`, `<%>`).
- pgvector's HNSW index for large collections, with iterative scans for filtered queries, and exact scans when that is faster or you need 100% recall.
- Attribute filters as a JSON tree or as Arkiv `where` strings, combined with vector search in one request.
- Writes that go on chain: the daemon builds, signs and sends the Arkiv transaction, then indexes the entity when it syncs back.
- Read-your-writes consistency through `as_of_block` and `min_block`.
- An MCP endpoint for AI agents and a Qdrant-compatible subset for existing integrations.

## About these pages

Every page lives in [`docs/wiki/`](https://github.com/DikshitRJ/arkivector/tree/main/docs/wiki) in the repository. The same Markdown files are rendered on the [website](https://arkivector.dikshitrj.in/docs) and published to the [GitHub wiki](https://github.com/DikshitRJ/arkivector/wiki) on every push to `main`, so all three always match. Navigation comes from [`SUMMARY.md`](https://github.com/DikshitRJ/arkivector/blob/main/docs/wiki/SUMMARY.md). To fix a page, edit the file and open a pull request.

The formal API contract (OpenAPI, error codes, golden test vectors) is in [`spec/`](https://github.com/DikshitRJ/arkivector/tree/main/spec) and [`docs/api/`](https://github.com/DikshitRJ/arkivector/tree/main/docs/api).
