/*
 * sync/discovery.c: Entity discovery strategies for curated and open collections.
 */
#include "sync/discovery.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/abi.h"
#include "arkiv/entity.h"
#include "arkiv/query_builder.h"
#include "core/arena.h"
#include "yyjson.h"

#define NOT_MINE_CAP 4096

typedef struct not_mine_entry {
    char coll_name[64];
    uint8_t key[32];
    uint64_t expires_at;
    struct not_mine_entry *next;
} not_mine_entry;

struct av_discovery {
    av_rpc *rpc;
    pthread_mutex_t mu;
    not_mine_entry *entries[NOT_MINE_CAP];
};

static uint32_t hash_key(const char *coll, const uint8_t key[32])
{
    uint32_t h = 2166136261u;
    while (*coll) {
        h ^= (uint8_t) *coll++;
        h *= 16777619u;
    }
    for (size_t i = 0; i < 32; i++) {
        h ^= key[i];
        h *= 16777619u;
    }
    return h % NOT_MINE_CAP;
}

av_discovery *av_discovery_create(av_rpc *rpc)
{
    av_discovery *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    d->rpc = rpc;
    pthread_mutex_init(&d->mu, NULL);
    return d;
}

void av_discovery_destroy(av_discovery *d)
{
    if (d == NULL) {
        return;
    }
    pthread_mutex_lock(&d->mu);
    for (size_t i = 0; i < NOT_MINE_CAP; i++) {
        not_mine_entry *e = d->entries[i];
        while (e != NULL) {
            not_mine_entry *next = e->next;
            free(e);
            e = next;
        }
    }
    pthread_mutex_unlock(&d->mu);
    pthread_mutex_destroy(&d->mu);
    free(d);
}

void av_discovery_cache_not_mine(av_discovery *d, const char *coll_name, const uint8_t key[32],
                                uint64_t expires_at)
{
    if (d == NULL || coll_name == NULL || key == NULL) {
        return;
    }
    uint32_t idx = hash_key(coll_name, key);

    pthread_mutex_lock(&d->mu);
    not_mine_entry *cur = d->entries[idx];
    while (cur != NULL) {
        if (strcmp(cur->coll_name, coll_name) == 0 && memcmp(cur->key, key, 32) == 0) {
            cur->expires_at = expires_at;
            pthread_mutex_unlock(&d->mu);
            return;
        }
        cur = cur->next;
    }

    not_mine_entry *n = malloc(sizeof(*n));
    if (n != NULL) {
        snprintf(n->coll_name, sizeof(n->coll_name), "%s", coll_name);
        memcpy(n->key, key, 32);
        n->expires_at = expires_at;
        n->next = d->entries[idx];
        d->entries[idx] = n;
    }
    pthread_mutex_unlock(&d->mu);
}

bool av_discovery_is_not_mine(av_discovery *d, const char *coll_name, const uint8_t key[32],
                             uint64_t current_block)
{
    if (d == NULL || coll_name == NULL || key == NULL) {
        return false;
    }
    uint32_t idx = hash_key(coll_name, key);
    bool found = false;

    pthread_mutex_lock(&d->mu);
    not_mine_entry *cur = d->entries[idx];
    while (cur != NULL) {
        if (strcmp(cur->coll_name, coll_name) == 0 && memcmp(cur->key, key, 32) == 0) {
            if (cur->expires_at > current_block) {
                found = true;
            }
            break;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&d->mu);
    return found;
}

static av_status probe_strategy_c(av_discovery *d, const av_coldef *coldef,
                                  const uint8_t (*cand_keys)[32], size_t n_cand,
                                  uint8_t (*out_keys)[32], size_t *n_out, size_t max_out,
                                  uint64_t block_num, av_err *e)
{
    if (n_cand == 0) {
        return AV_OK;
    }

    /* Build query in chunks of 32 */
    size_t chunk_size = 32;
    for (size_t offset = 0; offset < n_cand; offset += chunk_size) {
        size_t count = n_cand - offset;
        if (count > chunk_size) count = chunk_size;

        char lit[128];
        if (av_qb_lit_str(coldef->schema_tag, strlen(coldef->schema_tag), lit, sizeof(lit), e) != AV_OK) {
            return AV_ERR;
        }

        char qbuf[8192];
        av_qb qb;
        av_qb_init(&qb, qbuf, sizeof(qbuf));
        if (av_qb_cmp(&qb, AV_ATTR_SCHEMA, "=", lit, e) != AV_OK ||
            av_qb_and(&qb, e) != AV_OK) {
            return AV_ERR;
        }

        char or_buf[4096];
        if (av_qb_or_keys(or_buf, sizeof(or_buf), (const uint8_t (*)[32]) &cand_keys[offset], count, e) != AV_OK) {
            return AV_ERR;
        }
        if (av_qb_raw(&qb, or_buf, e) != AV_OK) {
            return AV_ERR;
        }

        av_arena *arena = av_arena_create("discovery", 64 * 1024, 0);
        if (arena == NULL) {
            av_err_set(e, "internal", "out of memory creating discovery arena");
            return AV_ERR;
        }

        av_query_page page;
        memset(&page, 0, sizeof(page));
        int rpc_code = 0;

        av_status st = av_rpc_query(d->rpc, qbuf, NULL, 100, true, arena, &page, &rpc_code, e);
        if (st == AV_OK) {
            for (size_t i = 0; i < page.n; i++) {
                if (*n_out < max_out) {
                    memcpy(out_keys[*n_out], page.ents[i].key, 32);
                    (*n_out)++;
                }
            }

            /* Record keys in this batch that were NOT returned as not mine */
            for (size_t i = 0; i < count; i++) {
                const uint8_t *k = cand_keys[offset + i];
                bool found = false;
                for (size_t j = 0; j < page.n; j++) {
                    if (memcmp(page.ents[j].key, k, 32) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    av_discovery_cache_not_mine(d, coldef->name, k, block_num + 100);
                }
            }
        }
        av_arena_destroy(arena);

        if (st != AV_OK) {
            return st;
        }
    }
    return AV_OK;
}

av_status av_discovery_filter_keys(av_discovery *d, const av_coldef *coldef, uint64_t block_num,
                                   const uint8_t (*cand_keys)[32], size_t n_cand,
                                   uint8_t (*out_keys)[32], size_t *n_out, size_t max_out,
                                   av_err *e)
{
    *n_out = 0;
    if (coldef == NULL || n_cand == 0) {
        return AV_OK;
    }

    if (coldef->membership != AV_MEMBERSHIP_OPEN) {
        /* Strategy A: writer-filtered; all candidate keys belong to collection */
        size_t n = n_cand < max_out ? n_cand : max_out;
        for (size_t i = 0; i < n; i++) {
            memcpy(out_keys[i], cand_keys[i], 32);
        }
        *n_out = n;
        return AV_OK;
    }

    if (d == NULL || d->rpc == NULL) {
        return AV_OK;
    }

    /* Filter out keys known not to belong */
    uint8_t unk[512][32];
    size_t n_unk = 0;
    for (size_t i = 0; i < n_cand && n_unk < 512; i++) {
        if (!av_discovery_is_not_mine(d, coldef->name, cand_keys[i], block_num)) {
            memcpy(unk[n_unk++], cand_keys[i], 32);
        }
    }

    if (n_unk == 0) {
        return AV_OK;
    }

    /* Run Strategy C probe (or calldata decode fallback) */
    return probe_strategy_c(d, coldef, (const uint8_t (*)[32]) unk, n_unk, out_keys, n_out, max_out, block_num, e);
}
