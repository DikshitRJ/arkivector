# daemon/src/codec/base64.c

## Purpose

Implements the base64 codec in `base64.h`.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `b64_val` | Map one alphabet character to its value 0 to 63, or -1. |
| `av_base64_encode` | Encode three bytes per group. Add `=` padding for one or two trailing bytes. |
| `av_base64_decode` | Check the length. Count padding in the last group only. Decode each group. Reject non-zero trailing bits in a padded group. |

## Ownership and thread safety

Output is arena memory. No shared state.

## Testing

`test/unit/test_vectorbytes.c`, test `base64_roundtrip_and_rejects`.
