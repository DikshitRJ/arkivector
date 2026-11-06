# daemon/src/codec/base64.h

## Purpose

Strict RFC 4648 base64 codec, standard alphabet, with padding. It is used for the `vector_bytes` field of the API.

## Public API

| Function | Purpose |
| :--- | :--- |
| `av_status av_base64_encode(const uint8_t *buf, size_t len, av_arena *a, char **out, size_t *outlen, av_err *e)` | Encode. The output is NUL-terminated. |
| `av_status av_base64_decode(const char *s, size_t n, av_arena *a, uint8_t **out, size_t *outlen, av_err *e)` | Decode. Strict. |

## Behavior

Decode rejects these with code `invalid_argument`:
- A length that is not a multiple of 4.
- Characters outside the standard alphabet. This includes whitespace, newlines, and the URL-safe `-` and `_`.
- Padding anywhere except the last one or two positions.
- Non-zero trailing bits in the last group. This rejects non-canonical encodings.

The empty string decodes to zero bytes.

## Ownership and thread safety

Output is arena memory. Reentrant.

## Testing

`test/unit/test_vectorbytes.c`, test `base64_roundtrip_and_rejects`. The RFC 4648 vectors and all 256 byte values are checked. The golden VectorBytes cases also check the base64 column.
