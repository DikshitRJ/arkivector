# daemon/src/arkiv/abi.c

## Purpose

Implements EVM ABI serialization and deserialization for Arkiv mutations as declared in `abi.h`. It constructs canonical `execute((uint8,bytes)[])` calldata, manages word-aligned byte packing for dynamic tuples, sorts attributes, implements Keccak-256 entity key prediction, and decodes incoming transaction calldata.

## Role in Architecture

Operates at the base of the Phase D5 write stack. Used by `writer.c` and `batcher.c` to produce executable EVM transaction payloads, by `rest_entities.c` to generate unsigned calldata for `/entities:prepare`, and by `discovery.c` (Discovery Strategy B) to decode full-block transaction calldata into operations matching log indices.

## Key Types and Functions

| Function / Helper | Role |
| :--- | :--- |
| `write_word` | Emits a 32-byte big-endian word into a growable buffer. |
| `write_bytes_padded` | Emits raw bytes padded with zeroes to a multiple of 32 bytes per EVM ABI specification. |
| `sort_attributes` | Sorts attribute structs lexicographically ascending by their padded 32-byte name representation. |
| `encode_attributes` | Encodes `Attribute[]` dynamic tuple array into EVM ABI format. |
| `encode_op_payload` | Encodes operation-specific payload structs for CREATE, PATCH, EXTEND, TRANSFER, and DELETE. |
| `av_abi_encode_execute` | Encodes function selector `0x49650044` followed by head offsets and operation tuples. |
| `av_abi_decode_execute` | Validates calldata bounds, reads offsets, parses operation tags, and extracts attribute tuples. |
| `av_abi_predict_key` | Packs the 96-byte derivation buffer and invokes `av_keccak256` to derive the deterministic entity key. |
| `av_abi_encode_entity_nonce_call` | Writes selector `0x36917bfd` and 32-byte right-aligned owner address for `eth_call`. |

## Memory Ownership

- **Calldata Buffer**: Dynamically constructed in a growable scratch buffer and copied into the caller-owned arena `a`.
- **Decoded Structs**: All dynamically allocated decoded strings, byte arrays, and operation structures reside in the arena provided to `av_abi_decode_execute`.
- **Arena Reset**: Callers typically discard all encoding/decoding memory in a single step via `av_arena_reset()`.

## Invariants

- **Selector Match**: The first 4 bytes of encoded calldata always equal `0x49 0x65 0x00 0x44`.
- **Word Alignment**: All dynamic tuple heads, tails, and arrays align to 32-byte boundaries (`length % 32 == 0`).
- **Ascending Attribute Names**: Attribute arrays are verified to be strictly sorted (`memcmp` ascending); identical attribute names trigger validation rejection.
- **Round-Trip Fidelity**: For any valid operation array `ops`, `av_abi_decode_execute(av_abi_encode_execute(ops)) == ops`.

## Error Handling

- Calldata shorter than 4 bytes, missing valid selector `0x49650044`, or truncated dynamic offsets set `code = "invalid_argument"` in `av_err`.
- Out-of-bounds offset references during decoding trigger immediate abort to prevent buffer over-reads.
- Duplicate or non-ASCII attribute names yield `code = "invalid_argument"`.

## Thread Safety

- Thread-safe and reentrant.
- All operations write to thread-local or arena-provided memory buffers without global or static state.
