# daemon/src/arkiv/abi.h

## Purpose

Declares the EVM ABI encoding and decoding interface for Arkiv's on-chain contract (`0x4400000000000000000000000000000000000044`), supporting Phase D5 write operations. It provides serialization for the single EVM mutation entrypoint `execute((uint8,bytes)[])` (selector `0x49650044`), attribute tuple encodings, deterministic entity key derivation, and calldata decoding for open-collection discovery.

## Role in Architecture

The ABI module defines the binary wire protocol between Arkivector and the Arkiv protocol contract. It translates native daemon entity models (`av_entity`, mutations, TTLs) into standard EVM calldata words and unpacked bytes, calculates deterministic entity keys before transaction broadcast, and enables reverse decoding of raw transaction calldata to correlate on-chain logs with entity mutations in discovery strategies.

## Key Types and Functions

### Enumerations and Data Structures

```c
/* Operation tags corresponding to contract execution subroutines */
typedef enum {
    AV_OP_CREATE = 1,
    AV_OP_PATCH = 2,
    AV_OP_EXTEND_EXPIRY = 3,
    AV_OP_TRANSFER_OWNERSHIP = 4,
    AV_OP_DELETE = 5
} av_op_kind;

/* Attribute serialization unit */
typedef struct {
    char name[33];             /* ASCII name (<= 32 chars) */
    uint8_t type_id;           /* 7-bit type code | (0x80 if indexed) */
    const uint8_t *val;        /* Serialized value payload */
    size_t len;                /* Payload length in bytes */
} av_abi_attr;

/* Unpacked mutation operation */
typedef struct {
    av_op_kind kind;
    union {
        struct {
            uint8_t salt[16];          /* uint128 salt (CSPRNG or zero) */
            uint64_t expires_at;       /* Target block or 0 */
            uint64_t min_lifetime;     /* Duration in blocks from inclusion */
            uint8_t creation_flags;    /* Immutable entity flags */
            const av_abi_attr *attrs;  /* Array of attributes */
            size_t n_attrs;
        } create;
        struct {
            uint8_t entity_key[32];    /* Target entity key */
            const av_abi_attr *attrs;  /* Mutations (type 0 = tombstone) */
            size_t n_attrs;
        } patch;
        struct {
            uint8_t entity_key[32];
            uint64_t expires_at;
            uint64_t min_lifetime;
        } extend;
        struct {
            uint8_t entity_key[32];
            uint8_t new_owner[20];
        } transfer;
        struct {
            uint8_t entity_key[32];
        } del;
    } op;
} av_abi_op;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_abi_encode_execute` | Encodes an array of `av_abi_op` items into canonical `execute((uint8,bytes)[])` calldata prefixed with selector `0x49650044`. |
| `av_abi_decode_execute` | Decodes `execute` calldata into an array of `av_abi_op` items, allocating decoded structures into the provided arena. |
| `av_abi_predict_key` | Computes deterministic entity key `keccak256(chainId || arkiv_addr || owner || nonce || salt)`. |
| `av_abi_encode_entity_nonce_call` | Encodes the `entityNonce(address)` call (`selector 0x36917bfd` + padded owner address) for `eth_call`. |

### Function Signatures

```c
av_status av_abi_encode_execute(const av_abi_op *ops, size_t n_ops,
                                av_arena *a, uint8_t **out_calldata,
                                size_t *out_len, av_err *e);

av_status av_abi_decode_execute(const uint8_t *calldata, size_t len,
                                av_arena *a, av_abi_op **out_ops,
                                size_t *out_n_ops, av_err *e);

av_status av_abi_predict_key(uint64_t chain_id, const uint8_t arkiv_addr[20],
                             const uint8_t owner[20], uint64_t entity_nonce,
                             const uint8_t salt[16], uint8_t out_key[32]);

av_status av_abi_encode_entity_nonce_call(const uint8_t owner[20],
                                          uint8_t out_call[36]);
```

## Memory Ownership

- **Calldata Output (`av_abi_encode_execute`)**: Output buffers are allocated from the supplied `av_arena *a`. Memory lifetime is tied to the arena and requires no per-call `free()`.
- **Decoded Operations (`av_abi_decode_execute`)**: The decoded operation array and all nested attribute names and payload buffers are allocated within the caller-provided arena `a`.
- **Key Output**: `av_abi_predict_key` writes 32 bytes into the caller-allocated buffer `out_key`.

## Invariants

- **Attribute Sorting**: Attribute names within each operation must be sorted in strictly ascending byte order (`memcmp(attr[i].name, attr[i+1].name, 32) < 0`).
- **No Duplicate Attributes**: Every attribute name in an operation must be unique; duplicates trigger an immediate encode error.
- **Name Packing**: Attribute names are stored left-aligned in a 32-byte word with trailing null padding.
- **Deterministic Key Packing**: Key derivation packs arguments tightly into exactly 96 bytes: `chainId` (32 bytes big-endian), `arkiv` (20 bytes), `owner` (20 bytes), `nonce` (8 bytes big-endian), and `salt` (16 bytes).

## Error Handling

- Returns `AV_OK` on success; returns `AV_ERR` and populates `av_err *e` on validation failure.
- Returns `code = "invalid_argument"` if operation tags are out of range (not 1–5), attribute names exceed 32 characters, or attribute order is non-monotonic.
- Allocation failures from the arena set `code = "internal"` with an out-of-memory message.

## Thread Safety

- All functions in `abi.h` are reentrant, stateless, and thread-safe.
- Concurrent threads can safely encode and decode operations provided each thread operates on its own `av_arena`.
