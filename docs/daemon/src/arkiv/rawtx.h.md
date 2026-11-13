# daemon/src/arkiv/rawtx.h

## Purpose

Declares `av_rawtx` (decoded fields of a signed EIP-1559 transaction: chain id, nonce, fees, gas, `to`, value-is-zero, data, access-list count, signature, recovered sender, hash), `av_rawtx_decode` and `av_rawtx_decode_hex`, and the size cap `AV_RAWTX_MAX_BYTES`.

## Ownership

`data` points into the input buffer; keep the buffer alive while the struct is used.
