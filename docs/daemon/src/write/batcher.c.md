# daemon/src/write/batcher.c

## Purpose

Implements the transaction batching algorithms declared in `batcher.h`. It iterates through operation streams, predicts calldata size accumulation, encodes candidate batches via `abi.c`, interacts with `rpc.c` to compute gas estimations, and handles transaction boundary splitting.

## Role in Architecture

Ensures write transactions submitted by `writer.c` conform to EVM network limitations (block gas limits, node calldata byte limits, mempool rejection thresholds) while maximizing throughput by aggregating operations into dense `execute(ops)` calls.

## Key Types and Functions

| Function / Helper | Role |
| :--- | :--- |
| `estimate_op_calldata_size` | Calculates the byte length of an operation's ABI encoding without performing full serialization. |
| `av_batcher_plan` | Main planning loop. Slices the operation array into transaction chunks, invokes `av_abi_encode_execute` for each chunk, and attaches gas estimates. |
| `av_batcher_estimate_gas` | Formats an `eth_estimateGas` JSON-RPC call, adds a 20% buffer margin, and bounds the result to `max_gas_per_tx`. |
| `av_batch_plan_free` | Frees heap buffers associated with an `av_batch_plan`. |

## Memory Ownership

- **Batch Descriptors**: Heap-allocated `batches` array is managed by `av_batch_plan_free`.
- **Arena Usage**: Serialization buffers are generated in the provided `av_arena *a`, allowing rapid bulk reclamation upon transaction completion.

## Invariants

- **Monotonic Splitting**: Slicing progresses strictly forward from index 0 to `n_ops - 1`.
- **Safety Margin**: Gas limits passed to transactions include at least a 20% margin above `eth_estimateGas` to absorb minor state variances between estimation and inclusion.

## Error Handling

- Returns `code = "invalid_argument"` if an individual operation exceeds `max_tx_bytes`.
- Returns `code = "upstream_error"` if the RPC client fails to return a gas estimate.

## Thread Safety

- Purely reentrant and thread-safe. Multiple writer threads plan batches concurrently without mutual contention.
