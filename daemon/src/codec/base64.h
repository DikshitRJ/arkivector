/*
 * codec/base64.h: strict RFC 4648 section 4 base64 (standard alphabet, padding).
 *
 * Decode rejects (code "invalid_argument"): length not a multiple of 4, characters
 * outside the alphabet (including whitespace, newlines and the URL-safe '-' '_'),
 * padding anywhere but the last one or two positions, and non-zero trailing bits in
 * the last quantum (non-canonical encodings). The empty string decodes to zero bytes.
 *
 * Ownership: results are arena memory (never NULL on success). encode output is
 * NUL-terminated (the NUL is not counted in *outlen).
 * Thread-safe; no shared state. A NULL e is allowed.
 */
#ifndef AV_CODEC_BASE64_H
#define AV_CODEC_BASE64_H

#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "log.h"

av_status av_base64_encode(const uint8_t *buf, size_t len, av_arena *a, char **out, size_t *outlen,
                           av_err *e);
av_status av_base64_decode(const char *s, size_t n, av_arena *a, uint8_t **out, size_t *outlen, av_err *e);

#endif /* AV_CODEC_BASE64_H */
