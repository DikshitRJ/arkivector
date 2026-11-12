# Vectors

## Types

| Type | Element | Dimensions | Bytes on chain (d dims) |
| :--- | :--- | :--- | :--- |
| `vector` | float32 | 1–16,000 | 8 + 4·d |
| `halfvec` | float16 (IEEE binary16) | 1–16,000 | 8 + 2·d |
| `sparsevec` | index/value pairs | 1–10⁹, ≤ 16,000 non-zeros | 16 + 8·nnz |
| `bit` | packed bits | 1–64,000 | 8 + d/8 |

`halfvec` is recommended for embeddings: half the bytes and gas of `vector`, with almost no loss in recall. Values are rounded from float32 with round-half-to-even, like pgvector; magnitudes above 65,504 are rejected.

## Sending vectors in JSON

Requests take exactly one of `vector` or `vector_bytes`.

| Form | Example | For |
| :--- | :--- | :--- |
| JSON array | `"vector": [1, 0.5, -0.25]` | `vector`, `halfvec` (read as float32, then cast to the column type); `bit` as an array of 0/1 |
| pgvector literal | `"vector": "[1,0.5,-0.25]"` | Any type, in pgvector's text format |
| sparse object | `"vector": {"dim": 1000, "indices": [3, 70], "values": [0.5, 1.0]}` | `sparsevec` (0-based, ascending indices) |
| sparse literal | `"vector": "{4:0.5,71:1}/1000"` | `sparsevec` (1-based indices, like pgvector) |
| bit string | `"vector": "101100"` | `bit` |
| VectorBytes | `"vector_bytes": "QVYRAAMAAAAAAIA/AAAAQAAAQEA="` | Any type, base64. Its type must equal the column type (no cast) |

NaN and infinity are rejected. A dense query against a `bit` column is not quantized for you: send bits. The dimension must equal the collection's (`422 dimension_mismatch`).

## VectorBytes

The binary format stored on chain in `av.vec`, and accepted as `vector_bytes`: a 4-byte header followed by **exactly pgvector's in-memory struct** for the type, little endian.

| Byte | Content |
| :--- | :--- |
| 0–1 | `'A' 'V'` magic |
| 2 | high nibble: version (1); low nibble: type (1 vector, 2 halfvec, 3 bit, 4 sparsevec) |
| 3 | flags (bit 0: normalized) |
| 4… | pgvector struct (e.g. `int16 dim, int16 unused, float32[dim]` for `vector`) |

See [VectorBytes](../architecture/vectorbytes.md) for every layout and the validation rules. The SDKs encode and decode it for you; [`spec/golden/vectorbytes.json`](https://github.com/DikshitRJ/arkivector/blob/main/spec/golden/vectorbytes.json) holds 29 test vectors.

## Responses

Search, get and scroll return the stored vector as base64 `vector_bytes` when `include` contains `vector`.
