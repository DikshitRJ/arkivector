/*
 * server/json_io.h: yyjson helpers for request parsing (strict) and response building.
 *
 * Errors are av_err with code "bad_request" (malformed JSON, wrong JSON types, unknown fields)
 * and `field` set to a JSON pointer (RFC 6901) of the offending member where there is one.
 * Thread safety: stateless; yyjson documents are owned by the caller.
 */
#ifndef AV_SERVER_JSON_IO_H
#define AV_SERVER_JSON_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "server/http.h"
#include "yyjson.h"

/* Parse a request body (strict RFC 8259: no comments, NaN, trailing commas). Empty body is an error.
 * On success the caller frees *doc with yyjson_doc_free. */
av_status av_json_parse(const char *body, size_t len, yyjson_doc **doc, av_err *e);

/* Require the document root to be an object; returns it or NULL with e set. */
yyjson_val *av_json_root_object(yyjson_doc *doc, av_err *e);

/*
 * Strict object check: every key of obj must be in allowed[n]. An unknown key gives bad_request
 * "unknown field" with field = ptr_prefix + "/" + escaped key. ptr_prefix may be NULL or "".
 */
av_status av_json_check_fields(yyjson_val *obj, const char *const *allowed, size_t n, const char *ptr_prefix,
                               av_err *e);

/* Typed member getters. Absent member: required ? bad_request : *out untouched and *present=false.
 * Wrong type: bad_request with field pointer. present may be NULL. */
av_status av_json_get_string(yyjson_val *obj, const char *key, const char *ptr_prefix, bool required,
                             const char **out, bool *present, av_err *e);
av_status av_json_get_bool(yyjson_val *obj, const char *key, const char *ptr_prefix, bool required, bool *out,
                           bool *present, av_err *e);
/* Integer in [min, max]; out of range is invalid_argument with field pointer. */
av_status av_json_get_int(yyjson_val *obj, const char *key, const char *ptr_prefix, bool required,
                          int64_t min, int64_t max, int64_t *out, bool *present, av_err *e);

/* Build "<prefix>/<escaped key>" into out (cap bytes, truncated). */
void av_json_pointer(char *out, size_t cap, const char *prefix, const char *key);

/* Serialize a mutable document into resp (status, application/json body). Frees doc. */
av_status av_resp_json(av_http_resp *resp, int status, yyjson_mut_doc *doc);
/* Set a small constant JSON body (copied). */
av_status av_resp_json_str(av_http_resp *resp, int status, const char *json);

#endif /* AV_SERVER_JSON_IO_H */
