# Changelog

All notable changes to Arkivector (daemon `arkivectord`, Python SDK `arkivector`, Node SDK `@dikshitrj/arkivector`). Versions follow the release tags; the REST API contract is at `1.1.0` (`spec/VERSION`, [contract changelog](docs/api/changelog.md)).

## 1.2.0 (2026-11-13)

### Daemon
- **HNSW graphs survive restarts** ([#11](https://github.com/DikshitRJ/arkivector/issues/11)). Graphs are grouped into expiry bands (`[hnsw] band_blocks`, default 30 days) and saved to `<data_dir>/segments/<collection>/<band>.hnsw` after background builds, at checkpoints and at shutdown (`[hnsw] persist`). At start each file is validated completely and only vectors added since the save are indexed. A corrupt or mismatched file is logged, deleted and rebuilt in the background, never a crash (fuzzed: `fuzz_hnsw_file`).
- **Rebuilds run in the background**: on a worker thread, while the sync thread keeps applying blocks; the new graph catches up and is swapped in. Startup no longer builds graphs synchronously.
- **Expired bands are released whole**, without tombstone work. `stats.live_entities` and search results never include expired rows, even while TTL reclamation runs behind. `stats.segments` counts the flat segment plus the bands.
- **Entity keys follow the wallet's on-chain nonce** ([#13](https://github.com/DikshitRJ/arkivector/issues/13)). Writes read `entityNonce` (`eth_call` at `pending`) and track their own in-flight creates; `wait: "receipt"`/`"indexed"` returns the keys from the `EntityCreated` receipt logs; `wait: "none"` fails with `arkiv_rpc_error` when the nonce cannot be read. `entities:prepare` uses the wallet's entityNonce and pending transaction count (it used nonce = batch index).
- **Dynamic gas** ([#14](https://github.com/DikshitRJ/arkivector/issues/14)). Fees from `eth_feeHistory`, then `eth_gasPrice`, then the config, capped by `write.max_fee_cap_gwei`. Gas limit = `eth_estimateGas` + `write.gas_margin_pct` (default 20). The fallback (60,000 + 150,000 per op, was 500,000 + 130,000 per op) is logged and counted. Stuck transactions are resent with the same nonce and fees +12.5% (`write.replace_after_blocks`, `write.max_replacements`). A failed send, revert or timeout resyncs the nonces instead of leaving a gap. New `[write]` section and write metrics.
- **Qdrant writes are real** ([#15](https://github.com/DikshitRJ/arkivector/issues/15)). `PUT …/points` and `POST …/points/delete` go through the managed writer (they returned success without writing). Search and query return payloads, honour `filter`, `with_payload`, `with_vector` and `score_threshold`; `/points/query` answers `{"points": [...]}`; count takes a filter. Without a signer writes return `403 write_disabled`. Collections created through `/qdrant` get a 30-day `default_ttl`. `GET /qdrant` reports a Qdrant API version; retrieve and scroll answer `501`.
- The router accepts one trailing slash; responses can carry a `Warning` header.
- **Sign with your own wallet** ([#12](https://github.com/DikshitRJ/arkivector/issues/12), contract 1.1.0, [CCR-0001](spec/changes/CCR-0001-client-side-signing.md)). `entities:prepare` transactions now carry `type`, `chain_id`, `from`, `gas` and a fee suggestion, so they can be signed as is. New `POST /v1/collections/{name}/entities:submit` (capability `entities.submit`) takes back raw signed transactions (validated before broadcast: canonical EIP-1559, low-s, chain id, Arkiv contract, `execute` calldata, sender in `writers`) or the hashes a wallet broadcast, waits for receipts and indexing, and returns the created keys. No daemon key needed.
- **Keystore and remote signers.** New `[write.signer]` section (alias `[signer]`): `kind = auto | none | env | keystore | remote`. Keystores are Web3 Secret Storage v3 (scrypt/pbkdf2) with the passphrase from a file or a systemd credential. The remote signer calls `eth_signTransaction` (Web3Signer, geth) or `account_signTransaction` (Clef) and decodes the returned transaction, rejecting it unless every field and the recovered address match. An explicit kind that fails stops startup. `GET /v1/version` reports `signer: {kind, address}`.

### SDKs
- Python: `Client.submit`, `Client.upsert_signed(..., sign=| send=)`, `arkivector.signing` (`eth_account_signer`, `web3_sender`), `PreparedTransaction.to_eth_account()` / `.to_rpc()`, `SubmitResult`, `FeeSuggestion`, `VersionInfo.signer`. Example `examples/sign_with_wallet.py`.
- Node: `submit`, `upsertSigned({ signer })` for EIP-1193 providers (MetaMask), viem `WalletClient`, ethers `Signer` or a function; converters `toRpcTransaction`, `toViemTransaction`, `toEthersTransaction`; `SubmitResult`, `VersionInfo.signer`. Example `examples/wallet_server.ts` (browser page signing with MetaMask).

### Testing
- New integration suite `test_signing.py` (client-signed submit, wallet-broadcast hashes, rejections, keystore and remote signers), unit suite `rawtx`, fuzz target `fuzz_rawtx`.
- mock-arkiv: `eth_feeHistory`, `/_mock/fees` (fee answers, held underpriced transactions and replacement rules).
- New integration suites: `test_write_keys_fees.py`, `test_qdrant_client.py` (official `qdrant-client`), `test_hnsw_persist.py`; unit suites `fees` and `hnsw_file`; fuzz targets `fuzz_qdrant`, `fuzz_hnsw_file`, and `eth_feeHistory` in `fuzz_rpc_response`.

## 1.1.0 (2026-11-12)

### Daemon
- **HNSW works.** Collections created with `"index": {"method": "hnsw"}` now build and search a pgvector HNSW graph. Before, every search was an exact scan. The planner uses the graph for large candidate sets and an exact scan for small or very selective ones, and falls back to exact if a graph walk finds fewer than `k` matches.
- Approximate results report the real operator distance (L2, negative inner product, cosine) on the stored vector, sorted by distance with ties broken by key, the same as exact results.
- `ef_search`, `iterative_scan` and `max_scan_tuples` on search requests are honoured and validated (they were accepted but ignored).
- Fixed a crash on HNSW searches with `iterative_scan: "off"` and on empty graphs.
- Zero vectors under cosine are kept out of the graph (as in pgvector) and never produce NaN distances.
- Errors raised inside pgvector code no longer abort the daemon.
- Unfiltered exact searches (Qdrant and MCP endpoints) no longer score replaced vector versions.
- **MCP server works.** Every `/mcp` tool now runs the matching REST endpoint, so search returns distances, attributes and documents and accepts `where`, `filter` and `include_document`; `get` and `count` return full data; `arkivector_upsert` is available when the daemon has a signer. Results carry `structuredContent`. API-key scopes are checked per tool, `ping` and notifications (202) are handled, protocol versions are negotiated, and non-localhost `Origin` headers are rejected unless API keys are on.
- The HNSW graph pointer is read only under its lock (a benign data race found in review).

### Build and CI
- ThreadSanitizer builds disable pgvector's CPU dispatch. Its ifunc resolvers ran before the TSan runtime and crashed every TSan binary, which had kept the integration and parity suites from running in CI.
- Nightly fuzzing fits in its job timeout.
- New end-to-end HNSW integration tests (recall, distances, filters, deletes, iterative scan modes).

### Documentation
- New documentation in [`docs/wiki/`](docs/wiki/README.md), one page per topic: getting started, the REST API endpoint by endpoint (curl, requests, responses), the Python and Node.js SDKs, the MCP server, architecture, operations and development. The same pages are rendered at [arkivector.dikshitrj.in/docs](https://arkivector.dikshitrj.in/docs) and published to the [GitHub wiki](https://github.com/DikshitRJ/arkivector/wiki).
- SDK package pages (PyPI, npm) now include a quick start.

## 1.0.2, 1.0.1, 1.0.0 (2026-11-06 to 2026-11-07)

First releases: daemon with exact search, filters, writes on the Rabdi testnet, Qdrant and MCP endpoints; Python and Node SDKs; container image on GHCR.
