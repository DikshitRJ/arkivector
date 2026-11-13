# Contract Changelog

| Version | Date | CCR | Change |
| :--- | :--- | :--- | :--- |
| 1.0.0 | 2026-11-06 | — | Initial frozen contract: ops, collections, entities (read/write/prepare/delete/extend), search, batch, scroll, count, filters, consistency, errors, VectorBytes v1, golden vectors, demo dataset. |
| 1.1.0 | 2026-11-13 | [CCR-0001](../../spec/changes/CCR-0001-client-side-signing.md) | Client-side signing (GitHub issue #12): `entities:prepare` transactions carry `type`, `chain_id`, `from`, `gas` and a fee suggestion (plus top-level `from`, `fees`); new `POST /v1/collections/{name}/entities:submit` (capability `entities.submit`) broadcasts client-signed transactions or waits for wallet-broadcast hashes and returns the created keys; `GET /v1/version` reports `signer` (`kind`, `address`). Additive. |
