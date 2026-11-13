# daemon/src/arkiv/signer.h

## Purpose

Declares the signer handle used for managed writes and its backends: `AV_SIGNER_ENV`, `AV_SIGNER_KEYSTORE`, `AV_SIGNER_REMOTE` (`av_signer_kind`).

## Key Types and Functions

| Declaration | Role |
| :--- | :--- |
| `av_signer_remote_config` | `url`, `address` (optional), `method` (`eth_signTransaction` or `account_signTransaction`), `chain_id`, `timeout_ms`. |
| `av_signer_create_env`, `av_signer_create_keystore`, `av_signer_create_remote_ex` | Backend constructors. `av_signer_create_remote` is a URL-only shortcut. |
| `av_signer_get_address`, `av_signer_get_address_hex` | 20-byte address and its EIP-55 text. |
| `av_signer_get_kind`, `av_signer_kind_name` | Backend kind and name. |
| `av_signer_sign` | Sign a sighash (local backends only). |
| `av_signer_remote_method`, `av_signer_remote_call` | Remote backend only: the signing method and one JSON-RPC call. |
| `av_signer_destroy` | Wipe and free. |

## Ownership

The caller owns the handle and destroys it after the writer that borrows it.
