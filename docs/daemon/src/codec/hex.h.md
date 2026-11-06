# daemon/src/codec/hex.h

## Purpose

Strict hexadecimal codec for Arkiv JSON values.

## Public API

| Function | Purpose |
| :--- | :--- |
| `av_status av_hex_encode(const uint8_t *buf, size_t len, av_arena *a, char **out, size_t *outlen, av_err *e)` | Encode to lowercase hex with no prefix. |
| `av_status av_hex_decode(const char *s, size_t n, av_arena *a, uint8_t **out, size_t *outlen, av_err *e)` | Decode hex. An optional `0x` or `0X` prefix is allowed. |

## Behavior

- Decode rejects odd lengths, characters that are not hex, and whitespace. Each error uses code `invalid_argument`.
- The empty string and the bare prefix decode to zero bytes.
- Encode output is NUL-terminated. The NUL is not counted in `outlen`.

## Ownership and thread safety

- Output belongs to the arena. Reentrant.

## Testing

`test/unit/test_vectorbytes.c`, test `hex_roundtrip_and_rejects`.

## Notes

- Callers must cap the input length before decoding. The Arkiv budget is 128 KiB.
