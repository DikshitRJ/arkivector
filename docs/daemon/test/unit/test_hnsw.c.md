# daemon/test/unit/test_hnsw.c

## Purpose

D0 spike (decision Q3). It tests that pgvector's in-memory HNSW graph works as a long-lived, mapped, concurrently searched index.

## Checks

1. Build 10,000 vectors of 128 dimensions in an arena. Recall@10 must be at least 0.95.
2. Copy the arena to a file. Map it read-only at a different address. Searches give identical docids and distances.
3. Four inserters and four searchers run together. No crash, and recall at least 0.95. Run under TSan for the race check.
4. The insert path reports "seal needed" when pgvector's memory-full condition is met.

`av_spike_insert()` is the prototype of `av_hnsw_insert`, the insert call of a later phase. It keeps the steps of pgvector's `InsertTuple` and replaces the disk flush with "seal needed". The concurrency duration is set by `AV_HNSW_SPIKE_SECONDS` (default 3).

## Results

See `src/pgvector/PROVENANCE.md`, "Spike results". The recall, memory and concurrency numbers come from that record.

## Testing

Run with `av_unit -s hnsw`, or `ctest -R unit_hnsw`. Built only when this file is non-empty.

## Notes

- Searchers must use pgvector's `entryWaitLock` handshake. Without it, inserters starve.
