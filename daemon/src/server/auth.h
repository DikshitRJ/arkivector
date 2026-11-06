/*
 * server/auth.h: API keys authentication and scopes.
 *
 * Implements docs/api/conventions.md §3:
 * - Scopes: read (GET, search, scroll, count), write (entity writes, plus read),
 *   admin (collection create/follow/delete, plus write and read).
 * - Constant-time comparison to prevent timing attacks.
 * - Always unauthenticated: /healthz, /readyz, /v1/version, /metrics.
 */
#ifndef AV_SERVER_AUTH_H
#define AV_SERVER_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

#define AV_AUTH_NONE  0u
#define AV_AUTH_READ  (1u << 0)
#define AV_AUTH_WRITE ((1u << 1) | AV_AUTH_READ)
#define AV_AUTH_ADMIN ((1u << 2) | AV_AUTH_WRITE | AV_AUTH_READ)

typedef struct av_auth av_auth;

/*
 * Create auth context from a TOML api_keys_file.
 * If api_keys_file is NULL or empty (""), auth is disabled (av_auth_is_enabled returns false).
 * Returns NULL on allocation or parse error, setting e.
 */
av_auth *av_auth_create(const char *api_keys_file, av_err *e);
void av_auth_destroy(av_auth *a);

bool av_auth_is_enabled(const av_auth *a);

/* True if path is exempt from auth (/healthz, /readyz, /v1/version, /metrics). */
bool av_auth_is_unauthenticated_path(const char *path);

/* Determine the required scope based on HTTP method and path. */
uint32_t av_auth_required_scope(const char *method, const char *path);

/*
 * Validate Authorization header: "Bearer <token>".
 * Returns AV_OK on successful authorization.
 * Returns AV_ERR with e->code = "unauthorized" (401) on missing/invalid key,
 * or e->code = "forbidden" (403) on insufficient scopes.
 * If out_key_id is not NULL and check succeeds, points to the matched key's id.
 */
av_status av_auth_check(const av_auth *a, const char *auth_header, uint32_t required_scope,
                        const char **out_key_id, av_err *e);

#endif /* AV_SERVER_AUTH_H */
