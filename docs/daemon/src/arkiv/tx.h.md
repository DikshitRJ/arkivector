# daemon/src/arkiv/tx.h

## Purpose

Declares the EIP-1559 (Type-2) transaction construction, signing-hash (sighash) generation, and signed envelope serialization interface for the Arkivector write rail (Phase D5).

## Role in Architecture

Operates between high-level operation batching (`batcher.c`) and low-level cryptographic signing (`signer.c`) and broadcasting (`rpc.c`). It encapsulates Ethereum transaction envelope details, invokes `rlp.c` for recursive-length-prefix encoding, and formats raw byte payloads suitable for submission via `eth_sendRawTransaction`.

## Key Types and Functions

### Data Structures

```c
/* EIP-1559 Transaction Parameters */
typedef struct {
    uint64_t chain_id;
    uint64_t nonce;
    uint64_t max_priority_fee_per_gas;
    uint64_t max_fee_per_gas;
    uint64_t gas_limit;
    uint8_t to[20];
    uint64_t value;             /* Always 0 for Arkiv contract mutations */
    const uint8_t *data;        /* Calldata (e.g. execute(ops)) */
    size_t data_len;
} av_tx_1559;

/* ECDSA Signature Components */
typedef struct {
    uint8_t r[32];
    uint8_t s[32];
    uint8_t y_parity;           /* 0 or 1 */
} av_tx_sig;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_tx_sighash` | Generates the 32-byte Keccak-256 transaction signing hash per EIP-1559: `keccak256(0x02 || rlp([...]))`. |
| `av_tx_encode_signed` | Serializes the complete signed Type-2 transaction envelope: `0x02 || rlp([... yParity, r, s])`. |
| `av_tx_compute_fees` | Calculates effective gas fee parameters given current base fee, priority fee tip, and configured caps. |

### Function Signatures

```c
av_status av_tx_sighash(const av_tx_1559 *tx, uint8_t out_hash[32], av_err *e);

av_status av_tx_encode_signed(const av_tx_1559 *tx, const av_tx_sig *sig,
                              av_arena *a, uint8_t **out_raw,
                              size_t *out_len, av_err *e);

void av_tx_compute_fees(uint64_t base_fee, uint64_t tip, uint64_t max_fee_cap,
                        uint64_t *out_max_priority, uint64_t *out_max_fee);
```

## Memory Ownership

- **Signed Payload Buffer**: `av_tx_encode_signed` allocates the resulting raw transaction byte array from the provided `av_arena *a`.
- **Transaction Inputs**: Pointers inside `av_tx_1559` (such as `data`) are borrowed from the caller and must remain valid for the duration of the call.
- **Hash Output**: `av_tx_sighash` writes directly into the caller's 32-byte fixed array `out_hash`.

## Invariants

- **EIP-2718 Type Prefix**: All envelopes and sighash pre-images are prefixed with byte `0x02`.
- **Zero Value**: `tx->value` must be 0; non-zero native currency transfers to the Arkiv contract are disallowed.
- **Recipient Address**: For mutations, `to` must match the canonical Arkiv precompile address `0x4400000000000000000000000000000000000044`.
- **Canonical Signatures**: `sig->y_parity` must strictly be `0` or `1`.

## Error Handling

- Missing calldata (`data == NULL` with `data_len > 0`) or gas limit of zero returns `AV_ERR` with `code = "invalid_argument"`.
- RLP encoding failures propagate `AV_ERR` and populate `av_err *e`.
- Returns `AV_OK` upon successful hash derivation or envelope serialization.

## Thread Safety

- Reentrant and thread-safe. Multiple threads can concurrently generate sighashes and serialize transactions across independent arenas.
