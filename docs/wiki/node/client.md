# Client

```ts
import { Arkivector } from "@dikshitrj/arkivector";

const client = new Arkivector({
  url,                       // default: process.env.ARKIVECTOR_URL ?? "http://127.0.0.1:8080"
  apiKey,                    // default: process.env.ARKIVECTOR_API_KEY (Bearer token)
  timeoutMs: 10_000,         // per request
  maxRetries: 3,             // idempotent calls and retryable errors
  sessionConsistency: false, // read-your-writes (below)
  idempotency: true,         // Idempotency-Key on every write, so retries are safe
  fetch,                     // custom fetch (tests, proxies, edge runtimes)
  userAgent,                 // default: arkivector-node/<version>
});
```

`Client` is an alias of `Arkivector`.

## Daemon information

| Method | Returns |
| :--- | :--- |
| `await client.health()` | `true` when `/healthz` answers |
| `await client.ready()` | `{ ready, collections: { name: state } }` |
| `await client.version()` | `{ daemonVersion, apiVersion, pgvectorVersion, kernelTier, chainId, writesEnabled, capabilities }` (cached) |
| `await client.hasCapability("search.hnsw")` | Whether the daemon supports a feature |
| `await client.syncStatus()` | Per-collection sync state and lag |

Optional features are checked against `capabilities` before the request; a missing one throws `NotImplementedByDaemonError` locally.

## Session consistency

With `sessionConsistency: true` the client remembers the highest block of its own writes (`client.lastWriteBlock`) and sends it as `minBlock` on later reads, so a search right after an upsert sees it. Otherwise pass `consistency: { minBlock }` yourself or upsert with `{ wait: "indexed" }`. See [Consistency](../api/consistency.md).

## Retries

Reads are retried on transport errors and retryable problems (exponential backoff, honouring `Retry-After`). Writes are retried only when an idempotency key is sent, which is the default.
