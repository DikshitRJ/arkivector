# Tasks: issue-12-client-signing

- [x] 1. CCR-0001: openapi.yaml, capabilities.json, docs/api, changelog, spec 1.1.0, golden regenerated
- [x] 2. arkiv/rawtx.c decoder + sender recovery; fuzz_rawtx (60 s smoke: 14M runs, no crash); unit suite rawtx
- [x] 3. av_writer_submit + REST handler entities:submit (signed_transactions | tx_hashes, wait, idempotency)
- [x] 4. prepare: type, chain_id, from, gas, fee suggestion
- [x] 5. [write.signer] config; keystore (passphrase file / systemd credential); remote signer with answer verification
- [x] 6. GET /v1/version signer {kind, address}
- [x] 7. Integration suite test_signing.py (17 tests); full integration suite green
- [x] 8. Python SDK: submit, upsert_signed, signing helpers, example (verified with real eth_account)
- [x] 9. Node SDK: submit, upsertSigned (EIP-1193 / viem / ethers / function), converters, browser example (verified with real viem and ethers)
- [x] 10. Docs: wiki Signing + Submit pages, security, configuration, deployment, troubleshooting, SDK pages, README, website, CHANGELOG, example config, docs/daemon
- [ ] 11. Rabdi end to end with MetaMask in a browser (needs a funded testnet wallet and a browser)
