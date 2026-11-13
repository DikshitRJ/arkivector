# Tasks: daemon-issues-11-13-14-15

- [x] 1. #13 entityNonce-based keys, submit lock, keys from receipt logs, prepare with chain nonces
- [x] 2. #14 fee chain + cap, gas margin, logged fallback, stuck-tx replacement, nonce resync, metrics, [write] config
- [x] 3. mock-arkiv: eth_feeHistory, /_mock/fees, held mempool with replacement rules
- [x] 4. #15 Qdrant mapping (qdrant_map.c), writer-backed upsert/delete, payloads, filters, 403 without signer
- [x] 5. #11 hnsw_index: expiry bands, worker builds + swap, persistence, validating loader, verify vs flat
- [x] 6. live_entities and unfiltered Qdrant search exclude expired rows
- [x] 7. Fuzz: fuzz_qdrant, fuzz_hnsw_file (found and fixed an overlap case), eth_feeHistory in fuzz_rpc_response
- [x] 8. Unit: fees, hnsw_file (incl. the index's threads under TSan); dev/ASan/TSan 20/20
- [x] 9. Integration 190/190 (test_write_keys_fees, test_qdrant_client, test_hnsw_persist); parity 97/97
- [x] 10. Docs: wiki (write path, search, storage, configuration, monitoring, Qdrant, prepare/upsert),
      README, CHANGELOG, example config, website What's next and site.md
- [ ] 11. Rabdi end to end: keys vs EntityCreated logs, reserved gas within 25% of used (needs a funded wallet)
- [ ] 12. docs/daemon per-file mirror (opencode-docs subagent not available in this session)
