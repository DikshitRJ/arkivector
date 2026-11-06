# daemon/src/query/executor.c

## Purpose

Implements the query execution engine declared in `executor.h`. It executes exact flat scans over index segments, filters results against score/distance thresholds, retrieves entity metadata from LMDB storage, unpacks and formats typed attributes into yyjson structures, and computes query timing.

## Key Types and Functions

| Function / Type | Role |
| :--- | :--- |
| `current_time_ms` | Reads monotonic clock (`CLOCK_MONOTONIC`) in milliseconds for elapsed query timing. |
| `addr_to_hex` / `key_to_hex` | Encodes binary Ethereum addresses (20-byte) and entity keys (32-byte) into canonical lowercase 0x-prefixed hex strings. |
| `format_typed_val` | Formats an unpacked `av_attr` value into a typed JSON object `{"type": "...", "value": ...}` per OpenAPI spec. Supports boolean, integer, string, base64 bytes, and decimal/u256/address formatting. |
| `av_execute_search` | Coordinates the full query pipeline: executes `av_flat_scan`, applies threshold cuts, looks up entity rows in LMDB via `av_store_get_entity`, unpackages attributes (`av_attrs_unpack`), populates projection fields (`document`, `vector_bytes`, `attributes`), and attaches the execution plan. |
| `av_search_result_free` | Traverses `hits`, frees dynamically allocated strings (`document`, `vector_bytes`), frees the hits array, and frees the `yyjson_mut_doc`. |

## Memory Ownership

- **Result Allocations**:
  - `out->hits` is allocated via `calloc(n_raw, sizeof(av_search_hit))`.
  - Individual strings (`hit->document` and `hit->vector_bytes`) are allocated with `malloc`.
  - `out->doc` owns the `yyjson_mut_doc` holding attribute JSON values.
  - All are freed by `av_search_result_free`.
- **Ephemeral Storage Arena**: Uses an internal ephemeral `av_arena` (`search_row`, 64 KiB default block) to unpack LMDB rows. The arena is reset per row and destroyed before `av_execute_search` returns.
- **Raw Hits**: Temporary `av_hit` array from `av_flat_scan` is allocated via `malloc` and freed before exit.

## Invariants

- **Score Invariant**:
  - Cosine metric: `score = 1.0 - distance`
  - Non-cosine metrics: `score = -distance`
- **Projection Rules**:
  - `include & AV_INCLUDE_ATTRIBUTES`: user attributes placed in `dst->attributes_obj`.
  - `include & AV_INCLUDE_DOCUMENT`: text payload `av.doc` unpacked into `dst->document`.
  - `include & AV_INCLUDE_VECTOR`: VectorBytes `av.vec` converted to base64 string into `dst->vector_bytes`.
- **Sync Snapshot Metadata**: `as_of_block`, `sync_state`, and `open_collection` reflect the snapshot state of the queried collection at scan time.

## Error Handling

- Memory exhaustion on heap or yyjson document creation sets `code = "internal"` and returns `AV_ERR`.
- Errors during LMDB lookups or attribute unpacking are logged/ignored to return partial row data rather than dropping valid search hits.

## Thread Safety

- Completely thread-safe for parallel worker execution.
- Reads LMDB using thread-safe environment handles and accesses collection segments under shared read locks.
