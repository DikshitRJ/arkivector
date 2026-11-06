# daemon/src/arkiv/signer.c

## Purpose

Implements the SECP256k1 signing backends declared in `signer.h`. It parses and decrypts Web3 Secret Storage v3 JSON files using OpenSSL EVP primitives (PBKDF2, scrypt, AES-128-CTR), parses raw keys from environment variables, generates ECDSA signatures with malleability protection, and handles HTTP communication with remote signers.

## Role in Architecture

Serves as the cryptographic heart of Phase D5. Enables `writer.c` to produce authentic EVM transactions signed by the daemon's configured account without exposing raw private keys or credentials across thread boundaries.

## Key Types and Functions

| Function / Helper | Role |
| :--- | :--- |
| `decrypt_keystore_v3` | Parses JSON keystore parameters (`crypto.kdf`, `cipher`, `mac`), derives decryption keys via OpenSSL PBKDF2/scrypt, decrypts ciphertext via AES-128-CTR, and verifies Keccak-256 MAC. |
| `derive_address` | Computes 20-byte Ethereum address from SECP256k1 public key: `keccak256(pubkey[1..64])[12..31]`. |
| `secp256k1_sign_digest` | Generates raw ECDSA signature, enforces low-S normalization ($s = N - s$ if $s > N/2$), and extracts recovery parity bit. |
| `signer_sign_keystore` | Backend signing implementation for keystore and env keys using in-memory SECP256k1 context. |
| `signer_sign_remote` | Backend signing implementation formatting JSON-RPC `eth_sign` requests to external Web3Signer endpoint. |
| `av_signer_create` | Factory function setting up backend function vtables and address caches. |
| `av_signer_destroy` | Wipes private key storage using `explicit_bzero` and frees memory. |

## Memory Ownership

- **Private Key Storage**: The decrypted 32-byte private key is retained in a securely allocated heap struct and zeroed with `explicit_bzero()` upon destruction.
- **Remote Signer Buffers**: URL and token strings are duplicated and owned by the `av_signer` handle.

## Invariants

- **Zeroization**: Private keys are never written to disk, never formatted into logs, and unconditionally scrubbed from RAM upon `av_signer_destroy`.
- **EIP-2 Malleability**: Every signature produced conforms strictly to EIP-2 low-S rules.
- **Non-blocking Keystore Parse**: Keystore decryption occurs once during initialization, avoiding costly KDF evaluations during transaction signing.

## Error Handling

- Keystore MAC verification failure sets `code = "unauthorized"` with message `"invalid keystore passphrase or corrupted MAC"`.
- OpenSSL initialization or allocation failures return `AV_ERR` with `code = "internal"`.
- Remote signer HTTP timeouts or non-200 responses map to `code = "upstream_error"`.

## Thread Safety

- **Local Signers (keystore/env)**: Signature operations are lock-free and reentrant across multiple threads.
- **Remote Signer**: Employs an internal mutex around the HTTP connection handle to serialize concurrent signature RPCs.
