# daemon/src/index/compact.h

## Purpose

Declares the HNSW segment compaction, index rebuild, and expiry band management interface for Arkivector (Phase D4). It controls background index maintenance, tombstone reclamation, multi-segment consolidation, and TTL band drops for long-running vector collections.

## Role in Architecture

Operates as a background index optimization subsystem alongside `index/segment.c` and `core/collection.c`. Prevents memory bloat and recall degradation caused by tombstoned vectors (from entity deletions and updates) by rebuilding fresh pgvector in-memory HNSW graphs and organizing segments into time-based expiry bands.

## Key Types and Functions

### Data Structures

```c
typedef struct {
    double dead_ratio_threshold;    /* Trigger compaction if dead / total > threshold (default 0.30) */
    size_t min_vectors_to_compact;  /* Minimum vectors required to trigger compaction (default 10,000) */
    size_t max_segments;            /* Maximum sealed segments before consolidation (default 16) */
    uint64_t band_block_width;      /* Block width for expiry bands (default 302,400 = ~7 days) */
    int parallel_workers;           /* Number of worker threads for parallel rebuild */
} av_compact_config;

/* Compaction status summary */
typedef struct {
    size_t segments_before;
    size_t segments_after;
    size_t vectors_scanned;
    size_t tombstones_reclaimed;
    uint64_t elapsed_ms;
} av_compact_stats;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_compact_check_needed` | Evaluates collection metrics (dead vector ratio, segment count, growing segment capacity) to determine if compaction is required. |
| `av_compact_run` | Executes the compaction pipeline: gathers live vectors, partitions by expiry band, rebuilds HNSW graphs in parallel arenas, and performs atomic segment swap. |
| `av_compact_drop_expired_bands` | Identifies and unlinks sealed segment files whose expiry band end height is less than or equal to current `as_of_block`. |

### Function Signatures

```c
bool av_compact_check_needed(const av_collection *c, const av_compact_config *cfg);

av_status av_compact_run(av_collection *c, av_store *store,
                         const av_compact_config *cfg,
                         av_compact_stats *stats, av_err *e);

av_status av_compact_drop_expired_bands(av_collection *c, uint64_t as_of_block,
                                        size_t *out_dropped, av_err *e);
```

## Memory Ownership

- **New Arena Files**: Compaction creates new file-backed virtual memory arenas via `store/arena_file.h`.
- **Old Segments**: Replaced segments are unlinked from disk and their memory unmapped only after active query readers have completely drained (tracked via reader reference counting).
- **Temporary State**: Rebuild inserter threads utilize ephemeral thread-local memory contexts reset after each batch.

## Invariants

- **Zero Query Interruption**: Compaction runs entirely in the background. Read queries continue searching existing segments without lock contention.
- **Microsecond Swap**: New segments are published atomically by acquiring the collection's publish RW lock exclusively for the duration of a pointer swap.
- **Determinism**: Graph construction in rebuild arenas uses a seeded PRNG (`pg_prng`) to ensure reproducibility across rebuilds.
- **Tombstone Elimination**: The rebuilt segments contain only active, live document vectors; all tombstones are purged.

## Error Handling

- Out-of-disk-space during new arena reservation aborts compaction, unlinks temporary files, and leaves existing segments intact without index corruption.
- Memory allocation failures during parallel worker setup set `code = "internal"`.

## Thread Safety

- Background compactor runs on a dedicated worker thread.
- Graph insertion into the rebuild arena uses pgvector's fine-grained element LWLocks across parallel rebuild threads.
- Swapping segments into the live collection is synchronized via the collection's publish `pthread_rwlock_t`.
