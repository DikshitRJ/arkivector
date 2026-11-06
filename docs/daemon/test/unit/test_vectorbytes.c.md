# daemon/test/unit/test_vectorbytes.c

## Purpose

Tests the VectorBytes codec, the hex codec, and the base64 codec.

## Suite `vectorbytes`

| Test | Checks |
| :--- | :--- |
| `golden_cases` | Every case in `spec/golden/vectorbytes.json`. Valid cases: decode, the struct matches the JSON values, encode gives the same bytes, and the base64 column matches. Invalid cases: validate and decode return the documented `VB_*` code. The test requires at least 10 valid and 19 invalid cases. |
| `validate_edges` | Null input, sparsevec under 16 bytes, negative dimension, out-of-range sparse index, NaN on encode. |
| `encoded_size_limits` | Size formula and limits per type. |
| `hex_roundtrip_and_rejects` | Hex encode and decode, and rejected inputs. |
| `base64_roundtrip_and_rejects` | RFC 4648 vectors, all 256 byte values, and rejected inputs. |

## Testing

Run with `av_unit -s vectorbytes`, or `ctest -R unit_vectorbytes`. The golden file path comes from `AV_REPO_ROOT`.
