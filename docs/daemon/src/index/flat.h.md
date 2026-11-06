# daemon/src/index/flat.h

## Purpose

Declares the exact flat vector scan interface for Arkivector (Phase D3). Performs exact brute-force nearest-neighbor searches over segment vector data using CPU-dispatched pgvector distance kernels, constrained to a candidate Roaring bitmap.

## Key Types and Functions

| Symbol | Role |
| :--- | :--- |
| `av_flat_scan` | Computes distances between `query` and segment vectors filtered by `cand`, maintains a top `(offset + k)` max-heap, and outputs `k` nearest neighbors starting at `offset`. |

### Function Signature

```c
av_status av_flat_scan(const av_segment *s, const av_docmap *docmap, const void *query,
                       const av_opclass *oc, const roaring_bitmap_t *cand,
                       size_t offset, size_t k, av_hit *out, size_t *n_out, av_err *e);
```

## Memory Ownership

- **Output Array**: The caller owns and must allocate `out` with capacity for at least `k` `av_hit` structures.
- **Vectors and Index**: The segment `s` and document map `docmap` remain borrowed and owned by the calling collection.
- **Query Vector**: The `query` vector struct is borrowed from the caller.

## Invariants

- If `k == 0`, immediately returns `AV_OK` with `*n_out = 0`.
- If `cand != NULL`, only docids present in `cand` and physically stored in segment `s` are evaluated.
- If `cand == NULL`, evaluates all entities in `s` from `docid = 0` to `count - 1`.
- Nearest neighbors are sorted ascending by distance; ties are broken deterministically by ascending 32-byte entity key.

## Error Handling

- Memory allocation failure for internal heap structures sets `code = "internal"` with message `"out of memory for heap"` in `e` and returns `AV_ERR`.
- Returns `AV_OK` on successful scan.

## Thread Safety

- Thread-safe and reentrant. Multiple worker threads can concurrently run `av_flat_scan` against the same segment and docmap without mutual interference.
