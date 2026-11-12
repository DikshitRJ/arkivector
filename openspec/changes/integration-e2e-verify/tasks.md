# integration-e2e-verify: tasks

Branch `integration/e2e-verify` (from `main` @ 886e8b6, after the daemon and SDK merges).
Goal: CI green, PLAN_DAEMON D0–D6 and PLAN_SDK S0–S6 (Python, Node) actually implemented,
daemon + SDKs verified end to end against mock-arkiv, then Rabdi.
Sources: three read-only audits (daemon plan, SDK plan, math) run 2026-11-07.

Legend: [x] done · [ ] open · (decision) = approach chosen.

## A. CI pipeline (`.github/workflows/daemon.yml`)
- [x] clang build: `-Wgnu-statement-expression-from-macro-expansion` (clang-only flag in cmake/Warnings.cmake), eval.c conversion
- [x] provenance: hnsw.h restored to upstream (pedantic pragma moved into our hnswscan_mem.h)
- [x] clang-format applied (23.1.4, same as CI pin)
- [x] clang-tidy: real fixes (keystore hardening, file reads via core/fileio.c, http UAF order, null guards,
      cursor parse, atoi/sscanf, duplicated TypedValue JSON → filter/value_json.c); 2 idiomatic checks disabled with reasons
- [ ] clang-tidy version pinned in CI (local 23.x vs apt 18)
- [ ] cppcheck clean
- [x] integration job builds the dev binary BEFORE pytest (today daemon suites silently skip)
- [x] unit tests ASan/UBSan + TSan green (TSan root cause: pgvector ifunc resolvers; see daemon-hnsw-ci-fix)
- [ ] integration, contract, parity, fuzz smoke green (integration, parity, fuzz smoke green in daemon-hnsw-ci-fix; contract tests not in CI yet)

## B. Math (daemon)
- [x] B1 HNSW returns index distance (L2², -ip) as API distance → recompute true operator distance for returned hits
- [x] B2 cosine HNSW segments store normalized vectors; flat stores raw → keep raw vectors for exact/non-index operators
- [x] B3 zero-norm vectors: NaN distances break JSON → reject zero vectors for cosine insert/query (pgvector skips them in HNSW), never emit NaN (graph skips them, NaN hits dropped; inserts are not rejected)
- [x] B4 strict_order guard broken for negative distances (previous_distance = -1) → -INFINITY like upstream
- [ ] B5 Qdrant Manhattan score sign
- [x] B6 HNSW tie-break by key (search.md:37), not docid
- [ ] B7 TTL: validate before converting; odd seconds rejected; 0 lifetime rejected
- [ ] B8 ef_search ≥ offset+k; iterative bump uses offset+k; Qdrant limit/offset bounds (ef_search/iterative bump done in daemon-hnsw-ci-fix; Qdrant bounds open)
- [x] B9 iterative_scan off + empty first batch null deref (hnswscan_mem.c)

## C. Daemon features (plan gaps)
- [x] C1 HNSW reachable: collections with an hnsw index build/seal HNSW segments (today always FLAT)
- [ ] C2 multi-segment collections + expiry-banded compaction + compactor thread + TTL segment drop (D4.6, D4.7)
- [ ] C3 sealed segments reloaded on restart
- [ ] C4 calibration runs at start; planner uses measured constants
- [ ] C5 capabilities derived from what is implemented/configured (writes only with a signer)
- [ ] C6 write path: entityNonce via eth_call 0x36917bfd for key prediction
- [ ] C7 prepare: chain nonce, eth_estimateGas, fee from eth_feeHistory/eth_gasPrice
- [ ] C8 signer wiring from config (`[signer]` env/keystore+password file/remote); remote signing implemented or rejected at start
- [ ] C9 Qdrant upsert/delete/count real (or unadvertised + 501)
- [ ] C10 manifest publishing writes the entity on chain (or 501 when no signer)
- [ ] C11 membership=open: discovery B/C wired or rejected with not_implemented
- [ ] C12 recall canary: configurable, off the request thread, fires for approximate plans
- [ ] C13 metrics: reconcile divergence, RPC budget, sync/apply/ttl
- [ ] C14 scroll cursor: HMAC + expiry (1 h, search.md:73)
- [ ] C15 rabdi-probe.py really measures; RABDI_FACTS.md regenerated from a real run (needs testnet access)
- [ ] C16 empty planned files (sync/{apply,bootstrap,reorg,reconcile}, arkiv/curl_uv, query/rerank) removed or filled; docs updated

## D. SDKs
- [ ] D1 Python follow_collection sends `default_ttl: null`
- [ ] D2 Python pooled connections ignore per-request timeout
- [ ] D3 Python upsert/prepare send typed vectors as JSON (should be vector_bytes when type == column type)
- [ ] D4 Python HalfVector skips float32 rounding before binary16
- [ ] D5 Node User-Agent `arkivector-node/<ver>`
- [ ] D6 Node direct-write av.vec ignores column type; schemaTag default
- [ ] D7 Node missing capability checks; get() drops as_of_block (both)
- [ ] D8 golden_check online treats mock responses as PASS (both); quickstart --write must assert found/gone
- [ ] D9 LlamaIndex adapter pydantic field; LangChain get_by_ids; LangChain.js extends VectorStore
- [ ] D10 packaging: build/twine/npm pack checks, prettier in CI, @eslint/js devDep, repo URLs
- [ ] D11 percent-encoding of refs (Node encodeURIComponent leaves !'()*)

## E. End to end
- [ ] E1 dev-up.sh + SDK golden_check online (both) PASS
- [ ] E2 quickstart --write round trip (both) against `dev-up.sh --signer dev`
- [ ] E3 RAG examples run
- [ ] E4 release-static binary builds and serves
- [ ] E5 Rabdi: probes, then soak (after owner provides wallet/access key)
