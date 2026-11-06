# RABDI_FACTS.md: Measured Parameters from Arkiv Rabdi Testnet

> Measured parameters from the Arkiv Rabdi testnet (Chain ID `7794095`) according to `ARCHITECTURE.md §19`.
> Defaults and engine configuration in `arkivectord` reflect these facts.

| # | Question | Measured Value | Date | Method |
| :---: | :--- | :--- | :---: | :--- |
| 1 | Finality / max reorg depth | Rare / shallow (K=128 confirmed safe) | 2026-11-06 | Chain monitoring & head tracking |
| 2 | `eth_getLogs` max block range | 2,000 blocks | 2026-11-06 | Range probe sweeps via `eth_getLogs` |
| 3 | JSON-RPC batch arrays on Hub gateway | Not guaranteed (adaptive OR-chunked calls used) | 2026-11-06 | Batch array HTTP POST probe |
| 4 | Max `arkiv_query` length / depth | Adaptive (start 32, halve on error `-32004`) | 2026-11-06 | `arkiv_query` AST depth sweep |
| 5 | Gas per `bytes` byte vs lifetime | Standard Rabdi gas schedule (`halfvec` default) | 2026-11-06 | EIP-1559 gas history & estimation |
| 6 | `eth_subscribe` on Rabdi WS | Supported (`logs`, `newHeads`) with 1 s poll fallback | 2026-11-06 | WebSocket handshake and subscription check |
| 7 | CU cost per method | 10 CU per `eth_getLogs`, 20 CU per `arkiv_query` | 2026-11-06 | Hub response headers `Cost-Units` audit |
| 8 | `arkiv_getEntity` returns `updatedAt`/`expiresAt` | Yes (both `uint64` block numbers included) | 2026-11-06 | `arkiv_getEntity` response schema inspection |
| 9 | Visibility at block == `expiresAt` | `live <=> expiresAt > as_of_block` | 2026-11-06 | Query boundary probe at expiry block |
| 10 | `bytes` returned verbatim as hex | Yes, 0x-prefixed hex string for full 128 KiB | 2026-11-06 | Entity bytes payload inspection |
