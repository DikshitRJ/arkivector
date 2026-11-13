# Security

## Exposure

By default the daemon listens on `127.0.0.1:8080` with no authentication: anyone who can reach the port can read every collection and, with a signer, spend the wallet's gas. Before listening on another interface or putting it behind a public proxy, **enable API keys**.

## API keys

```toml
# api_keys.toml
[[keys]]
id = "search-frontend"
sha256 = "<hex sha256 of the secret>"   # or key = "<secret>"
scopes = ["read"]

[[keys]]
id = "ingest-job"
sha256 = "…"
scopes = ["read", "write"]

[[keys]]
id = "ops"
sha256 = "…"
scopes = ["read", "write", "admin"]
```

```bash
export ARKIVECTOR_SERVER_API_KEYS_FILE=/etc/arkivector/api_keys.toml
echo -n 'the-secret' | sha256sum          # value for sha256 =
```

| Scope | Allows |
| :--- | :--- |
| `read` | GET endpoints, search, batch, scroll, count, prepare, MCP read tools |
| `write` | `read` plus entity upsert, delete and extend (also through MCP) |
| `admin` | `write` plus creating, following and deleting collections |

Clients send `Authorization: Bearer <secret>`. Keys are compared in constant time and never logged. `/healthz`, `/readyz`, `/v1/version` and `/metrics` stay open.

## The signing key

`ARKIV_PRIVATE_KEY` makes the daemon a hot wallet. Use a **dedicated** wallet holding only what the daemon needs for gas, pass the key through the environment (never the config file, never shell history), and restrict who can call write endpoints with `write` keys. For user-owned data, prefer [`entities:prepare`](../api/entities-prepare.md) and let each user's wallet sign.

## MCP and browsers

The `/mcp` endpoint rejects requests whose `Origin` header is not localhost unless API keys are on. This blocks DNS-rebinding attacks, where a web page tricks a browser into calling a server on your machine. MCP tool calls are checked against the caller's key with the scope of the underlying REST route.

## Collection trust

A collection only indexes entities from its `writers` (`membership: creator` or `owner`), so other wallets can't inject vectors by reusing your schema tag. `open` collections accept anyone and flag every response with `open_collection: true`.

## Rate limiting

`server.ratelimit_rps` and `ratelimit_burst` enable a per-client token bucket (per API key, or per address without one); excess requests get `429` with `Retry-After`.

## Hardening in the code

Every parser that sees untrusted input (HTTP, JSON-RPC responses, filters, vector literals, VectorBytes, transaction calldata) has a libFuzzer target that runs in CI. The daemon is built with `-Wall -Wextra -Wpedantic -Wconversion -Werror`, checked with clang-tidy and cppcheck, and its tests run under AddressSanitizer, UBSan and ThreadSanitizer.
