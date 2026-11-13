# Proposal: issue-12-client-signing

## Why
GitHub issue #12: every write was signed by the daemon's hot wallet (`ARKIV_PRIVATE_KEY`). That fits a
single-tenant server, not apps where each end user owns their data, nor operators who don't want a raw
key in the process environment. `entities:prepare` already returned unsigned calldata, but the client
had to work out gas and fees, find an RPC to broadcast on, and poll for indexing itself.

## What changes
- Contract 1.1.0 (CCR-0001, additive): prepare transactions carry `type`, `chain_id`, `from`, `gas`
  and a fee suggestion; new `POST /v1/collections/{name}/entities:submit` (capability
  `entities.submit`) relays client-signed transactions or waits for wallet-broadcast hashes and returns
  the created keys; `GET /v1/version` reports `signer: {kind, address}`.
- Daemon: `arkiv/rawtx.c` (strict signed EIP-1559 decoder + sender recovery, fuzzed),
  `av_writer_submit` (validate all before broadcast: chain, contract, value 0, execute selector, sender
  in writers), `[write.signer]` config (`auto | none | env | keystore | remote`), keystore passphrase from
  a file or systemd credential, remote signer over JSON-RPC (`eth_signTransaction` / Clef
  `account_signTransaction`) whose answers are decoded and checked field by field.
- SDKs: Python `submit`, `upsert_signed(sign= | send=)`, `arkivector.signing`; Node `submit`,
  `upsertSigned({signer})` for EIP-1193 providers, viem, ethers and functions, plus converters.
- Docs: wiki "Signing writes" and "Submit" pages, security, configuration, write path, SDK pages,
  README, website, CHANGELOG, example config, generated daemon docs.

## Contract impact
Minor bump 1.0.0 → 1.1.0, additive only (see spec/changes/CCR-0001-client-side-signing.md).

## Out of scope
Rabdi end-to-end run with a real MetaMask in a browser (tested here against mock-arkiv with real
`eth_account`, viem and ethers signers, and a scripted EIP-1193 wallet). Authentication to the remote
signer (run it on localhost / a private network). Client-side delete/extend helpers in the SDKs
(submit already waits for such transactions).
