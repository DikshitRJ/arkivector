# daemon/src/arkiv/signer.h

## Purpose

Declares the cryptographic transaction signer abstraction and backend factory functions for the Arkivector managed write rail (Phase D5). It supports Web3 encrypted keystore files, environment-variable keys (development mode), and external remote signing services.

## Role in Architecture

Isolates private key management, ECDSA signature generation, and address resolution from transaction orchestration. Used by `writer.c` to sign EIP-1559 sighashes without exposing raw cryptographic keys to the rest of the application.

## Key Types and Functions

### Data Structures and Enums

```c
typedef enum {
    AV_SIGNER_KEYSTORE,  /* Web3 Secret Storage v3 JSON */
    AV_SIGNER_ENV,       /* Hex private key from environment variable */
    AV_SIGNER_REMOTE     /* Remote HTTP JSON-RPC signer (Web3Signer) */
} av_signer_type;

/* Forward declaration of opaque signer handle */
typedef struct av_signer av_signer;

/* Configuration for initializing a signer */
typedef struct {
    av_signer_type type;
    union {
        struct {
            const char *path;       /* Path to keystore JSON */
            const char *password;   /* Decryption passphrase */
        } keystore;
        struct {
            const char *var_name;   /* Environment variable name */
        } env;
        struct {
            const char *url;        /* Remote endpoint URL */
            const char *auth_token; /* Optional Bearer token */
            const char *address;    /* Target account address */
        } remote;
    } cfg;
} av_signer_config;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_signer_create` | Factory function instantiating a signer backend based on `av_signer_config`. |
| `av_signer_get_address` | Retrieves the 20-byte EVM account address associated with the signer. |
| `av_signer_sign` | Signs a 32-byte sighash using SECP256k1, producing `r`, `s`, and `y_parity` with canonical low-S enforcement. |
| `av_signer_destroy` | Sanitizes sensitive memory via `explicit_bzero()` and frees the signer instance. |

### Function Signatures

```c
av_status av_signer_create(const av_signer_config *cfg, av_signer **out_signer, av_err *e);

const uint8_t *av_signer_get_address(const av_signer *s);

av_status av_signer_sign(av_signer *s, const uint8_t sighash[32],
                         uint8_t out_r[32], uint8_t out_s[32],
                         uint8_t *out_y_parity, av_err *e);

void av_signer_destroy(av_signer *s);
```

## Memory Ownership

- **Signer Handle**: Created on the heap via `malloc`; the caller owns the handle and must release it using `av_signer_destroy()`.
- **Address Pointer**: `av_signer_get_address` returns a borrowed pointer to an internal 20-byte buffer valid for the lifetime of the `av_signer`.
- **Zeroing on Free**: Destroying a signer guarantees that private key buffers are zeroed using `explicit_bzero` before deallocation.

## Invariants

- **Low-S Normalization**: Generated signatures always satisfy $s \le \frac{N}{2}$ (where $N$ is the SECP256k1 curve order) to prevent transaction malleability.
- **Y-Parity**: `y_parity` output is strictly `0` or `1`.
- **Immutable Address**: The address returned by `av_signer_get_address` remains constant throughout the signer's lifecycle.

## Error Handling

- Keystore decryption failures (invalid passphrase, corrupted JSON, unsupported KDF, MAC mismatch) return `AV_ERR` and set `code = "unauthorized"`.
- Missing environment variable or invalid hex format in `AV_SIGNER_ENV` sets `code = "invalid_argument"`.
- Remote RPC connection or signing failures set `code = "upstream_error"`.

## Thread Safety

- `av_signer_sign` is thread-safe. Multiple worker threads can invoke signing concurrently on the same `av_signer` handle (protected by internal mutexes where necessary for remote HTTP or hardware channels).
