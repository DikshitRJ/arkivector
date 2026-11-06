/*
 * server/auth.c: API keys authentication and scopes.
 */
#include "server/auth.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>

#include "core/fileio.h"
#include "toml.h"

typedef struct av_api_key {
    char *id;
    uint8_t sha256[32];
    uint32_t scopes;
} av_api_key;

struct av_auth {
    bool enabled;
    av_api_key *keys;
    size_t n_keys;
};

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex32(const char *s, uint8_t out[32])
{
    if (s == NULL || strlen(s) != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return true;
}

bool av_auth_is_enabled(const av_auth *a)
{
    return a != NULL && a->enabled;
}

bool av_auth_is_unauthenticated_path(const char *path)
{
    if (path == NULL) {
        return false;
    }
    if (strcmp(path, "/healthz") == 0 || strcmp(path, "/readyz") == 0 || strcmp(path, "/v1/version") == 0 ||
        strcmp(path, "/metrics") == 0) {
        return true;
    }
    return false;
}

uint32_t av_auth_required_scope(const char *method, const char *path)
{
    if (method == NULL || path == NULL) {
        return AV_AUTH_READ;
    }

    bool is_write_method = (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
                            strcmp(method, "DELETE") == 0 || strcmp(method, "PATCH") == 0);
    if (!is_write_method) {
        return AV_AUTH_READ;
    }

    /* Admin actions: collection create, follow, delete */
    if (strcmp(path, "/v1/collections") == 0 && strcmp(method, "POST") == 0) {
        return AV_AUTH_ADMIN;
    }
    if (strcmp(path, "/v1/collections:follow") == 0 && strcmp(method, "POST") == 0) {
        return AV_AUTH_ADMIN;
    }
    if (strncmp(path, "/v1/collections/", 16) == 0 && strcmp(method, "DELETE") == 0) {
        /* If deleting collection */
        if (strstr(path, "/entities") == NULL) {
            return AV_AUTH_ADMIN;
        }
    }
    if (strncmp(path, "/qdrant/collections/", 20) == 0) {
        if (strstr(path, "/points") == NULL) {
            return AV_AUTH_ADMIN;
        }
    }

    /* Write actions: entity creates, updates, deletes, extends */
    if (strstr(path, "/entities") != NULL) {
        return AV_AUTH_WRITE;
    }
    if (strstr(path, "/points") != NULL) {
        return AV_AUTH_WRITE;
    }
    if (strcmp(path, "/mcp") == 0) {
        return AV_AUTH_READ; /* Tool execution checks individual actions */
    }

    return AV_AUTH_READ;
}

av_auth *av_auth_create(const char *api_keys_file, av_err *e)
{
    av_auth *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        av_err_set(e, "internal", "out of memory allocating auth context");
        return NULL;
    }

    if (api_keys_file == NULL || api_keys_file[0] == '\0') {
        a->enabled = false;
        return a;
    }

    size_t nread = 0;
    char *buf = av_read_file(api_keys_file, "api_keys_file", (size_t) 10 * 1024 * 1024, &nread, e);
    if (buf == NULL) {
        free(a);
        return NULL;
    }

    char errbuf[200];
    toml_table_t *root = toml_parse(buf, errbuf, (int) sizeof(errbuf));
    free(buf);
    if (root == NULL) {
        av_err_set(e, "invalid_argument", "failed to parse api_keys_file: %s", errbuf);
        free(a);
        return NULL;
    }

    toml_array_t *keys_arr = toml_array_in(root, "keys");
    if (keys_arr == NULL) {
        toml_free(root);
        a->enabled = true;
        a->keys = NULL;
        a->n_keys = 0;
        return a;
    }

    int n = toml_array_nelem(keys_arr);
    if (n > 0) {
        a->keys = calloc((size_t) n, sizeof(av_api_key));
        if (a->keys == NULL) {
            toml_free(root);
            av_err_set(e, "internal", "out of memory allocating keys");
            free(a);
            return NULL;
        }
        a->n_keys = (size_t) n;

        for (int i = 0; i < n; i++) {
            toml_table_t *ktab = toml_table_at(keys_arr, i);
            if (ktab == NULL)
                continue;

            toml_datum_t d_id = toml_string_in(ktab, "id");
            if (d_id.ok) {
                a->keys[i].id = strdup(d_id.u.s);
                free(d_id.u.s);
            } else {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "key_%d", i);
                a->keys[i].id = strdup(tmp);
            }

            toml_datum_t d_key = toml_string_in(ktab, "key");
            toml_datum_t d_sha = toml_string_in(ktab, "sha256");

            if (d_sha.ok && parse_hex32(d_sha.u.s, a->keys[i].sha256)) {
                /* parsed sha256 */
            } else if (d_key.ok) {
                SHA256((const unsigned char *) d_key.u.s, strlen(d_key.u.s), a->keys[i].sha256);
            }

            if (d_key.ok) {
                explicit_bzero(d_key.u.s, strlen(d_key.u.s));
                free(d_key.u.s);
            }
            if (d_sha.ok) {
                free(d_sha.u.s);
            }

            /* Parse scopes */
            a->keys[i].scopes = AV_AUTH_READ; /* default scope */
            toml_array_t *s_arr = toml_array_in(ktab, "scopes");
            if (s_arr != NULL) {
                int ns = toml_array_nelem(s_arr);
                for (int sidx = 0; sidx < ns; sidx++) {
                    toml_datum_t sd = toml_string_at(s_arr, sidx);
                    if (sd.ok) {
                        if (strcmp(sd.u.s, "read") == 0) {
                            a->keys[i].scopes |= AV_AUTH_READ;
                        } else if (strcmp(sd.u.s, "write") == 0) {
                            a->keys[i].scopes |= AV_AUTH_WRITE;
                        } else if (strcmp(sd.u.s, "admin") == 0) {
                            a->keys[i].scopes |= AV_AUTH_ADMIN;
                        }
                        free(sd.u.s);
                    }
                }
            }
        }
    }

    toml_free(root);
    a->enabled = true;
    return a;
}

void av_auth_destroy(av_auth *a)
{
    if (a == NULL) {
        return;
    }
    for (size_t i = 0; i < a->n_keys; i++) {
        free(a->keys[i].id);
        explicit_bzero(a->keys[i].sha256, 32);
    }
    free(a->keys);
    free(a);
}

av_status av_auth_check(const av_auth *a, const char *auth_header, uint32_t required_scope,
                        const char **out_key_id, av_err *e)
{
    if (!av_auth_is_enabled(a)) {
        return AV_OK;
    }

    if (auth_header == NULL || strncmp(auth_header, "Bearer ", 7) != 0) {
        av_err_set(e, "unauthorized", "missing or malformed Authorization header; expected Bearer token");
        return AV_ERR;
    }

    const char *token = auth_header + 7;
    while (*token == ' ') {
        token++;
    }
    if (*token == '\0') {
        av_err_set(e, "unauthorized", "empty Bearer token");
        return AV_ERR;
    }

    uint8_t token_hash[32];
    SHA256((const unsigned char *) token, strlen(token), token_hash);

    const av_api_key *matched = NULL;
    for (size_t i = 0; i < a->n_keys; i++) {
        if (CRYPTO_memcmp(token_hash, a->keys[i].sha256, 32) == 0) {
            matched = &a->keys[i];
            break;
        }
    }

    explicit_bzero(token_hash, 32);

    if (matched == NULL) {
        av_err_set(e, "unauthorized", "invalid API key");
        return AV_ERR;
    }

    if ((matched->scopes & required_scope) != required_scope) {
        av_err_set(e, "forbidden", "API key has insufficient scope for this operation");
        return AV_ERR;
    }

    if (out_key_id != NULL) {
        *out_key_id = matched->id;
    }
    return AV_OK;
}
