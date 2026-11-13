# Troubleshooting

## My writes never show up in search

The collection only indexes entities created by its `writers`. Check that the daemon wallet's address is in the list:

```bash
curl -s $AV/v1/collections/notes | jq .writers
curl -s $AV/v1/version | jq .writes_enabled
```

Also read with `min_block` set to the write's `block_number`, or write with `"wait": "indexed"` ([Consistency](../api/consistency.md)).

## `503 not_ready`

The collection is still bootstrapping. `/v1/sync` shows progress; large collections take a while the first time. Reads with `consistency.min_block` wait instead of failing.

## `403 write_disabled`

The daemon has no signer. Set `ARKIV_PRIVATE_KEY` and restart.

## `422 invalid_filter` on a `where` string

String literals need the typed form: `topic = str('pets')`, not `topic = 'pets'`. `position` in the error points at the problem.

## `422 dimension_mismatch`

The query or entity vector length differs from the collection's `column.dimensions`. Use the same embedding model (and output size) for writes and queries.

## Searches look approximate

Large HNSW collections return approximate results. Pass `"exact": true` for 100% recall, raise `ef_search`, or check the plan with `"explain": true`.

## Sync is slow or lagging

- Use an Arkiv Hub access key (`ARKIVECTOR_ARKIV_ACCESS_KEY`); the public pool is heavily throttled.
- Check `last_error` in `/v1/sync` and the logs at `debug` level.
- Raise `arkiv.rpc_budget_per_sec` if your access key allows more.

## `consistency_timeout`

The daemon had not applied `min_block` within `timeout_ms`. The transaction may still be pending, or sync is lagging; retry, or raise `timeout_ms` (≤ 60,000).

## Start over

The local index is a cache of the chain. Stop the daemon and start it with `--resync` (or an empty data directory) to rebuild everything from Arkiv.

## Reporting bugs

Open an issue at [github.com/DikshitRJ/arkivector/issues](https://github.com/DikshitRJ/arkivector/issues) with the daemon version (`/v1/version`), the request, and the `request_id` from the error.
