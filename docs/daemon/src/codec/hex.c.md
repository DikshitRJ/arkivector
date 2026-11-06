# daemon/src/codec/hex.c

## Purpose

Implements the hex codec in `hex.h`.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `hex_val` | Map one character to its value, or -1. |
| `av_hex_encode` | Reject inputs whose doubled length would overflow. Write two digits per byte. |
| `av_hex_decode` | Strip the optional prefix. Reject odd lengths. Decode pairs and report the byte offset of a bad character. |

## Ownership and thread safety

Output is arena memory. No shared state.

## Testing

`test/unit/test_vectorbytes.c`, test `hex_roundtrip_and_rejects`.
