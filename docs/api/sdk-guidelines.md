# SDK Guidelines (normative for `sdk/*`)

All five SDKs (C, C++, Rust, Python, Node.js) are **thin HTTP clients** of the daemon. None of them computes distances, builds indexes, or talks to Arkiv. They MUST:

1. implement every operation in §2 with the per-language names in §3;
2. encode and decode **VectorBytes v1** and pass all 29 cases in `spec/golden/vectorbytes.json`;
3. provide the four pgvector value types (`Vector`, `HalfVector`, `SparseVector`, `Bit`), `TypedValue` constructors, and a filter builder that emits the JSON AST of [filters.md](filters.md);
4. follow [conventions.md](conventions.md) (headers, retries, idempotency keys, timeouts) and [errors.md](errors.md) (error model);
5. check `GET /v1/version` capabilities before optional features;
6. ship `examples/quickstart.*` and `examples/golden_check.*` (§6).

## 1. Client configuration (all languages)

| Option | Default | Env var |
| :--- | :--- | :--- |
| `url` | `http://127.0.0.1:8080` | `ARKIVECTOR_URL` |
| `api_key` | none | `ARKIVECTOR_API_KEY` |
| `timeout` | 10 s (reads), 30 s (writes waiting) | — |
| `max_retries` | 3 (idempotent ops only, or writes with an Idempotency-Key) | — |
| `session_consistency` | false. When true, remember the max `block_number` from own writes and send it as `consistency.min_block` | — |
| `user_agent` | `arkivector-<lang>/<version>` | — |
| `idempotency` | true. Auto-generate a UUIDv4 `Idempotency-Key` per write call | — |

Clients are cheap to create, safe for concurrent use (where the language allows it), and lazily fetch and cache `/v1/version`. A major mismatch (`api_version` major ≠ 1) raises a configuration error on first use.

## 2. Canonical operations

| Op | HTTP | Capability | Returns |
| :--- | :--- | :--- | :--- |
| `version` | GET /v1/version | — | VersionInfo |
| `health` | GET /healthz | — | bool |
| `ready` | GET /readyz | — | Readiness |
| `sync_status` | GET /v1/sync | ops.sync | SyncStatus[] |
| `list_collections` | GET /v1/collections | collections.read | Collection[] |
| `get_collection` | GET /v1/collections/{name} | collections.read | Collection |
| `create_collection` | POST /v1/collections | collections.write | Collection |
| `follow_collection` | POST /v1/collections:follow | collections.follow | Collection |
| `delete_collection` | DELETE /v1/collections/{name} | collections.write | void |
| `get` | GET …/entities/{ref} | entities.read | Entity (raises NotFound) |
| `search` | POST …/search | search.exact | SearchResult `{as_of_block, sync_state, open_collection, hits, plan}` |
| `search_batch` | POST …/search:batch | search.batch | BatchResult `{as_of_block, sync_state, results: SearchResult[]}` |
| `scroll` | POST …/scroll, auto-paging | scroll | lazy iterator / stream of Entity |
| `scroll_page` | POST …/scroll, one page | scroll | `{entities, next_cursor, as_of_block}` |
| `count` | POST …/count | count | `{count, as_of_block}` |
| `upsert` | POST …/entities | entities.write | UpsertResult |
| `prepare` | POST …/entities:prepare | entities.prepare | PrepareResult (transactions ready to sign; helpers convert them for eth_account / EIP-1193 / viem / ethers) |
| `submit` | POST …/entities:submit | entities.submit | SubmitResult `{tx_hashes, block_number, created_keys}` |
| `upsert_signed` | prepare → caller's signer → submit | entities.prepare + entities.submit | UpsertResult (keys from the receipts). Default `wait: "indexed"` |
| `delete` | POST …/entities:delete, or DELETE …/{ref} for a single ref | entities.write | TxResult |
| `extend` | POST …/entities:extend | entities.write | TxResult |

Search parameters map 1:1 to `SearchRequest` fields. `vector` accepts the SDK's vector types or plain float arrays. Dense vectors are sent as `vector_bytes` when the caller passes a typed vector whose type equals the column type (known from `get_collection`, cached per collection). Otherwise they are sent as JSON arrays.

## 3. Per-language surface

### Python (`sdk/python`, package `arkivector`, Python ≥ 3.9, stdlib only)

```python
from arkivector import Client, Vector, HalfVector, SparseVector, Bit, F, T, Ttl
c = Client(url=None, api_key=None, timeout=10.0, max_retries=3, session_consistency=False)
c.version(); c.health(); c.ready(); c.sync_status()
c.list_collections(); c.get_collection("demo.docs")
c.create_collection("demo.docs", column=("halfvec", 8), index={"method": "hnsw", "opclass": "halfvec_cosine_ops"},
                    writers=[...], attributes={"category": "str"})
c.follow_collection(manifest_key="0x…")                 # or definition={...}
c.delete_collection("demo.docs")
c.get("demo.docs", "doc-040", include=("attributes", "document"), min_block=None)
r = c.search("demo.docs", HalfVector([...]), k=5, filter=F.eq("category", "finance"), exact=False,
             include=("attributes",), explain=False)   # also: where="…", operator="<=>", ef_search=…, max_distance=…
r.hits[0].key, r.hits[0].distance, r.hits[0].attributes["category"].value, r.as_of_block
c.search_batch("demo.docs", [dict(vector=..., k=5), ...])
for e in c.scroll("demo.docs", filter=F.gte("score", T.dec("4.0")), limit=100): ...
c.count("demo.docs", where="published = true").count
c.upsert("demo.docs", [{"id": "x", "vector": [...], "attributes": {...}, "ttl": Ttl.days(30)}], wait="receipt")
c.prepare("demo.docs", entities, from_address="0x…"); c.delete("demo.docs", refs=["x"]); c.extend("demo.docs", Ttl.days(30), refs=["x"])
# Client-side signing (no key on the daemon). sign returns raw signed bytes/hex; send returns a tx hash.
p = c.prepare("demo.docs", entities, from_address=acct.address)
raws = [acct.sign_transaction(tx.to_eth_account()).raw_transaction for tx in p.transactions]
c.submit("demo.docs", signed_transactions=raws, wait="indexed")      # or tx_hashes=[...]
c.upsert_signed("demo.docs", entities, from_address=acct.address, sign=eth_account_signer(acct))
```
Extras (lazy imports): `arkivector.langchain.ArkivectorVectorStore` (LangChain `VectorStore`), `arkivector.llama_index.ArkivectorVectorStore` (LlamaIndex `BasePydanticVectorStore`). numpy arrays are accepted when numpy is installed. pgvector-python objects (`pgvector.Vector` & co.) are accepted when installed.

### Node.js / TypeScript (`sdk/node`, package `@arkivector/client`, Node ≥ 18, zero runtime deps, ESM)

```ts
import { Arkivector, Vector, HalfVector, SparseVector, Bit, F, T, Ttl } from "@arkivector/client";
const c = new Arkivector({ url, apiKey, timeoutMs: 10_000, maxRetries: 3, sessionConsistency: false, fetch });
await c.version(); await c.listCollections(); await c.getCollection("demo.docs");
await c.createCollection({ name, column: { type: "halfvec", dimensions: 8 }, index: { method: "hnsw", opclass: "halfvec_cosine_ops" } });
const r = await c.search("demo.docs", { vector: new HalfVector([...]), k: 5, filter: F.eq("category", "finance") });
for await (const e of c.scroll("demo.docs", { where: "published = true" })) { … }
await c.count("demo.docs", { filter }); await c.upsert("demo.docs", entities, { wait: "receipt" });
await c.prepare("demo.docs", entities, { from }); await c.delete("demo.docs", { refs }); await c.extend("demo.docs", Ttl.days(30), { refs });
// Client-side signing: signer = EIP-1193 provider (window.ethereum), viem WalletClient, ethers Signer,
// or (tx) => Promise<rawTx | txHash>.
await c.upsertSigned("demo.docs", entities, { signer: window.ethereum, wait: "indexed" });
const p = await c.prepare("demo.docs", entities, { from }); /* sign p.transactions anywhere */
await c.submit("demo.docs", { signedTransactions: raws }); // or { txHashes }
```
camelCase in the API, snake_case on the wire. LangChain.js adapter at subpath `@arkivector/client/langchain` (peer dependency `@langchain/core`).

### Rust (`sdk/rust`, crate `arkivector`, MSRV 1.75)

```rust
use arkivector::{Client, HalfVector, F, T, SearchRequest, Ttl};
let c = Client::builder().url("http://127.0.0.1:8080").api_key(key).build()?;   // async (tokio)
let r = c.search("demo.docs", SearchRequest::new(HalfVector::from_f32(&q)).k(5).filter(F::eq("category", "finance"))).await?;
let mut s = c.scroll("demo.docs", ScrollRequest::default().filter(f));   // impl Stream<Item = Result<Entity>>
// feature "blocking": arkivector::blocking::Client with identical method names
```
Dependencies: `reqwest` (rustls, json), `serde`, `serde_json`, `half`, `base64`, `thiserror`, `uuid`, `futures-core`. Features: `blocking`, `native-tls` (alternative TLS).

### C (`sdk/c`, library `libarkivector`, C11, deps: libcurl + vendored yyjson)

```c
#include <arkivector/arkivector.h>
av_config_t cfg = AV_CONFIG_INIT;            /* url, api_key, timeout_ms, max_retries, ... */
av_error_t err = {0};
av_client_t *c = av_client_new(&cfg, &err);
av_vector_t *q = av_halfvec_from_f32(vals, 8, &err);
av_filter_t *f = av_f_eq("category", av_v_str("finance"));
av_search_params_t p = AV_SEARCH_PARAMS_INIT; p.vector = q; p.k = 5; p.filter = f;
av_search_result_t *res = NULL;
if (av_search(c, "demo.docs", &p, &res, &err) != AV_OK) { fprintf(stderr, "%s: %s\n", err.code, err.detail); }
for (size_t i = 0; i < res->n_hits; i++) printf("%s %f\n", res->hits[i].key, res->hits[i].distance);
av_search_result_free(res); av_filter_free(f); av_vector_free(q); av_client_free(c); av_error_clear(&err);
```
Rules: every allocated object has a matching `*_free`. Functions return `av_status_t` (`AV_OK` = 0) and fill `av_error_t` (code, status, title, detail, request_id, retryable, raw problem JSON). Thread-safety: an `av_client_t` may be shared across threads (one curl easy handle per call, or a pool). No global state except `av_global_init()`/`av_global_cleanup()` wrapping `curl_global_init`. Exported via CMake target `arkivector::c` and `arkivector.pc`.

### C++ (`sdk/cpp`, C++17, wraps the C SDK)

```cpp
#include <arkivector/arkivector.hpp>
arkivector::Client c({.url = "http://127.0.0.1:8080"});
auto r = c.search("demo.docs", arkivector::SearchRequest{}.vector(arkivector::HalfVector(q)).k(5)
                                   .filter(arkivector::F::eq("category", "finance")));
for (const auto& h : r.hits) std::cout << h.key << " " << h.distance << "\n";
for (const auto& e : c.scroll("demo.docs", arkivector::ScrollRequest{}.where("published = true"))) { … }  // input range
```
RAII over the C handles. Errors are thrown as `arkivector::Error` subclasses (§errors). Value types use `std::vector<float>`, `std::optional`, `std::string`. CMake target `arkivector::cpp` links `arkivector::c`.

## 4. Value helpers (all languages)

- **TypedValue constructors:** `T.bool(x) T.i32(x) T.u64("…") T.u256("…") T.dec("4.5") T.str("…") T.addr("0x…") T.key("0x…") T.bytes(b)` (C: `av_v_*`; Rust: `T::dec("4.5")`). Responses decode into a `TypedValue {type, value}` with accessors (`as_str()`, `as_dec_string()`, `as_i64()`, `as_bool()` …). Python and TS also offer `entity.plain_attributes()`, which returns native scalars (dec/u64/u256 stay strings).
- **Filter builder `F`:** `and_ or_ not_ eq ne lt lte gt gte between startswith exists typeof` (Python uses trailing underscores for keywords. Rust/C++ use `F::and_`/`F::all`; C uses `av_f_and(n, ...)`). The output is the JSON AST. A plain string passed as `where` is sent verbatim.
- **TTL `Ttl`:** `Ttl.blocks(n) Ttl.seconds(n) Ttl.days(x) Ttl.at_block(n)`.

## 5. Vector types (all languages)

| Type | Constructor inputs | Methods |
| :--- | :--- | :--- |
| `Vector` | float list/array | `to_list()`, `to_text()` (`[1,2,3]`), `to_vectorbytes(normalized=False)`, `from_vectorbytes(b)`, `dim` |
| `HalfVector` | float list (rounded to binary16 round-half-even) | same |
| `SparseVector` | `(dim, indices, values)`, or a dict `{index: value}` + dim (0-based), or pgvector text | `to_text()` is 1-based like pgvector, `to_dict()`, … |
| `Bit` | `"1010…"` string, bool list, or bytes + length | `to_text()`, `to_bytes()`, … |

`from_vectorbytes` validates in the order given in [vectors.md §3.3](vectors.md#33-validation-order-decoders-must-check-in-this-order-and-report-the-first-failure) and raises `VectorBytesError(<VB_code>)`.

## 6. Required examples

### `examples/golden_check.*`
- Locate the repo root via env `ARKIVECTOR_REPO` or by walking up to the directory that contains `spec/`.
- **Offline part (always):** for every case in `spec/golden/vectorbytes.json`:
  - valid → build the typed vector from the decoded fields (for halfvec rounding cases from `input_values`), encode, and compare to `hex`. Decode `hex` and compare the fields;
  - invalid → decoding `hex` raises exactly `error`.
- **Online part (only if `ARKIVECTOR_URL` is set):** `version()`. Ensure `demo.docs` exists (`get_collection`; if it is absent, print a hint to run `daemon/tools/dev-up.sh` and skip). Then for each `queries[]` in `spec/fixtures/demo-dataset.json`, run the search through the SDK API (filters rebuilt with `F`, `where` passed as-is, `exact=true`) and compare `expected_ids` exactly and distances within `distance_tolerance`. Run `count` with the same filter and compare `expected_count_matching_filter`.
- Print one line per case (`PASS`/`FAIL name`) and exit non-zero on any failure.

### `examples/quickstart.*`
Connect, print the version and capabilities, list collections, search `demo.docs` with one vector, print the hits. ≤ 60 lines.

## 7. Integration milestones (shared with PLAN_DAEMON.md / PLAN_SDK.md)

| Milestone | Daemon delivers | SDK proves |
| :--- | :--- | :--- |
| **M0** contract frozen | — (spec/ + docs/api exist) | Builds against Prism mock |
| **M1** codec | `daemon/src/codec/vectorbytes.c` passes all golden cases | `golden_check` offline part passes |
| **M2** read API | D3 done: `dev-up.sh` serves demo.docs (version, collections, get, search exact, filters, scroll, count, consistency) | `golden_check` online part passes against the real daemon |
| **M3** writes | D5 done: managed upsert/delete/extend/prepare + idempotency on mock-arkiv | `quickstart --write` round trip (upsert → search with session consistency → delete) |
| **M4** release | D6 done: auth, qdrant, mcp, static binaries | Packages build, framework adapters run the RAG quickstart |
