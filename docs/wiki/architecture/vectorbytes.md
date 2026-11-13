# VectorBytes

The format of the vector stored on chain in the `av.vec` attribute, also accepted and returned by the API as base64 `vector_bytes`. The idea: **make the on-chain bytes be pgvector's in-memory struct**. A 4-byte Arkivector header replaces PostgreSQL's 4-byte varlena length; decoding is validate, copy, overwrite the header. No parsing, no float formatting.

## On-chain entity layout

| Attribute | Arkiv type | Content |
| :--- | :--- | :--- |
| `arkiv.schema` | indexed `str` | The collection's schema tag, e.g. `arkivector.v1.notes` |
| `av.id` | indexed `str` | Your id |
| `av.vec` | `bytes` | VectorBytes |
| `av.doc` | `bytes` | Optional UTF-8 document |
| your attributes | typed | Up to 29 more |

Any Arkiv client can read these entities; nothing depends on Arkivector staying online.

## Header

| Byte | Field | Value |
| :---: | :--- | :--- |
| 0 | magic | `0x41` (`'A'`) |
| 1 | magic | `0x56` (`'V'`) |
| 2 | version / type | high nibble version `1`; low nibble type `1` vector, `2` halfvec, `3` bit, `4` sparsevec (`0x11`–`0x14`) |
| 3 | flags | bit 0 `NORMALIZED` (the writer claims ‖v‖ = 1); bits 1–7 must be 0 |

## Layouts (little endian)

```
vector     header | int16 dim | int16 unused = 0 | float32 x[dim]              8 + 4·dim bytes
halfvec    header | int16 dim | int16 unused = 0 | binary16 x[dim]             8 + 2·dim bytes
bit        header | int32 bit_len | uint8 data[ceil(bit_len/8)]                 8 + ceil(bit_len/8) bytes
           (MSB first, unused low bits of the last byte are 0)
sparsevec  header | int32 dim | int32 nnz | int32 unused = 0
           | int32 indices[nnz] (0-based, ascending) | float32 values[nnz]      16 + 8·nnz bytes
```

Example, `vector [1, 2, 3]`:

```
41 56 11 00 | 03 00 | 00 00 | 00 00 80 3f | 00 00 00 40 | 00 00 40 40
 'A' 'V' v1t1 flags | dim 3 | unused | 1.0          | 2.0          | 3.0
```

A `halfvec(768)` costs 4 + 4 + 2·768 = 1,544 bytes on chain.

## Validation order

Decoders check in this order and report the first failure (`422 invalid_vector` with `vb_error`; on chain the entity is quarantined):

| # | Check | Code |
| :---: | :--- | :--- |
| 1 | length ≥ 8 (≥ 16 for sparsevec) | `VB_TOO_SHORT` |
| 2 | magic `AV` | `VB_BAD_MAGIC` |
| 3 | version 1 | `VB_BAD_VERSION` |
| 4 | type 1–4 | `VB_UNKNOWN_TYPE` |
| 5 | flag bits 1–7 zero | `VB_BAD_FLAGS` |
| 6 | `unused` zero | `VB_UNUSED_NONZERO` |
| 7 | dimension within limits | `VB_DIM_OUT_OF_RANGE` |
| 8 | sparsevec nnz ≤ min(dim, 16000) | `VB_NNZ_OUT_OF_RANGE` |
| 9 | exact length | `VB_LENGTH_MISMATCH` |
| 10 | all floats finite | `VB_NON_FINITE` |
| 11 | sparse indices ascending | `VB_SPARSE_UNSORTED` |
| 12 | sparse indices < dim | `VB_SPARSE_INDEX_OUT_OF_RANGE` |
| 13 | sparse values non-zero | `VB_SPARSE_ZERO_VALUE` |
| 14 | bit padding zero | `VB_BIT_PADDING` |

## Encoding rules

- float32 → binary16 uses round-to-nearest-even; values beyond ±65,504 are rejected, not saturated.
- `NORMALIZED` is optional; the daemon verifies it and ignores a false claim.
- `-0.0` is kept as-is, like pgvector.

[`spec/golden/vectorbytes.json`](https://github.com/DikshitRJ/arkivector/blob/main/spec/golden/vectorbytes.json) has 10 valid and 19 invalid cases; the daemon and both SDKs reproduce all 29 byte for byte. The decoder is fuzzed with libFuzzer.
