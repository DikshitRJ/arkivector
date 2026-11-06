/*
 * server/idempotency.h: Idempotency-Key store with in-flight request synchronization and 24 h TTL.
 */
#ifndef AV_SERVER_IDEMPOTENCY_H
#define AV_SERVER_IDEMPOTENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "store/lmdb_store.h"

typedef struct av_idempotency_store av_idempotency_store;

typedef enum {
    AV_IDEM_NEW,       /* Key was not seen; current request is the leader */
    AV_IDEM_REPLAY,    /* Key was previously completed with matching body; replay cached response */
    AV_IDEM_CONFLICT   /* Key was previously seen or in-flight with DIFFERENT body; return 409 */
} av_idem_action;

typedef struct {
    int status_code;
    char *content_type;
    char *body;
    size_t body_len;
} av_idem_cached_response;

av_idempotency_store *av_idempotency_store_create(av_store *store);
void av_idempotency_store_destroy(av_idempotency_store *s);

/*
 * Begin idempotency check.
 * If key is NULL or empty, returns AV_IDEM_NEW and *entry_handle is set to NULL.
 * If AV_IDEM_NEW: caller proceeds with request, then calls av_idempotency_store_complete.
 * If AV_IDEM_REPLAY: caller reads *cached_out and returns immediately.
 * If AV_IDEM_CONFLICT: caller aborts with 409 idempotency_conflict.
 */
av_status av_idempotency_check(av_idempotency_store *s, const char *key,
                               const char *method, const char *path,
                               const char *request_body, size_t request_body_len,
                               av_idem_action *out_action,
                               av_idem_cached_response *cached_out,
                               void **out_handle, av_err *e);

/* Complete the request: store cached response and wake up any waiting callers */
void av_idempotency_complete(av_idempotency_store *s, void *handle,
                             int status_code, const char *content_type,
                             const char *resp_body, size_t resp_body_len);

/* Abort the in-flight request on internal error */
void av_idempotency_abort(av_idempotency_store *s, void *handle);

void av_idem_cached_response_free(av_idem_cached_response *r);

#endif /* AV_SERVER_IDEMPOTENCY_H */
