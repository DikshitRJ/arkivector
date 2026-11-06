/*
 * codec/text.h: pgvector text literals <-> pgvector structs, using pgvector's own
 * vector_in/halfvec_in/sparsevec_in and PostgreSQL's bit_in (and the *_out
 * functions), so accepted inputs and error messages match pgvector exactly.
 *
 * Errors: AV_ERR with e->code = "invalid_vector" and e->msg = pgvector's message.
 * Thread safety: reentrant; uses a private memory context per call.
 */
#ifndef AV_CODEC_TEXT_H
#define AV_CODEC_TEXT_H

#include <stdint.h>

#include "codec/vtype.h"
#include "core/arena.h"
#include "log.h"

/*
 * Parse a text literal. typmod_dim > 0 additionally requires that dimension (for bit:
 * that exact bit length), as pgvector's typmod does; pass -1 for no check.
 * On success *out is an 8-byte aligned pgvector struct (Vector*, HalfVector*, VarBit*,
 * SparseVector*) allocated in arena a.
 */
av_status av_text_to_pgvec(av_vtype t, const char *s, int32_t typmod_dim, av_arena *a, void **out, av_err *e);

/*
 * Same with an explicit length (e.g. from JSON): rejects literals longer than
 * AV_TEXT_MAX_LEN and embedded NUL bytes. Parsed values must also satisfy the
 * per-type VectorBytes limits (dims, nnz), which bounds memory and CPU per input.
 */
#define AV_TEXT_MAX_LEN (1024u * 1024u)
#define AV_TEXT_CTX_CAP ((size_t) 16 * 1024 * 1024)
av_status av_text_to_pgvecn(av_vtype t, const char *s, size_t len, int32_t typmod_dim, av_arena *a,
                            void **out, av_err *e);

/* Format a pgvector struct as text (pgvector's *_out). *out is NUL-terminated, in a. */
av_status av_pgvec_to_text(av_vtype t, const void *v, av_arena *a, char **out, av_err *e);

/* Total size in bytes of a pgvector struct (its varlena length). */
size_t av_pgvec_size(const void *v);

#endif /* AV_CODEC_TEXT_H */
