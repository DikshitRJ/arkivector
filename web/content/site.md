# Arkivector

> Vector search for Arkiv. Arkivector runs pgvector's distance functions over entities stored on the Arkiv chain. Your embeddings are written on chain by your wallet, and you query them with one REST call, from Python or Node.js, or from an AI agent over MCP.

This file is the whole website ({{SITE}}) as Markdown, written for AI agents and LLMs. It is generated at build time from the same sources as the site. All documentation on one page: [{{SITE}}/docs.md]({{SITE}}/docs.md). Index in the llms.txt format: [{{SITE}}/llms.txt]({{SITE}}/llms.txt).

- Current release: **v{{VERSION}}**
- Source: <https://github.com/DikshitRJ/arkivector>
- Install guide: [{{SITE}}/docs/install]({{SITE}}/docs/install) (Markdown: [{{SITE}}/docs/install.md]({{SITE}}/docs/install.md))
- License: MIT. The copied pgvector code keeps its PostgreSQL License.

## Quick facts for agents

- **What it is.** `arkivectord` is a daemon written in C11 that contains pgvector 0.8.7's own code. It syncs entities from the Arkiv chain into a local index (LMDB rows, mmap vector segments, roaring-bitmap filters, HNSW graphs) and serves REST on port 8080. It writes by building, signing and sending Arkiv transactions.
- **Clients.** A Python SDK (`pip install arkivector`), a Node.js SDK (`@dikshitrj/arkivector` on GitHub Packages), plain HTTP (`curl`), and an MCP server at `/mcp` for AI agents (Claude Code: `claude mcp add --transport http arkivector http://127.0.0.1:8080/mcp`).
- **Run it.** `docker run -d -p 127.0.0.1:8080:8080 -v arkivector-data:/data -e ARKIVECTOR_ARKIV_ACCESS_KEY=... -e ARKIV_PRIVATE_KEY ghcr.io/dikshitrj/arkivectord:latest`. Without `ARKIV_PRIVATE_KEY` the daemon is read-only.
- **Vector types** (same as pgvector): `vector`, `halfvec`, `sparsevec`, `bit`.
- **Distance operators:** `<->` L2, `<#>` negative inner product, `<=>` cosine, `<+>` L1, `<~>` Hamming, `<%>` Jaccard.
- **Indexes:** `hnsw` (pgvector's graph) or `none` (exact scan). The planner picks HNSW for large candidate sets and an exact scan for small or very selective ones. Every hit is rescored with the real operator. `"exact": true` forces a full scan.
- **Collections need `writers`.** A collection only indexes entities created by the wallets in its `writers` list. Put the daemon's own wallet address there, or your writes land on chain but never show up in search.
- **Chain:** Arkiv Rabdi testnet, chain id 7794095, 2 s blocks, RPC `https://rpc.rabdi.db-chain.testnet.arkiv.network`.

## Home

### Why it exists

**pgvector made retrieval boring.** pgvector is the Postgres extension that turned "where do I keep my embeddings?" into `ORDER BY embedding <=> $1 LIMIT 5`. It has more than 23,000 GitHub stars and sits under a large share of RAG and agent stacks, because it adds vector search to a database people already run.

**Arkiv had no vector search.** Arkiv is a chain built for data: entities with typed attributes, an owner, and an expiry block, queried with a filter language. You could store an embedding there as bytes, but you could not ask which entities are closest to it. Arkivector adds that, using pgvector's own C code for the math.

### Search is geometry

An embedding model places texts with similar meaning close together. A query becomes a vector too, and search asks which stored vectors point in nearly the same direction. Cosine distance is `d = 1 − cos θ`; with `k = 2` the two smallest distances are returned. Real embeddings have hundreds of dimensions, so the angle is computed over all of them, but the arithmetic is the same.

### Numbers we can back up

| Number | Meaning |
| :--- | :--- |
| 97 | Parity tests against pgvector 0.8.7 running in Postgres. Every distance is bit-identical or within float32 rounding |
| 4 | Vector types, same as pgvector: vector, halfvec, sparsevec, bit |
| 6 | Distance operators: `<->` L2, `<#>` negative inner product, `<=>` cosine, `<+>` L1, `<~>` Hamming, `<%>` Jaccard |
| < 3 s | From upsert to mined transaction on the Arkiv Rabdi testnet in our end-to-end run |
| 18 MB | Container image. One daemon, written in C11, plus thin Python and Node.js SDKs |

### Store, then search

```python
from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)
client.upsert("agent.memory", [
    EntityInput(id="cat", vector=[1, 0, 0, 0], document="Cats purr.", attributes={"topic": "pets"}),
], wait="receipt")      # signed, sent to Arkiv, mined
res = client.search("agent.memory", vector=[1, 0.05, 0, 0], k=2, filter=F.eq("topic", "pets"))
print([(h.id, h.distance) for h in res.hits])
```

The daemon builds the Arkiv transaction, signs it, waits for it to be mined, and indexes the entity once it reads it back from the chain. With `session_consistency`, the search waits for that block, so you always see your own write.

## Architecture

Page: [{{SITE}}/architecture]({{SITE}}/architecture). A write, a sync and a search followed through the daemon, then the math that ranks the results.

### The big picture

Your code talks to `arkivectord` through an SDK, curl or MCP. The daemon writes to Arkiv with signed transactions and reads entities back into its local index. Search never leaves the daemon. The chain is the source of truth.

| Component | Role |
| :--- | :--- |
| HTTP | REST and MCP on a libuv event loop, port 8080 |
| writer | Encodes Arkiv operations (ABI), signs with secp256k1, sends transactions to the Arkiv contract (`execute(ops)`) |
| planner | Chooses HNSW or an exact scan and runs pgvector's distance kernels |
| filters | Attribute filters as roaring bitmaps |
| store | LMDB for rows, mmap'd vector segments |
| syncer | Follows the chain (`arkiv_query`, `eth_getLogs`), handles reorgs and expiry |

### The story of one entity

1. **The parts.** SDK, daemon, chain.
2. **A write.** The vector becomes VectorBytes inside an Arkiv CREATE operation. For `halfvec [1, 0, 0, 0]` the bytes are `41 56 12 00 04 00 00 00 00 3c 00 00 00 00 00 00`: `'AV'`, type 2 = halfvec, dimension 4, then 1.0 as IEEE binary16.
3. **Signed and sent.** The transaction is signed with secp256k1 and mined in a block.
4. **Synced back.** The syncer reads the entity back from the chain, then indexes it. Every response carries `as_of_block`.
5. **A search.** Filter bitmap AND live bitmap, score the survivors with the operator (e.g. `<=>` cosine), keep the best k.
6. **The arithmetic.** For `q = [1, 0.05, 0, 0]`, `cat = [1, 0, 0, 0]` gives `d = 1 − 1/1.00125 = 0.00125`, and `dog = [0.9, 0.1, 0, 0]` gives `d = 1 − 0.905/(0.9055·1.00125) = 0.00184`. L2 gives the same order (cat 0.050, dog 0.112).
7. **16 bits per number.** `halfvec` stores 0.1 as binary16 `0 01011 1001100110` = `2^(11−15) · (1 + 614/1024)` = 0.0999755859375, 0.024% off float32 with half the bytes, rounded half to even with pgvector's own conversion code.
8. **Exact scan and HNSW.** An exact scan computes n distances and keeps a top-k heap (O(n log k)). HNSW (M = 16 links, `ef_search` = 40 by default) descends the graph layers (`ℓ = ⌊−ln U · m_L⌋`, `m_L = 1 / ln M`) and visits a small fraction of the nodes. Since v1.1.0 every `hnsw` collection searches a real pgvector graph.
9. **Expiry.** When the chain passes an entity's expiry block, the entity leaves the index.

## Build artifacts

Page: [{{SITE}}/build]({{SITE}}/build). Where every piece is published.

### How a release is made

One workflow, `.github/workflows/release.yml`, runs on every `v*` tag (or a manual dispatch): build (binary, image, wheel, npm package), smoke test (the binary and image report the version), publish (GitHub Release, GHCR, PyPI, GitHub npm). PyPI uses trusted publishing, so there is no token in the repository.

### Artifacts

**Python SDK** ([pypi.org/project/arkivector](https://pypi.org/project/arkivector/)). Pure Python, no dependencies. Wheel and sdist are also attached to every GitHub release.

```bash
pip install arkivector=={{VERSION}}
```

**Node.js SDK** (GitHub Packages, `@dikshitrj/arkivector`). ESM, TypeScript types, zero runtime dependencies. GitHub Packages asks for a token with `read:packages`; the tarball is also attached to each release.

```bash
echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
npm install @dikshitrj/arkivector@{{VERSION}}
```

**Daemon, container image** (`ghcr.io/dikshitrj/arkivectord`). 18 MB Alpine image, linux/amd64, runs as a non-root user, index in the `/data` volume. Tags `:{{VERSION}}`, `:1`, `:latest`.

```bash
docker run -d -p 127.0.0.1:8080:8080 -v arkivector-data:/data \
  -e ARKIVECTOR_ARKIV_ACCESS_KEY=... -e ARKIV_PRIVATE_KEY \
  ghcr.io/dikshitrj/arkivectord:{{VERSION}}
```

**Daemon, Linux binary** (GitHub Releases). x86_64 glibc build with libcurl bundled; needs libuv and OpenSSL 3 from the OS.

```bash
curl -LO https://github.com/DikshitRJ/arkivector/releases/download/v{{VERSION}}/arkivectord-{{VERSION}}-linux-x86_64.tar.gz
curl -LO https://github.com/DikshitRJ/arkivector/releases/download/v{{VERSION}}/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
tar xzf arkivectord-{{VERSION}}-linux-x86_64.tar.gz
```

**Arkiv Rabdi testnet** (not something we ship: the chain the daemon reads from and writes to). Chain id 7794095 (`0x76edaf`), 2 s blocks, RPC `https://rpc.rabdi.db-chain.testnet.arkiv.network`. Test GLM from the faucet at <https://hub.arkiv.network/faucet>.

## Roadmap

Tracked on GitHub: <https://github.com/DikshitRJ/arkivector/issues/19>. Everything in the install guide works and was tested end to end, including on the Rabdi testnet. These are the parts that are not finished yet.

| Limitation | Today | Plan | Issue |
| :--- | :--- | :--- | :--- |
| The daemon signs with a key in an environment variable | The daemon holds a hot wallet (`ARKIV_PRIVATE_KEY`) | `entities:prepare` already returns unsigned transactions keyed to the wallet's real nonces; next the SDKs hand them to the user's wallet (MetaMask, WalletConnect, hardware), plus encrypted keystores and remote signers | [#12](https://github.com/DikshitRJ/arkivector/issues/12) |
| Tested on the Rabdi testnet only | Mainnet is configurable but untried | Run the sync and write suites on mainnet and measure gas reservations there | [#16](https://github.com/DikshitRJ/arkivector/issues/16) |
| Linux and Docker only | macOS and Windows use the container | Port the glibc-specific calls | [#17](https://github.com/DikshitRJ/arkivector/issues/17) |
| Two SDK languages | SDKs for Python and Node.js | C, C++ and Rust SDKs | [#18](https://github.com/DikshitRJ/arkivector/issues/18) |

Done since v1.1.0:

| Fixed | What changed | Issue |
| :--- | :--- | :--- |
| HNSW graphs survive restarts | Graphs are saved per expiry band under `data_dir/segments` and loaded at startup (only changes since the save are indexed); rebuilds run on a background thread while sync continues; expired bands are dropped whole; corrupt files are rejected and rebuilt | [#11](https://github.com/DikshitRJ/arkivector/issues/11) |
| Entity keys follow the wallet's on-chain nonce | Writes read `entityNonce` before predicting keys and take final keys from the `EntityCreated` receipt logs; `entities:prepare` uses the wallet's real nonces | [#13](https://github.com/DikshitRJ/arkivector/issues/13) |
| Dynamic gas fees | Fees from `eth_feeHistory` (then `eth_gasPrice`, then config) with a cap; gas limit = estimate + margin; stuck transactions are resent with bumped fees | [#14](https://github.com/DikshitRJ/arkivector/issues/14) |
| Qdrant writes are real | Upserts and deletes go through the REST writer, searches return payloads, a daemon without a signer refuses writes | [#15](https://github.com/DikshitRJ/arkivector/issues/15) |

## Team

Page: [{{SITE}}/team]({{SITE}}/team). One person built Arkivector for ETHGlobal Mumbai 2026: Dikshit Rishi Jain (NITK Surathkal), <https://github.com/DikshitRJ>. Questions, bugs or ideas: open a GitHub issue at <https://github.com/DikshitRJ/arkivector/issues>.

## Documentation index

Every page also exists as Markdown: add `.md` to its URL (the docs home is `/docs/index.md`). All of them together: [{{SITE}}/docs.md]({{SITE}}/docs.md).

{{DOCS_INDEX}}
