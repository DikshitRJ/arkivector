/*
 * server/idempotency.c: Idempotency-Key store with in-flight synchronization and 24 h TTL.
 */
#include "server/idempotency.h"

#include <openssl/evp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IDEM_TABLE_BUCKETS 1024
#define IDEM_TTL_SECONDS   86400

typedef enum { IDEM_STATE_IN_PROGRESS, IDEM_STATE_COMPLETED, IDEM_STATE_ABORTED } idem_state;

typedef struct idem_entry {
    char *key;
    uint8_t req_hash[32];
    time_t created_at;
    idem_state state;
    pthread_cond_t cond;
    int waiters;

    /* Cached response */
    int status_code;
    char *content_type;
    char *body;
    size_t body_len;

    struct idem_entry *next;
} idem_entry;

struct av_idempotency_store {
    av_store *store;
    pthread_mutex_t mu;
    idem_entry *buckets[IDEM_TABLE_BUCKETS];
};

static uint32_t hash_key(const char *key)
{
    uint32_t h = 2166136261u;
    for (; *key; key++) {
        h ^= (uint8_t) (*key);
        h *= 16777619u;
    }
    return h;
}

static void free_entry(idem_entry *e)
{
    if (e == NULL)
        return;
    free(e->key);
    free(e->content_type);
    free(e->body);
    pthread_cond_destroy(&e->cond);
    free(e);
}

av_idempotency_store *av_idempotency_store_create(av_store *store)
{
    av_idempotency_store *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->store = store;
    pthread_mutex_init(&s->mu, NULL);
    return s;
}

void av_idempotency_store_destroy(av_idempotency_store *s)
{
    if (s == NULL)
        return;
    pthread_mutex_lock(&s->mu);
    for (size_t i = 0; i < IDEM_TABLE_BUCKETS; i++) {
        idem_entry *curr = s->buckets[i];
        while (curr != NULL) {
            idem_entry *next = curr->next;
            free_entry(curr);
            curr = next;
        }
    }
    pthread_mutex_unlock(&s->mu);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

static void compute_request_hash(const char *method, const char *path, const char *request_body,
                                 size_t request_body_len, uint8_t out_hash[32])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    if (method != NULL)
        EVP_DigestUpdate(ctx, method, strlen(method));
    EVP_DigestUpdate(ctx, ":", 1);
    if (path != NULL)
        EVP_DigestUpdate(ctx, path, strlen(path));
    EVP_DigestUpdate(ctx, ":", 1);
    if (request_body != NULL && request_body_len > 0)
        EVP_DigestUpdate(ctx, request_body, request_body_len);
    unsigned int hash_len = 32;
    EVP_DigestFinal_ex(ctx, out_hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

av_status av_idempotency_check(av_idempotency_store *s, const char *key, const char *method, const char *path,
                               const char *request_body, size_t request_body_len, av_idem_action *out_action,
                               av_idem_cached_response *cached_out, void **out_handle, av_err *e)
{
    if (out_action == NULL || out_handle == NULL)
        return AV_ERR;

    *out_action = AV_IDEM_NEW;
    *out_handle = NULL;
    if (cached_out != NULL)
        memset(cached_out, 0, sizeof(*cached_out));

    if (key == NULL || key[0] == '\0' || s == NULL)
        return AV_OK;

    uint8_t req_hash[32];
    compute_request_hash(method, path, request_body, request_body_len, req_hash);

    uint32_t b_idx = hash_key(key) % IDEM_TABLE_BUCKETS;
    time_t now = time(NULL);

    pthread_mutex_lock(&s->mu);

    idem_entry *prev = NULL;
    idem_entry *entry = s->buckets[b_idx];
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0)
            break;
        prev = entry;
        entry = entry->next;
    }

    /* Check if existing entry expired */
    if (entry != NULL && (now - entry->created_at >= IDEM_TTL_SECONDS)) {
        if (entry->waiters == 0) {
            if (prev != NULL)
                prev->next = entry->next;
            else
                s->buckets[b_idx] = entry->next;
            free_entry(entry);
            entry = NULL;
        }
    }

    if (entry != NULL) {
        /* Compare request hash */
        if (memcmp(entry->req_hash, req_hash, 32) != 0) {
            pthread_mutex_unlock(&s->mu);
            *out_action = AV_IDEM_CONFLICT;
            av_err_set(e, "idempotency_conflict",
                       "Idempotency key '%s' reused with different request payload", key);
            return AV_ERR;
        }

        /* In-flight synchronization */
        while (entry->state == IDEM_STATE_IN_PROGRESS) {
            entry->waiters++;
            pthread_cond_wait(&entry->cond, &s->mu);
            entry->waiters--;
        }

        if (entry->state == IDEM_STATE_COMPLETED) {
            *out_action = AV_IDEM_REPLAY;
            if (cached_out != NULL) {
                cached_out->status_code = entry->status_code;
                cached_out->content_type = entry->content_type ? strdup(entry->content_type) : NULL;
                if (entry->body != NULL && entry->body_len > 0) {
                    cached_out->body = malloc(entry->body_len + 1);
                    if (cached_out->body != NULL) {
                        memcpy(cached_out->body, entry->body, entry->body_len);
                        cached_out->body[entry->body_len] = '\0';
                        cached_out->body_len = entry->body_len;
                    }
                }
            }
            pthread_mutex_unlock(&s->mu);
            return AV_OK;
        }

        /* If previous request aborted, this one becomes the leader */
        entry->state = IDEM_STATE_IN_PROGRESS;
        entry->created_at = now;
        *out_action = AV_IDEM_NEW;
        *out_handle = entry;
        pthread_mutex_unlock(&s->mu);
        return AV_OK;
    }

    /* Create new entry */
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        pthread_mutex_unlock(&s->mu);
        av_err_set(e, "internal", "out of memory allocating idempotency entry");
        return AV_ERR;
    }

    entry->key = strdup(key);
    memcpy(entry->req_hash, req_hash, 32);
    entry->created_at = now;
    entry->state = IDEM_STATE_IN_PROGRESS;
    pthread_cond_init(&entry->cond, NULL);

    entry->next = s->buckets[b_idx];
    s->buckets[b_idx] = entry;

    *out_action = AV_IDEM_NEW;
    *out_handle = entry;
    pthread_mutex_unlock(&s->mu);
    return AV_OK;
}

void av_idempotency_complete(av_idempotency_store *s, void *handle, int status_code, const char *content_type,
                             const char *resp_body, size_t resp_body_len)
{
    if (s == NULL || handle == NULL)
        return;

    idem_entry *entry = (idem_entry *) handle;
    pthread_mutex_lock(&s->mu);
    entry->status_code = status_code;
    if (content_type != NULL) {
        free(entry->content_type);
        entry->content_type = strdup(content_type);
    }
    if (resp_body != NULL && resp_body_len > 0) {
        free(entry->body);
        entry->body = malloc(resp_body_len + 1);
        if (entry->body != NULL) {
            memcpy(entry->body, resp_body, resp_body_len);
            entry->body[resp_body_len] = '\0';
            entry->body_len = resp_body_len;
        }
    }
    entry->state = IDEM_STATE_COMPLETED;
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&s->mu);
}

void av_idempotency_abort(av_idempotency_store *s, void *handle)
{
    if (s == NULL || handle == NULL)
        return;

    idem_entry *entry = (idem_entry *) handle;
    pthread_mutex_lock(&s->mu);
    entry->state = IDEM_STATE_ABORTED;
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&s->mu);
}

void av_idem_cached_response_free(av_idem_cached_response *r)
{
    if (r == NULL)
        return;
    free(r->content_type);
    free(r->body);
    r->content_type = NULL;
    r->body = NULL;
    r->body_len = 0;
}
