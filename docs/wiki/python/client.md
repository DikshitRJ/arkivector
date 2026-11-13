# Client

```python
from arkivector import Client

client = Client(
    url=None,                  # default: $ARKIVECTOR_URL or http://127.0.0.1:8080
    api_key=None,              # default: $ARKIVECTOR_API_KEY, sent as "Authorization: Bearer ..."
    timeout=10.0,              # seconds per request
    max_retries=3,             # idempotent calls and retryable errors, exponential backoff
    session_consistency=False, # read-your-writes (below)
    idempotency=True,          # send an Idempotency-Key with every write, so retries are safe
)
```

The client is thread-safe and reuses HTTP connections. Close it when done, or use it as a context manager:

```python
with Client() as client:
    print(client.health())    # True
```

## Daemon information

| Method | Returns |
| :--- | :--- |
| `client.health()` | `True` when `/healthz` answers |
| `client.ready()` | `Readiness(ready, collections={name: state})` |
| `client.version()` | `VersionInfo(daemon_version, api_version, pgvector_version, kernel_tier, chain_id, writes_enabled, capabilities, signer)` (cached; `signer` is `SignerInfo(kind, address)` or `None`) |
| `client.has_capability("search.hnsw")` | Whether the daemon supports a feature |
| `client.sync_status()` | `[SyncStatus]` per collection (state, lag, last error) |

The client checks capabilities before optional calls and raises `NotImplementedByDaemon` locally when one is missing.

## Session consistency

With `session_consistency=True` the client remembers the highest `block_number` of its own writes (`client.last_written_block`) and sends it as `min_block` on every read, so a search right after an upsert always sees it. Without it, pass `min_block=` yourself or upsert with `wait="indexed"`. See [Consistency](../api/consistency.md).

## Retries and timeouts

Reads (search, batch, scroll, count, get, prepare) are retried up to `max_retries` times on transport errors and on problems marked `retryable` (honouring `Retry-After`). Writes are retried only because the client sends an `Idempotency-Key`; set `idempotency=False` to disable both.
