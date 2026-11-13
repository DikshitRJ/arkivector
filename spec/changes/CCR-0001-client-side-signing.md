# CCR-0001: Client-side signing (prepare → sign → submit) and signer kinds

- Status: accepted (owner request on GitHub issue #12)
- Class: minor (additive: optional response fields, one endpoint, one capability) — spec 1.0.0 → 1.1.0
- Requested by: orchestrator

## Motivation

Until now every write was signed by the daemon's hot wallet (`ARKIV_PRIVATE_KEY`). `entities:prepare`
returned unsigned calldata, but the client then had to work out fees and gas, sign, find an Arkiv RPC
endpoint to broadcast on, and poll for indexing itself. Apps where **each end user owns their data**
(browser wallets, hardware wallets, KMS) need the daemon to hand out transactions that are ready to
sign, and to take the signed result back. Operators who keep a managed signer want to know which kind
of signer is configured without seeing key material.

## Change (exact)

1. `UnsignedTx` (in `PrepareResponse.transactions`) gains optional fields `type` (const 2), `chain_id`,
   `from`, `gas` (gas limit = `gas_estimate`), `max_fee_per_gas`, `max_priority_fee_per_gas` (0x-hex wei,
   new schema `HexQuantity`). `PrepareResponse` gains optional `from` and `fees` (`FeeSuggestion`:
   `max_fee_per_gas`, `max_priority_fee_per_gas`, `source` ∈ fee_history | gas_price | static).
2. New endpoint `POST /v1/collections/{name}/entities:submit` (`SubmitRequest` → `SubmitResponse`):
   exactly one of `signed_transactions` (raw `0x02…`, 1–256) or `tx_hashes` (1–256), plus `wait`
   (none | receipt | indexed, default receipt) and `timeout_ms` (1–600000, default 60000). Response
   `{tx_hashes, block_number (null for wait=none, status 202), created_keys}`. Raw transactions are
   validated before any broadcast (canonical EIP-1559, low-s, chain id, `to` = Arkiv contract, value 0,
   `execute(...)` calldata, sender is a writer) → `422 invalid_argument` / `403 forbidden`; node
   rejections → `409 tx_reverted`. Accepts `Idempotency-Key`. Scope `write`.
3. `VersionInfo` gains optional `signer`: `null` or `{kind: env | keystore | remote, address}`.
4. `docs/api/entities.md §4`, `operations.md`, `README.md`, `conventions.md`, `consistency.md`,
   `sdk-guidelines.md` updated; `changelog.md` entry 1.1.0.
5. No change to error codes (existing `invalid_argument`, `forbidden`, `tx_reverted`,
   `consistency_timeout` are reused). Golden files regenerated only for the `spec_version` field.

## Capability flag

`entities.submit` (new). The new prepare fields and `signer` need no flag: they are optional and
SDKs read them when present.

## Impact on daemon

`arkiv/rawtx.c` (strict decoder + sender recovery, fuzz target `fuzz_rawtx`), `write/writer.c`
`av_writer_submit`, REST handler, fee suggestion in prepare, version `signer`. Daemon config gains
`[write.signer]` (`kind` = auto | none | env | keystore | remote) — configuration, not contract.

## Impact on SDKs

Python and Node: `submit`, `upsert_signed` / `upsertSigned` (prepare → caller's signer → submit),
converters for eth_account / EIP-1193 / viem / ethers, `VersionInfo.signer`. Older SDKs keep working:
all changes are additive.
