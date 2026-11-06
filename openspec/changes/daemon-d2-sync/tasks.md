# Tasks: daemon-d2-sync
- [ ] Lane A: config, log, threadpool, mpsc, http, router, problem, json_io, metrics, rest_ops, main
- [ ] Lane B: rpc, budget, ws, query_builder, entity, keccak, logs
- [ ] Lane C: lmdb_store, arena_file, docid, live, attr_index, segment (flat), coldef, collection, registry
- [ ] Lane D: apply, bootstrap, discovery A, syncer, reorg, ttl, reconcile, checkpoint
- [ ] Unit tests (keccak, ttl, apply) and fuzzers (http, rpc_response)
- [ ] Integration suites: test_sync.py, test_ttl.py, test_reorg.py
