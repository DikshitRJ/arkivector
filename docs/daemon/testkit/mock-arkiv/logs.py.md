# daemon/testkit/mock-arkiv/logs.py

## Purpose

Builds the five Arkiv event logs, the ABI encoding used by `execute` calldata, and the `eth_getLogs` filter matching. It is the base module that `state.py` and `query_lang.py` import.

## Key parts

- `keccak256`, `hx`, `qty`, `checksum_address`, `is_valid_checksum`: primitives. Keccak uses pycryptodome.
- `DEFAULT_CONTRACT` is `0x4400...0044`.
- `TOPIC0` is computed from the event signatures. Import fails if a value differs from the documented topic in `DOC_TOPIC0`.
- `EXECUTE_SELECTOR` (0x49650044) and `ENTITY_NONCE_SELECTOR` (0x36917bfd) are checked at import time.
- `ev_entity_created`, `ev_entity_patched`, `ev_expiry_extended`, `ev_ownership_transferred`, `ev_entity_deleted` return `(topics, data)`.
- `LogFilter.parse` and `LogFilter.matches` implement `eth_getLogs` filter semantics. Up to four topics are allowed.
- `enc_execute`, `enc_create_data`, `enc_patch_data`, `enc_extend_data`, `enc_transfer_data`, `enc_delete_data`: calldata encoders.

## Ownership and thread safety

Pure functions. Reentrant.

## Testing

Import-time assertions cover the topics and selectors. `selftest.py` checks the log output.

## Notes

- Unindexed `bytes` attributes are never indexed. Indexed `bytes` raises a revert.
