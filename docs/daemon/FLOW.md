# Daemon flows (generated scenarios, D0 to D3)

Each scenario gives the call path. The diagrams follow the source. The HNSW scenario follows the spike notes in `src/pgvector/PROVENANCE.md`, because the spike code is not yet part of the daemon path.

## HNSW insert (spike)

Status: D0 spike. Run only by `test/unit/test_hnsw.c`.

Insert adds one vector to an in-memory pgvector HNSW graph. The graph lives in an arena, so it can be written to a file and mapped again.

```mermaid
sequenceDiagram
    autonumber
    participant T as Inserter thread
    participant S as av_spike_insert (test_hnsw.c)
    participant B as InsertTupleInMemory (hnswbuild_mem.c)
    participant G as HNSW graph (hnswutils.c)
    participant A as Arena (av_arena)
    T->>S: insert vector and docid
    S->>B: call pgvector insert steps
    B->>B: check for duplicate (FindDuplicateInMemory)
    B->>A: allocate element and neighbour arrays
    A-->>B: memory, or NULL at the cap
    B->>G: search layers for candidate neighbours
    B->>G: update neighbours in each layer
    alt memory used + 1 MiB >= memory total
        B-->>S: seal needed
        S-->>T: return seal-needed status
    else room left
        B-->>S: inserted
        S-->>T: return ok
    end
```

Notes:
- An inserter takes pgvector's `entryWaitLock` and then `entryLock` exclusively. A searcher takes the same wait lock, releases it, then takes `entryLock` shared. Without this handshake, searchers starve inserters (D0 finding).
- "Seal needed" is the point where pgvector's memory-full condition is met. In the spike, the sealed graph still gives recall of at least 0.95.

## text literal → pgvector struct

Status: D1. Used by the text codec for every typed input.

```mermaid
sequenceDiagram
    autonumber
    participant C as Caller
    participant T as av_text_to_pgvec (text.c)
    participant N as av_text_to_pgvecn
    participant M as Memory context (AvContextCreate)
    participant P as pgvector *_in (vector_in and others)
    participant R as Arena (out)
    C->>T: type, literal, typmod, arena
    T->>N: literal length (strnlen, max 1 MiB + 1)
    N->>N: check length and embedded NUL
    N->>M: create context (cap 16 MiB)
    N->>M: copy literal (pnstrdup)
    N->>P: DirectFunctionCall3(in, literal, typmod)
    alt pgvector raises ERROR
        P-->>N: longjmp to PG_CATCH
        N->>N: av_err = invalid_vector with pgvector message
    else parsed
        P-->>N: struct in context
        N->>N: check VectorBytes dimension and nnz limits
        N->>R: allocate and copy struct (8-byte aligned)
        R-->>C: pgvector struct
    end
    N->>M: delete context
    N-->>C: AV_OK or AV_ERR
```

Notes:
- The result is copied to the arena before the context is deleted.
- A literal that is too long, or contains a NUL, is rejected before pgvector runs.

## VectorBytes decode

Status: D1. Used for every on-chain vector.

```mermaid
flowchart TD
    A["blob bytes and length"] --> B{"length >= 8?"}
    B -- no --> E1["VB_TOO_SHORT"]
    B -- yes --> C{"magic AV?"}
    C -- no --> E2["VB_BAD_MAGIC"]
    C -- yes --> D{"version 1?"}
    D -- no --> E3["VB_BAD_VERSION"]
    D -- yes --> F{"type 1..4?"}
    F -- no --> E4["VB_UNKNOWN_TYPE"]
    F -- yes --> G{"reserved flags zero?"}
    G -- no --> E5["VB_BAD_FLAGS"]
    G -- yes --> H{"unused fields zero and dim/nnz in range?"}
    H -- no --> E6["VB_UNUSED_NONZERO or VB_DIM_OUT_OF_RANGE or VB_NNZ_OUT_OF_RANGE"]
    H -- yes --> I{"length equals header-implied length?"}
    I -- no --> E7["VB_LENGTH_MISMATCH"]
    I -- yes --> J{"payload checks pass?"}
    J -- no --> E8["NaN, padding, sparse order, index range or zero value"]
    J -- yes --> K["allocate len bytes, 8-byte aligned, in arena"]
    K --> L["copy payload from byte 4, set varlena header"]
    L --> M["pgvector struct returned"]
```

Notes:
- `av_vb_decode` runs `av_vb_validate` first. A blob that fails validation never reaches the copy.
- Each failure sets `code = invalid_vector` and a `VB_*` sub-code.

## search request

Status: D3. Executed for `POST /v1/collections/{name}:search`.

Coordinates vector parsing, filter evaluation, flat vector scan, score derivation, and LMDB entity row hydration.

```mermaid
sequenceDiagram
    autonumber
    participant Client
    participant HTTP as HTTP Server (http.c / router.c)
    participant H as h_search (rest_search.c)
    participant F as Filter Evaluator (filter/eval.c)
    participant E as Query Executor (query/executor.c)
    participant S as Flat Scan (index/flat.c)
    participant LMDB as LMDB Store (store/lmdb_store.c)
    Client->>HTTP: POST /v1/collections/{name}:search (body JSON)
    HTTP->>H: dispatch request to query worker
    H->>H: decode query vector (array, literal, sparse, or VectorBytes)
    opt Cosine opclass
        H->>H: normalize query vector
    end
    opt Filter present (filter object or where string)
        H->>F: av_filter_eval(filter, attr_index, docmap, live, as_of)
        F->>F: evaluate AST nodes, intersect live, prune expired
        F-->>H: candidate Roaring bitmap
    end
    H->>E: av_execute_search(col, store, params, cand)
    E->>S: av_flat_scan(segment, docmap, query, opclass, cand, offset, k)
    S->>S: iterate candidates, compute distances, maintain max-heap
    S-->>E: top raw hits (docid, distance)
    loop Each raw hit
        E->>E: compute score (cosine: 1 - dist; other: -dist)
        opt Threshold checks
            E->>E: filter out if > max_distance or < min_score
        end
        opt Store provided & projections requested
            E->>LMDB: av_store_get_entity(def->name, key)
            LMDB-->>E: packed entity row
            E->>E: unpack attributes (av_attrs_unpack)
            E->>E: format projection (id, doc, vec, attributes)
        end
    end
    E->>E: attach query plan and elapsed ms
    E-->>H: av_search_result
    H->>HTTP: serialize JSON response with hits and plan
    HTTP-->>Client: 200 OK (Arkivector-As-Of-Block, X-Request-Id)
    H->>H: av_search_result_free()
```

Notes:
- Normalization occurs in-place before invoking distance kernels for cosine operator classes.
- Nearest neighbor ties are deterministically broken by ascending 32-byte entity key.
- Result memory (`av_search_result`) is freed with `av_search_result_free()` after JSON serialization.

## scroll

Status: D3. Executed for `POST /v1/collections/{name}:scroll`.

Implements stateless, deterministic cursor pagination over attribute-filtered entities, ordered by ascending entity key.

```mermaid
sequenceDiagram
    autonumber
    participant Client
    participant HTTP as HTTP Server (http.c)
    participant H as h_scroll (rest_search.c)
    participant F as Filter Evaluator (filter/eval.c)
    participant LMDB as LMDB Store (store/lmdb_store.c)
    Client->>HTTP: POST /v1/collections/{name}:scroll (limit, cursor, filter)
    HTTP->>H: dispatch request
    alt Cursor provided
        H->>H: verify HMAC signature on cursor token
        H->>H: decode (v1, as_of_block, last_key, filter_hash)
        opt Consistency check
            H->>H: verify collection as_of_block >= cursor as_of_block
        end
    else No cursor
        H->>H: start from first entity key (0x00...00)
    end
    opt Filter present
        H->>F: av_filter_eval(filter, attr_index, docmap, live, as_of)
        F-->>H: candidate Roaring bitmap
    end
    H->>H: scan matching docids strictly > last_key in ascending key order
    loop Up to limit entities
        H->>LMDB: read entity row and unpack requested projections
        LMDB-->>H: entity data
    end
    alt More entities remain
        H->>H: construct next cursor with new last_key and HMAC signature
    else End of iteration
        H->>H: next_cursor = null
    end
    H->>HTTP: serialize JSON response (entities, next_cursor)
    HTTP-->>Client: 200 OK
```

Notes:
- Scroll cursors are tamper-resistant using per-daemon-instance HMAC-SHA256 secrets.
- Paginates without holding locks or server-side cursor state across requests.

## count

Status: D3. Executed for `POST /v1/collections/{name}:count`.

Calculates the exact number of live, unexpired entities matching an arbitrary filter expression.

```mermaid
sequenceDiagram
    autonumber
    participant Client
    participant HTTP as HTTP Server (http.c)
    participant H as h_count (rest_search.c)
    participant P as Parser (json_filter.c / parser.c)
    participant F as Filter Evaluator (filter/eval.c)
    Client->>HTTP: POST /v1/collections/{name}:count (filter or where)
    HTTP->>H: dispatch request
    alt JSON filter object
        H->>P: av_json_filter_parse(filter_obj, schema)
    else SQL-like where string
        H->>P: av_filter_parse(where_str)
    end
    P-->>H: filter AST node
    H->>F: av_filter_eval(ast, attr_index, docmap, live, as_of_block)
    F->>F: execute postings operations, intersect live, prune expired
    F-->>H: candidate Roaring bitmap
    H->>H: count = roaring_bitmap_get_cardinality(candidate_bm)
    H->>F: roaring_bitmap_free(candidate_bm)
    H->>HTTP: serialize {"count": count}
    HTTP-->>Client: 200 OK
```

Notes:
- Filter evaluation short-circuits on empty subtrees.
- If no filter is specified, the count returns the cardinality of the collection's live bitmap minus expired entities.

## follow collection

Status: D3. Executed for `POST /v1/collections:follow`.

Discovers and synchronizes an existing collection published on the Arkiv chain via its manifest key.

```mermaid
sequenceDiagram
    autonumber
    participant Client
    participant HTTP as HTTP Server (http.c)
    participant H as h_collection_follow (rest_collections.c)
    participant RPC as Arkiv RPC (arkiv/rpc.c)
    participant R as Registry (core/registry.c)
    participant LMDB as LMDB Store (store/lmdb_store.c)
    participant S as Syncer (sync/syncer.c)
    Client->>HTTP: POST /v1/collections:follow (manifest_key or address)
    HTTP->>H: dispatch request
    H->>RPC: av_rpc_get_entity(manifest_key)
    RPC-->>H: manifest entity attributes JSON
    H->>H: parse manifest JSON into av_coldef
    H->>H: validate schema, opclass, dimension, and writer parameters
    H->>R: check name conflict
    H->>LMDB: persist collection definition to collections DB
    H->>R: register collection runtime instance
    H->>S: instantiate and launch syncer worker thread
    H->>HTTP: serialize 201 Created response with collection definition
    HTTP-->>Client: 201 Created (collection metadata, state=init)
```

Notes:
- The manifest entity contains the collection's authoritative configuration (vector column, metric, opclass, index hyperparameters, and membership rules).
- Following immediately triggers background bootstrap and sync catch-up routines.
