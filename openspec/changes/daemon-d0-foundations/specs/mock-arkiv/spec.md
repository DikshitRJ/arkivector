## ADDED Requirements

### Requirement: Arkiv JSON-RPC fidelity
The mock SHALL implement `arkiv_query`, `arkiv_getEntity`, `arkiv_getBlockTiming` and the eth_* methods listed in PLAN_DAEMON §4.4 step 6, with the documented error codes, pagination, liveness and ordering semantics.

#### Scenario: Limit bounds
- **WHEN** `arkiv_query` is called with limit 0 or 101
- **THEN** the response is JSON-RPC error -32602

#### Scenario: Expiry
- **WHEN** an entity has `expiresAt = e`
- **THEN** it is visible at block e-1 and absent at block e

### Requirement: Events
Each mutation SHALL emit one log per operation, in operation order, with the topic0 values documented in events-and-logs.md.

#### Scenario: Log range cap
- **WHEN** `eth_getLogs` spans more blocks than `--max-log-range`
- **THEN** the response is error -32005

### Requirement: Determinism and fault injection
Given the same `--seed` and the same control operations, block hashes, keys and logs SHALL be identical. Faults (429s, dropped WebSockets, latency, failing methods) SHALL apply only to RPC, not to the control API.

#### Scenario: Same seed
- **WHEN** two mocks are started with the same seed and receive the same operations
- **THEN** they report identical block hashes and entity keys
