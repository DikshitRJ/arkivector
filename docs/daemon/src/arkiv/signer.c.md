# daemon/src/arkiv/signer.c

## Purpose

Implements the three signer backends declared in `signer.h`: `env` (hex key from an environment variable), `keystore` (Web3 Secret Storage v3 JSON file, decrypted with OpenSSL scrypt/PBKDF2 and AES-128-CTR, Keccak-256 MAC check) and `remote` (a Web3Signer / geth / Clef JSON-RPC endpoint). Formats addresses as EIP-55.

## Role in Architecture

Gives `writer.c` (through `tx.c`) one signer handle for managed writes. `main.c` selects the backend from `[write.signer]` (GitHub issue #12). The local backends sign a 32-byte sighash with libsecp256k1. The remote backend does not sign hashes: `tx.c` sends it the whole transaction and checks the answer.

## Key Functions

| Function | Role |
| :--- | :--- |
| `av_signer_create_env` | Reads and parses the hex key, derives the address. |
| `av_signer_create_keystore` | Reads the file (max 1 MiB), checks cipher `aes-128-ctr`, bounds the KDF parameters (scrypt N ≤ 2^20, r, p ≤ 16; PBKDF2 c ≤ 10^7), derives the key, checks the MAC in constant time, decrypts, derives the address. |
| `av_signer_create_remote_ex` | Validates `url` and `method`, creates an `av_rpc` client for the signer (2 retries), uses `remote_address` or asks `eth_accounts` / `account_list` (exactly one account required). |
| `av_signer_remote_call` | One JSON-RPC call to the remote signer. Errors become `write_disabled` with the signer's message. |
| `av_signer_sign` | Recoverable ECDSA over a sighash (local backends only; remote returns an error). |
| `av_signer_get_kind`, `av_signer_kind_name` | `env`, `keystore` or `remote` (reported by `GET /v1/version`). |
| `av_signer_destroy` | Wipes the key with `explicit_bzero`, frees the RPC client. |

## Memory Ownership

The signer owns the 32-byte key (local backends), the remote URL copy and the remote `av_rpc` client. Decrypted and derived key buffers on the stack are wiped before return on every path.

## Invariants

- Key material is never logged and never leaves the process. The remote backend never sees or stores a key.
- An invalid key, wrong passphrase or unreachable signer fails at creation, so `main.c` can stop startup.

## Error Handling

`invalid_argument` for bad configuration, malformed keys, unsupported KDF/cipher and MAC mismatch (`keystore password incorrect (MAC mismatch)`); `internal` for allocation or OpenSSL failures; `write_disabled` for remote-signer call failures.

## Thread Safety

Local signing is reentrant (read-only key and context). Remote calls use the thread-safe `av_rpc` client (one curl handle per call).
