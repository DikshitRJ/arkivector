# `daemon/src/server/auth.c`

Implements API key parsing, hashing, and authorization checks.

## Key Features
- **TOML Key Storage**: Loads `[[keys]]` containing `id`, `sha256` (64-character hex hash of the secret token), and `scopes` list (`read`, `write`, `admin`).
- **Timing-Attack Resistance**: Uses OpenSSL's `CRYPTO_memcmp` to perform constant-time comparisons of SHA-256 digests.
- **Scope Enforcement**: Maps REST operations to granular permissions:
  - Admin: collection creation, deletion, follow by manifest.
  - Write: entity upsert, prepare, delete, extend.
  - Read: vector search, scroll, count, entity point lookup.
- **Unauthenticated Paths**: Whitelists health probes (`/healthz`, `/readyz`), Prometheus metrics (`/metrics`), and API version info (`/v1/version`).
