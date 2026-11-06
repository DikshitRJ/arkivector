# daemon/src/index/compact.c

## Purpose

Implements the background compaction, parallel HNSW rebuild, and band management engine declared in `compact.h`. It iterates over live collection documents, filters out dead/tombstoned entries, partitions vectors into temporal expiry bands, constructs new pgvector HNSW arenas using multiple worker threads, performs atomic segment replacement under the publish lock, and drops expired band files.

## Role in Architecture

Maintains steady-state memory sizing, query latency, and index recall in Phase D4. Prevents long-term graph degradation resulting from continuous entity churn, updates, and TTL expirations.

## Key Types and Functions

| Function / Helper | Role |
| :--- | :--- |
| `compact_worker_fn` | Worker thread routine pulling chunks of live vectors from a shared work queue and inserting them into the target rebuild arena via `av_segment_insert`. |
| `partition_by_band` | Sorts and assigns live vectors into discrete segment arenas according to their `expires_at_block` rounded up to multiples of `band_block_width`. |
| `seal_rebuilt_segment` | Writes sealed segment footer (`"AVSEG1"`, opclass, dimensions, entry point relptr, element count), calls `msync`, and re-maps as read-only. |
| `retire_old_segments` | Decrements reference counts on retired segments and triggers `unlink()` on arena files once all active search readers release shared locks. |
| `av_compact_check_needed` | Evaluates whether tombstone ratio (`tombstones / total_vectors`) exceeds threshold or segment count exceeds limit. |
| `av_compact_run` | Drives the full compaction workflow. |
| `av_compact_drop_expired_bands` | Compares segment footer band boundaries against current `as_of_block` and unlinks expired segments. |

## Memory Ownership

- **Arena Files**: Fresh arena files (`seg_<uuid>.arena`) are created on disk and mapped into memory. Old arena files are removed from the filesystem after reader refcounts reach zero.
- **Worker Pools**: Uses the daemon's internal threadpool or spawns ephemeral pthreads joined before swap.

## Invariants

- **Dead Vector Rejection**: Vectors whose document IDs are cleared in the collection's `live` bitmap are strictly excluded from the new graph.
- **Expiry Band Segregation**: Vectors with differing expiration horizons are segregated into distinct sealed segments, ensuring whole-segment dropping at band termination.
- **Safe Reader Draining**: An unlinked segment file remains mapped in memory as long as existing search queries retain a reference.

## Error Handling

- Disk allocation or mmap failure during arena file creation sets `code = "internal"` and rolls back all temporary arena files.
- Thread failure during rebuild safely aborts the compaction task without affecting the active collection state.

## Thread Safety

- Background compactor thread executes concurrently with query workers and sync applier.
- Parallel inserters synchronize on shared arena memory using pgvector's internal `LWLock` hierarchy.
- Final segment array replacement takes the collection's publish lock exclusively for minimal elapsed time.
