# Search and HNSW

## Pipeline

1. Parse the request and the filter (JSON tree or `where` string) into one AST.
2. If `consistency.min_block` is set, wait until the collection has published that block.
3. Take the collection's read lock (one consistent block for the whole query).
4. Evaluate the filter on the attribute bitmaps and intersect with the live bitmap: the **candidate set**, with its exact size.
5. Plan: exact scan or HNSW.
6. Execute, rescore with the requested operator, sort by `(distance, key)`, apply `offset`, `k`, `max_distance`, `min_score`.
7. Load attributes and documents for the hits and encode the response.

## The planner

| Condition | Plan |
| :--- | :--- |
| Index method `none`, `exact: true`, or an operator different from the index | exact scan |
| 2048 candidates or fewer, or selectivity under 0.5% | exact scan |
| Candidates under the crossover γ·√(m·ef·N) (γ ≈ 1.7) | exact scan |
| Otherwise, no filter | `hnsw` |
| Otherwise, with a filter | `hnsw_iterative` |

Exact selectivity is free here because filters are bitmaps; pgvector itself has to estimate it. Small candidate sets are always faster to scan exactly than to search a graph.

## Exact scan

Every candidate is scored with pgvector's distance kernel, keeping the best `offset + k` in a bounded heap. Kernels are compiled for several CPU levels (x86-64, AVX2/FMA, AVX-512, aarch64) and the best one is picked at start (`kernel_tier` in `/v1/version`).

## HNSW

Collections created with `"index": {"method": "hnsw"}` keep a pgvector HNSW graph next to the flat segment:

- **Expiry bands.** Vectors are grouped by the entity's expiry block into bands of `hnsw.band_blocks` blocks (30 days by default; `0` = one band). Each band has its own graph and exact tail. When a band's last block has passed, every entity in it has expired, so the whole band is released at once, with no per-entity tombstone work. An extended entity is inserted into its new band.
- **Building.** The applier inserts every new vector version into its band's graph with pgvector's `InsertTupleInMemory` (capacity for max(65,536, 2N) vectors). Zero-norm vectors under cosine are not indexable (as in pgvector) and go to the band's exact **tail**, as do vectors that arrive when a graph is full.
- **Persistence.** Each band's graph is saved to `<data_dir>/segments/<collection>/<band>.hnsw` after background builds, at checkpoints and at shutdown (`hnsw.persist`). At start the file is loaded and only vectors added since the save are inserted, so a restart does not rebuild anything. The file is validated completely before use (every pointer, level, neighbour list and value, the build parameters, the pgvector version, a CRC, and the stored vectors against the store); a missing, corrupt or mismatched file is logged and the band is rebuilt in the background, scanned exactly until then.
- **Searching.** Every band that holds candidates is searched and the hits are merged. pgvector's `GetScanItems` walks each graph with `ef_search` (raised to at least `offset + k`); with a filter, pgvector 0.8's iterative scan (`relaxed_order` or `strict_order`) keeps resuming the walk until enough candidates pass the filter or `max_scan_tuples` is reached. Tail vectors that match are scored exactly and merged.
- **Rescoring.** The graph works with index distances (squared L2, negative inner product, cosine on normalized vectors). Every hit is rescored with the requested operator on the raw stored vector, so approximate results report the same distances as exact ones.
- **Fallback.** If the walk returns fewer than `offset + k` hits while more candidates exist, the query is rerun as an exact scan: a filter can never starve the result.
- **Rebuilds.** Updates and deletes leave dead nodes in a graph. After a publish, if a band's tail exceeds max(1024, 10%) of a full graph or more than 30% of its graph is dead, the applier snapshots the band's live vectors and a background thread builds a fresh graph. The sync thread keeps applying blocks meanwhile and searches use the old graph plus the tail. The applier then inserts the vectors that arrived since the snapshot and swaps the new graph in.
- **Concurrency.** The applier inserts under a writer-preferring lock taken per vector; searches take it shared; the background thread builds graphs no one else can see, and copies a graph for a checkpoint under the shared lock. pgvector's own per-element locks protect the graph structure.

Recall at the defaults (m = 16, ef_construction = 64, ef_search = 40) is held to recall@10 ≥ 0.9 by the integration tests; raise `ef_search` per query for more.

## Limits

Each band is searched separately, so a collection whose entities spread over many bands does a little more work per query; widen `hnsw.band_blocks` if that matters more than releasing expired data early. A band without a saved graph (first start after an upgrade, or a rejected file) is scanned exactly until its background build finishes. `GET /_debug/collections/{name}/hnsw` (with `--debug-routes`) shows each band's graph size, tail, source (`loaded`, `built`, `live`, `pending`) and running builds.
