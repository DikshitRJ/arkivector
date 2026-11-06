# daemon/src/query/executor.h

## Purpose

Declares the query execution pipeline and search result representation for Arkivector (Phase D3). It unifies candidate filtering, flat vector scanning, distance threshold evaluation, LMDB entity row fetching, projection selection (`include`), and JSON result construction.

## Key Types and Constants

| Symbol | Role |
| :--- | :--- |
| `AV_INCLUDE_ATTRIBUTES` | Bitflag (0x01) requesting entity attributes in search hits. |
| `AV_INCLUDE_DOCUMENT` | Bitflag (0x02) requesting raw text document payload (`av.doc`) in search hits. |
| `AV_INCLUDE_VECTOR` | Bitflag (0x04) requesting base64-encoded VectorBytes (`av.vec`) in search hits. |
| `av_search_params` | Search configuration struct specifying query vector, opclass, pagination (`k`, `offset`), thresholds (`max_distance`, `min_score`), projections, and explain flags. |
| `av_search_hit` | Formatted search hit containing entity key, optional string ID, chain metadata (`owner`, `creator`, timestamps), distance, similarity score, optional document, optional vector bytes, and attribute JSON object. |
| `av_search_result` | Aggregate search outcome containing collection sync status (`as_of_block`, `sync_state`, `open_collection`), array of `hits`, execution `plan`, and underlying `yyjson_mut_doc`. |
| `av_execute_search` | Orchestrates vector scanning, threshold filtering, storage hydration, and plan timing. |
| `av_search_result_free` | Releases all heap memory allocated within an `av_search_result`. |

### Function Signatures

```c
av_status av_execute_search(av_collection *col, av_store *store, const av_search_params *params,
                            const roaring_bitmap_t *cand, av_search_result *out, av_err *e);

void av_search_result_free(av_search_result *res);
```

## Memory Ownership

- **Result Structure**: Caller provides `out` pointer. Upon successful execution, `out` owns its internal heap allocations (`hits` array, individual string fields `document` and `vector_bytes`, and `yyjson_mut_doc *doc`).
- **Cleanup**: The caller MUST call `av_search_result_free(out)` to release all allocated resources.
- **Borrowed Inputs**: `col`, `store`, `params`, and `cand` remain owned by the caller.

## Invariants

- **Score Calculation**: Cosine distance yields `score = 1.0 - distance`. Other distance metrics (L2, inner product) yield `score = -distance`.
- **Threshold Pruning**: Entities exceeding `max_distance` or falling below `min_score` are excluded from the output.
- **System Attributes**: Special attributes are segregated: `av.id` populates `dst->id`, `av.doc` populates `dst->document`, and `av.vec` populates `dst->vector_bytes`. Internal `av.*` prefixes and `arkiv.schema` are omitted from user attributes.

## Error Handling

- Memory allocation failures for raw hits, hits array, or yyjson document set `code = "internal"` and return `AV_ERR`.
- Scan failures from `av_flat_scan` propagate immediately.
- Corrupted or unreadable entity rows in the LMDB store fail gracefully without aborting the overall search.

## Thread Safety

- `av_execute_search` is reentrant and thread-safe.
- Multiple query worker threads execute searches concurrently using read-only LMDB transactions and shared collection locks.
