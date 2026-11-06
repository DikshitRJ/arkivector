# daemon/src/codec/vectorbytes.h

## Purpose

Public API of the VectorBytes v1 codec. VectorBytes is the on-chain vector format. It is the in-memory layout of a pgvector struct, with the 4-byte varlena header replaced by `'A' 'V' version|type flags`.

## Public API

| Function | Purpose |
| :--- | :--- |
| `bool av_vb_encoded_size(av_vtype t, int64_t dim, int64_t nnz, size_t *size)` | Exact blob length for a type and dimension. Returns false when the dimension or `nnz` is outside the type limits. |
| `av_status av_vb_validate(const uint8_t *buf, size_t len, av_vb_info *info, av_err *e)` | Check a blob. Fill `info` on success. |
| `av_status av_vb_decode(const uint8_t *buf, size_t len, av_arena *a, void **pgvec, av_vb_info *info, av_err *e)` | Validate, then copy the payload into an 8-byte-aligned pgvector struct in the arena. |
| `av_status av_vb_encode(const void *pgvec, av_vtype t, bool normalized, av_arena *a, uint8_t **out, size_t *len, av_err *e)` | Build a blob from a pgvector struct, then validate it. |

Types: `av_vb_info` (`type`, `flags`, `dim`, `nnz`). The flag `AV_VB_FLAG_NORMALIZED` is 0x01.

## Behavior

- Limits: vector and halfvec 1 to 16000 dimensions. Bit 1 to 64000 bits. Sparsevec dimension 1 to 1,000,000,000, and `nnz` 0 to min(dim, 16000).
- `NORMALIZED` is informational. Consumers always normalise cosine values on ingest.
- A NULL error object is allowed. On failure the functions return `AV_ERR` without touching `*info`.

## Ownership and thread safety

- Output memory belongs to the arena that the caller passes in. Nothing here uses `malloc`.
- All functions are reentrant. The arena must not be shared between threads.

## Testing

`test/unit/test_vectorbytes.c` runs the golden cases from `spec/golden/vectorbytes.json`. It checks decode, encode and base64 output. The fuzz target `test/fuzz/fuzz_vectorbytes.c` checks that every accepted blob re-encodes to the same bytes.

## Notes

- The header is the only public parser entry for untrusted on-chain bytes. Callers must not read a blob without calling `av_vb_validate` first.
