# Vectors: JSON Forms, pgvector Text, and VectorBytes v1

Arkivector uses **pgvector's four types** with pgvector's limits and semantics:

| Type | Element | Max dims | Typical use |
| :--- | :--- | ---: | :--- |
| `vector` | float32 | 16,000 | Full-precision dense embeddings |
| `halfvec` | IEEE binary16 | 16,000 | **Default.** Dense embeddings at half the size |
| `bit` | 1 bit | 64,000 | Binary-quantized embeddings, hashes (Hamming/Jaccard) |
| `sparsevec` | (int32 index, float32 value) pairs | dim ≤ 10⁹, nnz ≤ 16,000 | SPLADE / lexical-neural sparse embeddings |

## 1. Vector inputs in requests

Every request that takes a vector (`EntityInput`, `SearchRequest`) accepts **exactly one** of:

| Field | Form | Example | Notes |
| :--- | :--- | :--- | :--- |
| `vector` | JSON array of numbers | `[0.1, -0.2, 0.3]` | Dense. Parsed as float32, then **cast to the collection column type** (float32 → binary16 with round-half-even for `halfvec`). For `bit` columns, an array of 0/1 integers. |
| `vector` | pgvector text literal | `"[0.1,-0.2,0.3]"`, `"{1:1.5,4:-2}/10"`, `"1011"` | Parsed by pgvector's own `vector_in` / `halfvec_in` / `sparsevec_in` / bit input code, so errors match pgvector. **Sparse text is 1-based** (pgvector convention). |
| `vector` | Sparse JSON object | `{"dim": 10, "indices": [0, 3], "values": [1.5, -2.0]}` | **0-based** indices, strictly ascending, nonzero finite values. |
| `vector_bytes` | Base64 of a VectorBytes blob | `"QVYRAAMAAAAAAIA/AAAAQAAAQEA="` | Exact binary. **Type must equal the column type** (no implicit cast). Fastest, and the SDK default for dense vectors. |

Validation (the daemon MUST apply it, SDKs SHOULD pre-check): finite values only, dimension equals the column dimension (`dimension_mismatch`), and the pgvector limits above (`invalid_vector`).

**Query-time cast rule.** For `search`, a dense JSON or text query against a `bit` column is **not** auto-quantized: send bits. A `vector`-typed `vector_bytes` query against a `halfvec` column is rejected. Send the column type, or use the JSON array form, which is cast.

## 2. Vector outputs in responses

With `include` containing `"vector"`, entities and hits carry `vector_bytes` (base64 VectorBytes of the **stored** column type). SDKs decode it into their native vector type. No JSON-array output form exists, which avoids float formatting ambiguity.

## 3. VectorBytes v1 (binary format)

VectorBytes is the byte format stored in the Arkiv `bytes` attribute `av.vec` **and** used for `vector_bytes` in the API. It is **pgvector's in-memory struct layout**, with the 4-byte varlena length header replaced by an Arkivector header. All multi-byte numbers are **little-endian**.

### 3.1 Common header (bytes 0–3)

| Byte | Field | Value |
| :---: | :--- | :--- |
| 0 | magic | `0x41` (`'A'`) |
| 1 | magic | `0x56` (`'V'`) |
| 2 | version / type | high nibble = format version `1`; low nibble = type: `1` vector, `2` halfvec, `3` bit, `4` sparsevec. So the byte is `0x11`, `0x12`, `0x13`, or `0x14`. |
| 3 | flags | bit 0 = `NORMALIZED` (writer claims ‖v‖₂ = 1). Bits 1–7 MUST be 0. |

### 3.2 Type layouts (bytes 4+ equal pgvector's structs)

```
vector     [0..4) header | [4..6) int16 dim | [6..8) int16 unused = 0 | [8..) float32 x[dim]
           length = 8 + 4·dim

halfvec    [0..4) header | [4..6) int16 dim | [6..8) int16 unused = 0 | [8..) binary16 x[dim]
           length = 8 + 2·dim

bit        [0..4) header | [4..8) int32 bit_len | [8..) uint8 data[ceil(bit_len/8)]
           bit i is (data[i/8] >> (7 - i%8)) & 1   (MSB-first, PostgreSQL VarBit order)
           unused low bits of the last byte MUST be 0
           length = 8 + ceil(bit_len/8)

sparsevec  [0..4) header | [4..8) int32 dim | [8..12) int32 nnz | [12..16) int32 unused = 0
           | int32 indices[nnz] (0-based, strictly ascending, < dim)
           | float32 values[nnz] (finite, nonzero)
           length = 16 + 8·nnz
```

Daemon decode is: validate, `memcpy` into an aligned buffer, overwrite bytes 0–3 with the varlena length. The result is a pgvector `Vector *` / `HalfVector *` / `VarBit *` / `SparseVector *`.

### 3.3 Validation order (decoders MUST check in this order and report the first failure)

| # | Check | Error code |
| :---: | :--- | :--- |
| 1 | length ≥ 8 (≥ 16 for sparsevec once the type is known) | `VB_TOO_SHORT` |
| 2 | bytes 0–1 = `'A' 'V'` | `VB_BAD_MAGIC` |
| 3 | version nibble = 1 | `VB_BAD_VERSION` |
| 4 | type nibble ∈ 1..4 | `VB_UNKNOWN_TYPE` |
| 5 | flag bits 1–7 = 0 | `VB_BAD_FLAGS` |
| 6 | `unused` = 0 (vector, halfvec, sparsevec) | `VB_UNUSED_NONZERO` |
| 7 | dim within limits: vector/halfvec 1..16000, bit 1..64000, sparsevec 1..10⁹ | `VB_DIM_OUT_OF_RANGE` |
| 8 | sparsevec 0 ≤ nnz ≤ min(dim, 16000) | `VB_NNZ_OUT_OF_RANGE` |
| 9 | length = exact formula above (no trailing bytes) | `VB_LENGTH_MISMATCH` |
| 10 | every float finite | `VB_NON_FINITE` |
| 11 | sparse indices strictly ascending | `VB_SPARSE_UNSORTED` |
| 12 | sparse indices < dim (and ≥ 0) | `VB_SPARSE_INDEX_OUT_OF_RANGE` |
| 13 | sparse values ≠ 0 | `VB_SPARSE_ZERO_VALUE` |
| 14 | bit padding bits = 0 | `VB_BIT_PADDING` |

The daemon reports these as `422 invalid_vector` with `vb_error` set. On chain they cause the entity to be quarantined.

### 3.4 Encoder rules

- binary16 conversion from float input uses IEEE **round-to-nearest-even**. Values beyond ±65504 overflow to ±Inf and MUST be rejected (`VB_NON_FINITE`) rather than encoded.
- `NORMALIZED` is optional. Set it only if you actually normalised. The daemon verifies it and ignores a false claim.
- Encoders MUST NOT emit `-0.0` differently from pgvector. Copy the IEEE bits as-is (pgvector does not canonicalise).

### 3.5 Golden vectors

[`spec/golden/vectorbytes.json`](../../spec/golden/vectorbytes.json) holds 10 valid and 19 invalid cases. For each valid case: `hex`, `base64`, `type`, `dim`, decoded `values` / `bits` / `indices`, and for rounding cases the `input_values`. For each invalid case: `error`. Every implementation MUST:
1. encode the decoded form of every valid case to exactly `hex`;
2. decode every valid case to exactly the listed values;
3. reject every invalid case with exactly the listed `error`.

Example (`vector_dim3`, `[1,2,3]`):

```
41 56 11 00 | 03 00 | 00 00 | 00 00 80 3f | 00 00 00 40 | 00 00 40 40
 'A''V' v1t1 fl | dim=3 | unused | 1.0f         | 2.0f        | 3.0f
```

## 4. Distance operators

| Operator | Name | Types | Formula | Opclass suffix |
| :---: | :--- | :--- | :--- | :--- |
| `<->` | L2 | vector, halfvec, sparsevec | √Σ(uᵢ−vᵢ)² | `_l2_ops` |
| `<#>` | negative inner product | vector, halfvec, sparsevec | −Σuᵢvᵢ | `_ip_ops` |
| `<=>` | cosine distance | vector, halfvec, sparsevec | 1 − u·v / (‖u‖‖v‖) | `_cosine_ops` |
| `<+>` | L1 | vector, halfvec, sparsevec | Σ\|uᵢ−vᵢ\| | `_l1_ops` |
| `<~>` | Hamming | bit | popcount(u ⊕ v) | `bit_hamming_ops` |
| `<%>` | Jaccard | bit | 1 − \|u∧v\| / \|u∨v\| | `bit_jaccard_ops` |

Distances are computed by pgvector's kernels. Half-precision values are accumulated in float32, as in pgvector.
