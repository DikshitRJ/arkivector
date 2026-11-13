# Storage

Everything the daemon stores locally can be rebuilt from the chain (`--resync`). It lives in `storage.data_dir`.

## LMDB

One LMDB environment (`storage.map_size`, reserved address space) holds, per collection:

- the collection definition and sync cursor (block number and hash);
- entity rows: key, docid, owner, creator, created/updated/expires blocks, attributes, the raw `av.vec` bytes and the document;
- the id → key index, quarantined entities (invalid vectors), the reorg ring, and the next docid.

LMDB gives crash-safe single-writer transactions, which matches the single applier thread. An applied block batch and its cursor commit in one transaction.

## Docids

Each vector version gets a dense 32-bit **docid**. Keys map to docids and back (`docmap`). Updating an entity's vector allocates a new docid and tombstones the old one, so vectors are never rewritten in place and readers never see a half-updated vector.

## Vector segments

The **flat segment** (`<collection>.flat`) stores every vector version as pgvector's in-memory struct (`Vector`, `HalfVector`, `VarBit`, `SparseVector`) in a memory-mapped, append-only arena file, plus a docid → offset table. Exact scans and the final rescoring of HNSW hits read vectors in place, with no copy and no decoding. It is rebuilt from the LMDB rows at start.

Collections with an HNSW index also keep **HNSW graphs**, one per expiry band: pgvector's own in-memory graph, built by pgvector's insert code ([Search and HNSW](search.md)). They are saved to `segments/<collection>/<band>.hnsw` (a 256-byte header with the build parameters, opclass, pgvector version, docid bound, block watermark and CRC-32, then the graph's arena image, which uses relative pointers and loads at any address). The flat segment stays the source of truth: the graphs only decide which candidates to score, and a graph file can always be deleted (it is rebuilt).

## Live bitmap

A roaring bitmap of the docids that are live at the published block. Writers update a working copy; publishing swaps it in under the collection's write lock. Every query intersects its candidates with it.

## Attribute index

For each attribute value, a roaring bitmap (posting list) of docids, plus sorted structures for range queries. Filters compile to bitmap operations, so a filter's exact selectivity is known before any distance is computed; that is what the planner uses to choose between exact and HNSW search. Attributes that are not indexed on chain are indexed here too.

## Memory

Segments and the graph use memory-mapped arenas with reserved address space that is committed as it is used. A rough guide for 1 million `halfvec(1536)` vectors: about 3 GB of vectors, plus the graph (vectors again, normalized for cosine, plus about 32 neighbour links per vector at m = 16).
