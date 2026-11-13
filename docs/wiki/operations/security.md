# Security

## Exposure

By default the daemon listens on `127.0.0.1:8080` with no authentication: anyone who can reach the port can read every collection and, with a managed signer, spend the wallet's gas. Before listening on another interface or putting it behind a public proxy, **enable API keys**.

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
| `read` | GET endpoints, search, batch, scroll, count, MCP read tools |
| `write` | `read` plus entity upsert, delete, extend, prepare and submit (also through MCP) |
| `admin` | `write` plus creating, following and deleting collections |

Clients send `Authorization: Bearer <secret>`. Keys are compared in constant time and never logged. `/healthz`, `/readyz`, `/v1/version` and `/metrics` stay open.

## Signing keys

Options, from least to most exposure on the daemon host ([Signing writes](../getting-started/signing.md)):

1. **No key at all.** Users sign with their own wallets through [prepare](../api/entities-prepare.md) + [submit](../api/entities-submit.md). Set `[write.signer] kind = "none"` so the daemon can't sign even if a key appears in its environment. Submit checks every signed transaction before broadcasting it: chain id, the Arkiv contract, `execute(...)` calldata and a sender in the collection's `writers`, so the daemon can't be used to relay arbitrary transactions.
2. **Remote signer** (`kind = "remote"`, Web3Signer, Clef, or a KMS behind Web3Signer). The key never enters the daemon. Every signed transaction the signer returns is decoded and compared field by field with the request, and its signature must recover to the configured address, so a compromised or buggy signer can't make the daemon broadcast something else. The daemon doesn't authenticate to the signer: keep it on localhost or a private network, and use the signer's own policies to limit what it signs.
3. **Encrypted keystore** (`kind = "keystore"`). The key is at rest encrypted (scrypt/pbkdf2 + AES-128-CTR); the passphrase comes from a file or, better, a systemd encrypted credential, never from the config or the environment. The decrypted key lives in daemon memory and is wiped on exit.
4. **Environment variable** (`ARKIV_PRIVATE_KEY`). The daemon is a hot wallet and the key is visible to anything that can read the process environment.

For any managed signer: use a **dedicated** wallet holding only what the daemon needs for gas, restrict who can call write endpoints with `write` keys, and remember that `GET /v1/version` shows the signer's kind and address (never key material). Key material is never logged; keystore and remote-signer responses are parsed by code with fuzz targets.

## MCP and browsers

The `/mcp` endpoint rejects requests whose `Origin` header is not localhost unless API keys are on. This blocks DNS-rebinding attacks, where a web page tricks a browser into calling a server on your machine. MCP tool calls are checked against the caller's key with the scope of the underlying REST route.

## Collection trust

A collection only indexes entities from its `writers` (`membership: creator` or `owner`), so other wallets can't inject vectors by reusing your schema tag. `open` collections accept anyone and flag every response with `open_collection: true`.

## Rate limiting

`server.ratelimit_rps` and `ratelimit_burst` enable a per-client token bucket (per API key, or per address without one); excess requests get `429` with `Retry-After`.

## Hardening in the code

Every parser that sees untrusted input (HTTP, JSON-RPC responses, filters, vector literals, VectorBytes, transaction calldata, client-signed and remote-signed transactions) has a libFuzzer target that runs in CI. The daemon is built with `-Wall -Wextra -Wpedantic -Wconversion -Werror`, checked with clang-tidy and cppcheck, and its tests run under AddressSanitizer, UBSan and ThreadSanitizer.
