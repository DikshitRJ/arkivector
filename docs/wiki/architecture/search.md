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

- **Building.** At start the graph is built from the loaded vectors with capacity for max(65,536, 2N) vectors; the applier then inserts every new vector version with pgvector's `InsertTupleInMemory`. Zero-norm vectors under cosine are not indexable (as in pgvector) and go to an exact **tail**, as do vectors that arrive when the graph is full.
- **Searching.** pgvector's `GetScanItems` walks the graph with `ef_search` (raised to at least `offset + k`); with a filter, pgvector 0.8's iterative scan (`relaxed_order` or `strict_order`) keeps resuming the walk until enough candidates pass the filter or `max_scan_tuples` is reached. Tail vectors that match are scored exactly and merged.
- **Rescoring.** The graph works with index distances (squared L2, negative inner product, cosine on normalized vectors). Every hit is rescored with the requested operator on the raw stored vector, so approximate results report the same distances as exact ones.
- **Fallback.** If the walk returns fewer than `offset + k` hits while more candidates exist, the query is rerun as an exact scan: a filter can never starve the result.
- **Rebuilds.** Updates and deletes leave dead nodes in the graph. After a publish, if the tail exceeds max(1024, 10%) of a full graph or more than 30% of the graph is dead, the applier rebuilds the graph from the live vectors and swaps it in; searches keep using the old graph until the swap.
- **Concurrency.** The applier inserts under a writer-preferring lock taken per vector; searches take it shared. pgvector's own per-element locks protect the graph structure.

Recall at the defaults (m = 16, ef_construction = 64, ef_search = 40) is held to recall@10 ≥ 0.9 by the integration tests; raise `ef_search` per query for more.

## Limits

The graph lives in memory and is rebuilt at every start; very large collections take longer to become ready, and a rebuild runs on the applier thread (sync pauses for its duration). Persisting sealed graphs and background rebuilds are planned.
