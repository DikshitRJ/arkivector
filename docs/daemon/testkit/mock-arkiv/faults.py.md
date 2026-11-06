# daemon/testkit/mock-arkiv/faults.py

## Purpose

Seeded fault injection for the RPC surface. The faults are: HTTP 429, WebSocket drop, latency, and failure of chosen methods.

## Key parts

| Member | Meaning |
| :--- | :--- |
| `http_429_ratio` | Probability of an HTTP 429 response. |
| `drop_ws_ratio` | Probability of closing a WebSocket instead of answering. |
| `latency_ms` | Delay before each RPC. Up to 60000 ms. |
| `fail_methods` | Map of method (or `*`) to failure probability. Answers `-32603`. |

`Faults.update` validates a partial update. `Faults.to_obj` returns the current state.

## Ownership and thread safety

One object per mock. The mock is single-threaded.

## Testing

Used by the daemon's integration tests in later phases. `selftest.py` checks the control API.

## Notes

- The random source is seeded from the mock seed. Each roll consumes random numbers only when the fault is enabled, so a run replays exactly.
