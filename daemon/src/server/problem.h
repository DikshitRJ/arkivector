/*
 * server/problem.h: av_err to RFC 9457 problem+json.
 *
 * Body: {"type":"urn:arkivector:error:<code>","title":...,"status":N,"detail":...,"code":...,
 * "retryable":bool,"request_id":...,"instance":...} plus the extension fields that are set on
 * the av_err: field, position, vb_error, as_of_block. Status, title and retryable come from a
 * table generated at build time from spec/error-codes.json (build/generated/av_error_table.h).
 * An unknown or empty code is reported as "internal" (500).
 * 429 and 503 responses get Retry-After: 1 unless the handler already set it.
 */
#ifndef AV_SERVER_PROBLEM_H
#define AV_SERVER_PROBLEM_H

#include <stdbool.h>

#include "server/http.h"

/*
 * Fill resp (status, content_type, malloc'd body). Uses resp->request_id (pre-filled by the
 * server) for the request_id field. instance may be NULL (the request path is typical).
 */
void av_problem_from_err(const av_err *e, av_http_resp *resp, const char *instance);

/* HTTP status for a code in spec/error-codes.json, 500 when unknown. */
int av_problem_status(const char *code);
/* True when the code exists in the generated table. */
bool av_problem_known(const char *code);
/* Number of codes in the table (for tests that cross-check spec/error-codes.json). */
size_t av_problem_table_size(void);
const char *av_problem_table_code(size_t i);

#endif /* AV_SERVER_PROBLEM_H */
