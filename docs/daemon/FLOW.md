# Daemon flows (generated scenarios, D0 and D1)

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
    A[blob bytes and length] --> B{length >= 8?}
    B -- no --> E1[VB_TOO_SHORT]
    B -- yes --> C{magic AV?}
    C -- no --> E2[VB_BAD_MAGIC]
    C -- yes --> D{version 1?}
    D -- no --> E3[VB_BAD_VERSION]
    D -- yes --> F{type 1..4?}
    F -- no --> E4[VB_UNKNOWN_TYPE]
    F -- yes --> G{reserved flags zero?}
    G -- no --> E5[VB_BAD_FLAGS]
    G -- yes --> H{unused fields zero and dim/nnz in range?}
    H -- no --> E6[VB_UNUSED_NONZERO or VB_DIM_OUT_OF_RANGE or VB_NNZ_OUT_OF_RANGE]
    H -- yes --> I{length equals header-implied length?}
    I -- no --> E7[VB_LENGTH_MISMATCH]
    I -- yes --> J{payload checks pass?}
    J -- no --> E8[NaN, padding, sparse order, index range or zero value]
    J -- yes --> K[allocate len bytes, 8-byte aligned, in arena]
    K --> L[copy payload from byte 4, set varlena header]
    L --> M[pgvector struct returned]
```

Notes:
- `av_vb_decode` runs `av_vb_validate` first. A blob that fails validation never reaches the copy.
- Each failure sets `code = invalid_vector` and a `VB_*` sub-code.
