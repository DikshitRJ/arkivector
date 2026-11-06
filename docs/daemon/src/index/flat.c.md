# daemon/src/index/flat.c

## Purpose

Implements the exact flat vector scan declared in `flat.h`. It iterates over candidate document IDs, reads raw vector payloads from memory-mapped segment storage, computes similarity scores via `oc->distance()`, maintains a bounded max-heap of size `offset + k`, and performs heap sort to produce ordered results.

## Key Types and Functions

| Function / Type | Role |
| :--- | :--- |
| `flat_scan_ctx` | Context struct holding segment pointer, docmap pointer, query vector, opclass, heap array, limit (`offset + k`), and retained count. |
| `hit_worse` | Comparator implementing deterministic total ordering. Prioritizes non-NaN over NaN, lower distance over higher distance, ascending 32-byte key on distance equality, and ascending docid on key equality. |
| `sift_down` | Re-establishes max-heap property from root index downward. |
| `sift_up` | Re-establishes max-heap property from leaf index upward. |
| `scan_one` | Fetches vector for a given docid via `av_segment_get`, computes distance, and maintains the top `limit` entries in the heap. |
| `scan_cb` | Roaring bitmap iteration callback delegating each candidate docid to `scan_one`. |
| `av_flat_scan` | Main entrypoint. Allocates max-heap, iterates candidates, sorts the top entries ascending, copies the `offset` to `offset + k` window into `out`, and frees the heap. |

## Memory Ownership

- **Heap Memory**: An internal heap buffer of size `(offset + k) * sizeof(av_hit)` is allocated with `malloc` and strictly freed before returning.
- **Segment Data**: Vectors obtained via `av_segment_get` point directly to read-only virtual memory in the segment's mmapped arena file; no copies are created.
- **Output Buffer**: Results are written into the caller-owned `out` array.

## Invariants

- **Deterministic Tie-Breaking**: When two entities yield identical distances, order is strictly resolved by byte-wise lexicographical comparison (`memcmp`) of their 32-byte entity keys.
- **Top-K Boundedness**: The internal heap never retains more than `offset + k` elements, keeping memory consumption $O(offset + k)$ rather than $O(N)$.
- **Recall**: Guaranteed recall of 1.0 against the evaluated candidate set.

## Error Handling

- Out-of-memory during heap allocation sets `code = "internal"` with message `"out of memory for heap"` in `av_err` and returns `AV_ERR`.
- Returns `AV_OK` on success.

## Thread Safety

- Entirely thread-safe and lock-free during execution.
- Reads mapped segment pages and docmap data structures concurrently across multiple query workers.
