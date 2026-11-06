# daemon/src/write/batcher.h

## Purpose

Declares the operation batching, calldata size budgeting, and transaction chunking interface for the Arkivector write pipeline (Phase D5).

## Role in Architecture

Operates between `writer.c` and `abi.c`/`tx.c`. It takes an arbitrary number of operations and groups them into discrete transaction units, enforcing EVM calldata byte limits, operation count thresholds, and block gas caps.

## Key Types and Functions

### Data Structures

```c
typedef struct {
    size_t max_tx_bytes;        /* Calldata ceiling in bytes (default 128 KiB) */
    size_t max_ops_per_tx;      /* Max ops per execute() call (default 50) */
    uint64_t max_gas_per_tx;    /* Gas ceiling per transaction (default 15,000,000) */
} av_batcher_config;

typedef struct {
    const av_abi_op *ops;       /* Slice of operations belonging to this batch */
    size_t n_ops;
    uint8_t *calldata;          /* Encoded execute(ops) calldata */
    size_t calldata_len;
    uint64_t gas_estimate;      /* Estimated gas required for this batch */
} av_tx_batch;

typedef struct {
    av_tx_batch *batches;
    size_t n_batches;
} av_batch_plan;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_batcher_plan` | Evaluates a sequence of operations and groups them into one or more `av_tx_batch` items according to configured limits. |
| `av_batcher_estimate_gas` | Calls `eth_estimateGas` via RPC on candidate batch calldata and adds a configurable safety margin. |
| `av_batch_plan_free` | Releases heap memory allocated for an `av_batch_plan`. |

### Function Signatures

```c
av_status av_batcher_plan(const av_abi_op *ops, size_t n_ops,
                          const av_batcher_config *cfg, av_arena *a,
                          av_batch_plan *out, av_err *e);

av_status av_batcher_estimate_gas(av_rpc *rpc, const uint8_t *calldata,
                                  size_t calldata_len, const uint8_t from[20],
                                  uint64_t *out_gas, av_err *e);

void av_batch_plan_free(av_batch_plan *plan);
```

## Memory Ownership

- **Batch Plan Array**: The `batches` array within `av_batch_plan` is allocated on the heap; the caller must free it with `av_batch_plan_free()`.
- **Calldata Buffers**: Calldata payloads associated with each batch are allocated within the caller-supplied `av_arena *a`.
- **Operation References**: Operations inside `av_tx_batch` are borrowed slices from the original `ops` array.

## Invariants

- **Sequential Preservation**: The original relative order of operations is strictly maintained across all generated batches.
- **Strict Boundedness**: No generated transaction batch exceeds `max_tx_bytes` or `max_ops_per_tx`.
- **Non-Splitting of Atomic Units**: An individual operation cannot be split across transactions; if a single operation exceeds `max_tx_bytes`, planning fails immediately.

## Error Handling

- An individual operation whose encoded calldata alone exceeds `max_tx_bytes` returns `AV_ERR` with `code = "invalid_argument"`.
- Gas estimation failures (e.g. EVM execution revert) propagate the upstream error and set `code = "tx_reverted"`.

## Thread Safety

- Thread-safe and reentrant.
- Planning operates on caller-provided data and independent arenas without static state.
