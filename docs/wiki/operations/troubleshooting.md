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

The daemon has no signer (`/v1/version` shows `"signer": null`): set `ARKIV_PRIVATE_KEY`, or configure a keystore or remote signer, and restart ([Signing writes](../getting-started/signing.md)). Or sign with your own wallet through prepare + submit, which needs no daemon signer. With a remote signer, the message says why it failed: unreachable, refused by the signer's policy, or it returned a transaction that differs from the request.

## The daemon won't start: `keystore password incorrect` / `remote signer`

An explicit `[write.signer]` kind that can't be set up stops startup on purpose. Check the passphrase file or systemd credential, or that the remote signer is reachable and `eth_accounts` returns exactly one address (or set `remote_address`).

## `403 forbidden` from `entities:submit`

The transaction is signed by an address that isn't in the collection's `writers`, so the daemon would never index it. Add the address, or sign with a writer.

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
