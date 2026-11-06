/*
 * codec/hex.h: strict hexadecimal codec.
 *
 * Encode writes lowercase digits without prefix. Decode accepts an optional leading
 * "0x" or "0X", then an even number of hex digits (either case). Anything else
 * (whitespace, odd length, non-hex characters) is rejected with code
 * "invalid_argument". An empty string (or just the prefix) decodes to zero bytes.
 *
 * Ownership: results are arena memory (never NULL on success, even for length 0).
 * encode output is NUL-terminated (the NUL is not counted in *outlen).
 * Thread-safe; no shared state. A NULL e is allowed.
 */
#ifndef AV_CODEC_HEX_H
#define AV_CODEC_HEX_H

#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "log.h"

av_status av_hex_encode(const uint8_t *buf, size_t len, av_arena *a, char **out, size_t *outlen, av_err *e);
av_status av_hex_decode(const char *s, size_t n, av_arena *a, uint8_t **out, size_t *outlen, av_err *e);

/* Callers cap the input length (e.g. the 128 KiB Arkiv bytes budget) before decoding. */
#endif /* AV_CODEC_HEX_H */
