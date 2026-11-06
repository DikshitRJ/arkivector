/*
 * sync/syncer.c: chain synchronisation engine. See syncer.h for the state machine.
 */
#include "sync/syncer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <yyjson.h>

#include "arkiv/entity.h"
#include "arkiv/logs.h"
#include "arkiv/query_builder.h"
#include "arkiv/rpc.h"
#include "arkiv/ws.h"
#include "core/arena.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "server/rest.h"
#include "store/lmdb_store.h"

#define MAX_COLLECTIONS 256
#define BOOTSTRAP_PAGE  100

typedef struct {
    av_collection *c;
    char name[64];
    av_sync_state state;
    bool bootstrapped;
    av_cursor cur;
    bool have_cur;
    uint64_t reorgs;
    uint64_t divergence;
    time_t next_probe;
    time_t next_sweep;
    char last_error[256];
    bool seen; /* registry refresh marker */
} coll_state;

struct av_sync {
    av_sync_config cfg;
    pthread_t thread;
    atomic_bool stop;
    pthread_mutex_t mu; /* guards colls[] fields read by status/ready */
    coll_state colls[MAX_COLLECTIONS];
    size_t n_colls;
    av_ws *ws;
    bool ws_failed;
    _Atomic uint64_t head;
    char ws_url[512];
};

const char *av_sync_state_name(av_sync_state st)
{
    switch (st) {
    case AV_SYNC_INIT:
        return "init";
    case AV_SYNC_BOOTSTRAP:
        return "bootstrap";
    case AV_SYNC_CATCH_UP:
        return "catch_up";
    case AV_SYNC_LIVE:
        return "live";
    case AV_SYNC_REORG:
        return "reorg_recovery";
    case AV_SYNC_DEGRADED:
        return "degraded";
    }
    return "init";
}

static void set_state(av_sync *s, coll_state *cs, av_sync_state st, const char *err)
{
    pthread_mutex_lock(&s->mu);
    cs->state = st;
    if (cs->c != NULL) {
        av_collection_set_bootstrapping(cs->c, (st == AV_SYNC_BOOTSTRAP));
    }
    if (err != NULL) {
        snprintf(cs->last_error, sizeof(cs->last_error), "%.*s", (int) sizeof(cs->last_error) - 1, err);
    } else if (st == AV_SYNC_LIVE || st == AV_SYNC_CATCH_UP) {
        cs->last_error[0] = '\0';
    }
    pthread_mutex_unlock(&s->mu);
}

/* ------------------------------------------------------------------ membership */

static bool writer_ok(const av_coldef *d, const uint8_t addr[20])
{
    return d->membership == AV_MEMBERSHIP_OPEN || av_coldef_has_writer(d, addr);
}

/* Membership predicate as an Arkiv query (tag + writer scope). */
static av_status membership_query(const av_coldef *d, char *out, size_t cap, av_err *e)
{
    char lit[128];
    if (av_qb_lit_str(d->schema_tag, strlen(d->schema_tag), lit, sizeof(lit), e) != AV_OK) {
        return AV_ERR;
    }
    av_qb qb;
    av_qb_init(&qb, out, cap);
    if (av_qb_cmp(&qb, AV_ATTR_SCHEMA, "=", lit, e) != AV_OK) {
        return AV_ERR;
    }
    if (d->membership != AV_MEMBERSHIP_OPEN && d->n_writers > 0) {
        char writers_q[4096];
        if (av_qb_or_creators(writers_q, sizeof(writers_q), (const uint8_t (*)[20]) d->writers, d->n_writers,
                              e) != AV_OK) {
            return AV_ERR;
        }
        if (av_qb_and(&qb, e) != AV_OK || av_qb_raw(&qb, writers_q, e) != AV_OK) {
            return AV_ERR;
        }
    }
    return AV_OK;
}

/* ------------------------------------------------------------------ fetch helpers */

/* Insertion-ordered set of 32-byte keys: dense array + open-addressing index. */
typedef struct {
    uint8_t (*keys)[32];
    size_t n, cap;
    uint32_t *slots; /* index+1 into keys, 0 = empty */
    size_t nslots;   /* power of two, >= 2*cap */
} keyset;

static uint64_t key_hash(const uint8_t k[32])
{
    uint64_t h;
    memcpy(&h, k, 8); /* Arkiv keys are keccak outputs: uniformly distributed */
    return h ^ (h >> 29);
}

static int keyset_grow(keyset *ks)
{
    size_t nc = ks->cap ? ks->cap * 2 : 64;
    void *p = realloc(ks->keys, nc * 32);
    if (p == NULL) {
        return -1;
    }
    ks->keys = p;
    ks->cap = nc;
    size_t ns = nc * 2;
    uint32_t *sl = calloc(ns, sizeof(uint32_t));
    if (sl == NULL) {
        return -1;
    }
    for (size_t i = 0; i < ks->n; i++) {
        size_t j = (size_t) key_hash(ks->keys[i]) & (ns - 1);
        while (sl[j] != 0) {
            j = (j + 1) & (ns - 1);
        }
        sl[j] = (uint32_t) (i + 1);
    }
    free(ks->slots);
    ks->slots = sl;
    ks->nslots = ns;
    return 0;
}

static bool keyset_has(const keyset *ks, const uint8_t k[32])
{
    if (ks->nslots == 0) {
        return false;
    }
    size_t j = (size_t) key_hash(k) & (ks->nslots - 1);
    while (ks->slots[j] != 0) {
        if (memcmp(ks->keys[ks->slots[j] - 1], k, 32) == 0) {
            return true;
        }
        j = (j + 1) & (ks->nslots - 1);
    }
    return false;
}

static int keyset_add(keyset *ks, const uint8_t k[32])
{
    if (keyset_has(ks, k)) {
        return 0;
    }
    if (ks->n == ks->cap && keyset_grow(ks) != 0) {
        return -1;
    }
    memcpy(ks->keys[ks->n], k, 32);
    size_t j = (size_t) key_hash(k) & (ks->nslots - 1);
    while (ks->slots[j] != 0) {
        j = (j + 1) & (ks->nslots - 1);
    }
    ks->slots[j] = (uint32_t) (++ks->n);
    return 0;
}

static void keyset_free(keyset *ks)
{
    free(ks->keys);
    free(ks->slots);
    memset(ks, 0, sizeof(*ks));
}

/*
 * Fetch current state of keys (OR-chunks of $key = key(..)); entities found are
 * upserted, known keys not returned are deleted. Chunk halves on -32004.
 */
static av_status fetch_and_apply(av_sync *s, coll_state *cs, av_txn *t, const keyset *ks, av_arena *a,
                                 av_err *e)
{
    int chunk = s->cfg.fetch_chunk > 0 ? s->cfg.fetch_chunk : 32;
    size_t i = 0;
    while (i < ks->n) {
        size_t n = ks->n - i < (size_t) chunk ? ks->n - i : (size_t) chunk;
        char q[64 * 1024];
        if (av_qb_or_keys(q, sizeof(q), (const uint8_t (*)[32]) & ks->keys[i], n, e) != AV_OK) {
            return AV_ERR;
        }
        keyset found = {0};
        char *cursor = NULL;
        bool too_long = false;
        for (;;) {
            av_query_page page;
            int rpc_code = 0;
            if (av_rpc_query(s->cfg.rpc, q, cursor, BOOTSTRAP_PAGE, false, a, &page, &rpc_code, e) != AV_OK) {
                if (rpc_code == -32004 && chunk > 1) {
                    too_long = true;
                    break;
                }
                keyset_free(&found);
                return AV_ERR;
            }
            for (size_t k = 0; k < page.n; k++) {
                bool changed = false;
                if (keyset_add(&found, page.ents[k].key) != 0 ||
                    av_collection_upsert(cs->c, t, &page.ents[k], &changed, e) != AV_OK) {
                    keyset_free(&found);
                    return AV_ERR;
                }
            }
            if (page.next_cursor == NULL || page.n == 0) {
                break;
            }
            cursor = page.next_cursor;
        }
        if (too_long) {
            keyset_free(&found);
            chunk /= 2;
            continue; /* retry the same position with a smaller chunk */
        }
        for (size_t j = 0; j < n; j++) {
            if (!keyset_has(&found, ks->keys[i + j]) && av_collection_has_key(cs->c, ks->keys[i + j])) {
                if (av_collection_delete(cs->c, t, ks->keys[i + j], e) != AV_OK) {
                    keyset_free(&found);
                    return AV_ERR;
                }
            }
        }
        keyset_free(&found);
        i += n;
    }
    return AV_OK;
}

/* ------------------------------------------------------------------ bootstrap / sweep */

typedef struct {
    keyset *seen;
} sweep_ctx;

static bool collect_unseen(const uint8_t key[32], uint64_t updated_at, uint64_t expires_at, void *ctx)
{
    (void) updated_at;
    (void) expires_at;
    keyset *out = ctx;
    return keyset_add(out, key) == 0;
}

/*
 * Full membership scan at block 'at': every member entity is upserted (monotonic),
 * every local key the scan did not return is deleted, cursor := (at, hash), publish.
 * Used for bootstrap, --resync, deep reorgs and reconcile sweeps.
 */
static av_status full_scan(av_sync *s, coll_state *cs, uint64_t at, av_err *e)
{
    const av_coldef *d = av_collection_def(cs->c);
    av_block blk;
    if (av_rpc_get_block(s->cfg.rpc, at, &blk, e) != AV_OK) {
        return AV_ERR;
    }
    char q[64 * 1024];
    if (membership_query(d, q, sizeof(q), e) != AV_OK) {
        return AV_ERR;
    }
    av_arena *a = av_arena_create("scan", 1u << 20, 0);
    keyset seen = {0};
    char cursor_buf[1024];
    char *cursor = NULL;
    av_status st = AV_OK;
    av_txn *t = NULL;
    for (;;) {
        av_query_page page;
        int rpc_code = 0;
        if (av_rpc_query(s->cfg.rpc, q, cursor, BOOTSTRAP_PAGE, false, a, &page, &rpc_code, e) != AV_OK) {
            st = AV_ERR;
            break;
        }
        if (av_store_begin(s->cfg.store, &t, e) != AV_OK) {
            st = AV_ERR;
            break;
        }
        for (size_t i = 0; i < page.n && st == AV_OK; i++) {
            bool changed = false;
            if (keyset_add(&seen, page.ents[i].key) != 0) {
                av_err_set(e, "internal", "out of memory");
                st = AV_ERR;
            } else if (av_collection_upsert(cs->c, t, &page.ents[i], &changed, e) != AV_OK) {
                st = AV_ERR;
            }
        }
        if (st != AV_OK) {
            av_store_abort(t);
            t = NULL;
            break;
        }
        if (av_store_commit(t, e) != AV_OK) {
            t = NULL;
            st = AV_ERR;
            break;
        }
        t = NULL;
        if (page.next_cursor == NULL || page.n == 0) {
            break;
        }
        /* keep only the cursor; release the page (bounded memory for large collections) */
        snprintf(cursor_buf, sizeof(cursor_buf), "%s", page.next_cursor);
        cursor = cursor_buf;
        av_arena_reset(a);
    }
    if (st == AV_OK) {
        /* delete local keys the chain did not return, then commit the cursor and publish */
        keyset local = {0};
        av_collection_each_live(cs->c, collect_unseen, &local);
        if (av_store_begin(s->cfg.store, &t, e) == AV_OK) {
            for (size_t i = 0; i < local.n && st == AV_OK; i++) {
                if (!keyset_has(&seen, local.keys[i])) {
                    pthread_mutex_lock(&s->mu);
                    cs->divergence += cs->bootstrapped ? 1 : 0;
                    pthread_mutex_unlock(&s->mu);
                    st = av_collection_delete(cs->c, t, local.keys[i], e);
                }
            }
            av_cursor c = {.block = at};
            memcpy(c.hash, blk.hash, 32);
            if (st == AV_OK) {
                st = av_store_put_cursor(t, d->name, &c, e);
            }
            if (st == AV_OK) {
                st = av_store_ring_put(t, d->name, at, blk.hash, NULL, 0, e);
            }
            if (st == AV_OK) {
                st = av_store_commit(t, e);
            } else {
                av_store_abort(t);
            }
            if (st == AV_OK) {
                av_collection_publish(cs->c, at);
                cs->cur = c;
                cs->have_cur = true;
            }
        } else {
            st = AV_ERR;
        }
        keyset_free(&local);
    }
    keyset_free(&seen);
    av_arena_destroy(a);
    return st;
}

/* ------------------------------------------------------------------ live loop */

/* Is this event relevant for the collection? (decided locally; see design notes) */
static bool relevant(const av_coldef *d, av_collection *c, const av_event *ev)
{
    if (av_collection_has_key(c, ev->key)) {
        return true;
    }
    switch (ev->kind) {
    case AV_EV_CREATED:
    case AV_EV_PATCHED: /* owner (= creator at creation) in writers; patches may add the tag */
        return writer_ok(d, ev->owner);
    case AV_EV_TRANSFERRED:
        return d->membership == AV_MEMBERSHIP_OWNER && writer_ok(d, ev->new_owner);
    default:
        return false;
    }
}

/* Process blocks (cur, to]: logs -> dirty keys -> fetch -> apply -> cursor -> publish. */
static av_status process_range(av_sync *s, coll_state *cs, uint64_t to, bool *reorg, av_err *e)
{
    const av_coldef *d = av_collection_def(cs->c);
    uint64_t from = cs->cur.block + 1;
    av_block bfrom, bto;
    *reorg = false;
    if (av_rpc_get_block(s->cfg.rpc, from, &bfrom, e) != AV_OK) {
        return AV_ERR;
    }
    if (memcmp(bfrom.parent_hash, cs->cur.hash, 32) != 0) {
        *reorg = true;
        return AV_OK;
    }
    if (to == from) {
        bto = bfrom;
    } else if (av_rpc_get_block(s->cfg.rpc, to, &bto, e) != AV_OK) {
        return AV_ERR;
    }
    av_arena *a = av_arena_create("range", 1u << 20, 0);
    av_event *evs = NULL;
    size_t n = 0;
    int rpc_code = 0;
    av_status st = av_rpc_get_logs(s->cfg.rpc, from, to, NULL, a, &evs, &n, &rpc_code, e);
    keyset dirty = {0}, touched = {0};
    av_txn *t = NULL;
    if (st == AV_OK) {
        st = av_store_begin(s->cfg.store, &t, e);
    }
    /* Pass 1: decide per key; deletes/extends apply from log data unless a fetch is due. */
    for (size_t i = 0; st == AV_OK && i < n; i++) {
        const av_event *ev = &evs[i];
        if (ev->removed || !relevant(d, cs->c, ev)) {
            continue;
        }
        keyset_add(&touched, ev->key);
        switch (ev->kind) {
        case AV_EV_CREATED:
        case AV_EV_PATCHED:
        case AV_EV_TRANSFERRED:
            if (keyset_add(&dirty, ev->key) != 0) {
                av_err_set(e, "internal", "out of memory");
                st = AV_ERR;
            }
            break;
        default:
            break;
        }
    }
    for (size_t i = 0; st == AV_OK && i < n; i++) {
        const av_event *ev = &evs[i];
        if (ev->removed || keyset_has(&dirty, ev->key) || !av_collection_has_key(cs->c, ev->key)) {
            continue;
        }
        if (ev->kind == AV_EV_DELETED) {
            st = av_collection_delete(cs->c, t, ev->key, e);
        } else if (ev->kind == AV_EV_EXTENDED) {
            st = av_collection_extend(cs->c, t, ev->key, ev->expires_at, e);
        }
    }
    if (st == AV_OK && dirty.n > 0) {
        st = fetch_and_apply(s, cs, t, &dirty, a, e);
    }
    av_cursor c = {.block = to};
    memcpy(c.hash, bto.hash, 32);
    if (st == AV_OK) {
        st = av_store_ring_put(t, d->name, to, bto.hash, (const uint8_t (*)[32]) touched.keys, touched.n, e);
    }
    if (st == AV_OK) {
        st = av_store_put_cursor(t, d->name, &c, e);
    }
    if (st == AV_OK) {
        st = av_collection_expire(cs->c, t, to, s->cfg.ttl_budget, e);
    }
    if (t != NULL) {
        if (st == AV_OK) {
            st = av_store_commit(t, e);
        } else {
            av_store_abort(t);
        }
    }
    if (st == AV_OK) {
        av_collection_publish(cs->c, to);
        cs->cur = c;
    }
    keyset_free(&dirty);
    keyset_free(&touched);
    av_arena_destroy(a);
    return st;
}

/*
 * Reorg recovery: find the newest ring entry whose hash is still canonical, re-fetch
 * the keys touched after it, rewind the cursor. Returns AV_ERR with code "not_ready"
 * when no ring entry matches (caller re-bootstraps).
 */
static av_status recover_reorg(av_sync *s, coll_state *cs, av_err *e)
{
    const av_coldef *d = av_collection_def(cs->c);
    av_ring_entry *ring = NULL;
    size_t n = 0;
    av_arena *a = av_arena_create("reorg", 1u << 20, 0);
    av_status st = av_store_ring_list(s->cfg.store, d->name, a, &ring, &n, e); /* newest first */
    size_t anc = n;
    for (size_t i = 0; st == AV_OK && i < n; i++) {
        av_block b;
        if (av_rpc_get_block(s->cfg.rpc, ring[i].block, &b, e) != AV_OK) {
            st = AV_ERR;
            break;
        }
        if (memcmp(b.hash, ring[i].hash, 32) == 0) {
            anc = i;
            break;
        }
    }
    if (st == AV_OK && anc == n) {
        av_err_set(e, "not_ready", "reorg deeper than the ring");
        st = AV_ERR;
    }
    if (st == AV_OK) {
        keyset dirty = {0};
        for (size_t i = 0; i < anc; i++) {
            for (size_t k = 0; k < ring[i].n_keys; k++) {
                keyset_add(&dirty, ring[i].keys[k]);
            }
        }
        av_txn *t = NULL;
        st = av_store_begin(s->cfg.store, &t, e);
        if (st == AV_OK) {
            st = fetch_and_apply(s, cs, t, &dirty, a, e);
        }
        av_cursor c = {.block = ring[anc].block};
        memcpy(c.hash, ring[anc].hash, 32);
        if (st == AV_OK) {
            st = av_store_ring_drop_above(t, d->name, ring[anc].block, e);
        }
        if (st == AV_OK) {
            st = av_store_put_cursor(t, d->name, &c, e);
        }
        if (t != NULL) {
            if (st == AV_OK) {
                st = av_store_commit(t, e);
            } else {
                av_store_abort(t);
            }
        }
        if (st == AV_OK) {
            /* keep as_of monotonic for readers: the refetched state is the current head's */
            cs->cur = c;
            pthread_mutex_lock(&s->mu);
            cs->reorgs++;
            pthread_mutex_unlock(&s->mu);
            av_log(AV_LOG_WARN, "sync", "collection %s: reorg recovered at block %llu (%zu keys refetched)",
                   d->name, (unsigned long long) c.block, dirty.n);
        }
        keyset_free(&dirty);
    }
    av_arena_destroy(a);
    return st;
}

/* Headless count probe vs local live count. */
static av_status count_probe(av_sync *s, coll_state *cs, bool *mismatch, av_err *e)
{
    const av_coldef *d = av_collection_def(cs->c);
    uint64_t as_of = av_collection_as_of(cs->c);
    char q[64 * 1024];
    if (membership_query(d, q, sizeof(q), e) != AV_OK) {
        return AV_ERR;
    }
    uint64_t remote = 0;
    if (av_rpc_count(s->cfg.rpc, q, as_of, &remote, e) != AV_OK) {
        return AV_ERR;
    }
    uint64_t local = av_collection_live_count(cs->c);
    /* tolerate in-flight blocks: the count is at the node's head, ours at as_of */
    *mismatch = remote != local && av_sync_head(s) <= as_of + s->cfg.confirmations + 1;
    return AV_OK;
}

/* ------------------------------------------------------------------ main loop */

static void step_collection(av_sync *s, coll_state *cs, uint64_t target)
{
    av_err e;
    av_err_clear(&e);
    const av_coldef *d = av_collection_def(cs->c);
    time_t now = time(NULL);
    switch (cs->state) {
    case AV_SYNC_INIT: {
        bool found = false;
        if (!s->cfg.resync && av_store_get_cursor(s->cfg.store, d->name, &cs->cur, &found, &e) == AV_OK &&
            found) {
            cs->have_cur = true;
            cs->bootstrapped = true;
            av_collection_publish(cs->c, cs->cur.block); /* restored state is visible again */
            av_block b;
            if (av_rpc_get_block(s->cfg.rpc, cs->cur.block, &b, &e) != AV_OK) {
                set_state(s, cs, AV_SYNC_DEGRADED, e.msg);
                return;
            }
            set_state(s, cs, memcmp(b.hash, cs->cur.hash, 32) == 0 ? AV_SYNC_CATCH_UP : AV_SYNC_REORG, NULL);
        } else {
            set_state(s, cs, AV_SYNC_BOOTSTRAP, NULL);
        }
        return;
    }
    case AV_SYNC_BOOTSTRAP:
        if (target == 0) {
            return;
        }
        if (full_scan(s, cs, target, &e) != AV_OK) {
            set_state(s, cs, AV_SYNC_DEGRADED, e.msg);
            return;
        }
        pthread_mutex_lock(&s->mu);
        cs->bootstrapped = true;
        pthread_mutex_unlock(&s->mu);
        cs->next_probe = now + s->cfg.reconcile_interval_s;
        cs->next_sweep = now + s->cfg.sweep_interval_s;
        av_log(AV_LOG_INFO, "sync", "collection %s: bootstrap done at block %llu (%zu live)", d->name,
               (unsigned long long) target, av_collection_live_count(cs->c));
        set_state(s, cs, AV_SYNC_CATCH_UP, NULL);
        return;
    case AV_SYNC_REORG:
        if (recover_reorg(s, cs, &e) != AV_OK) {
            if (strcmp(e.code, "not_ready") == 0) {
                av_log(AV_LOG_WARN, "sync", "collection %s: %s, re-bootstrapping", d->name, e.msg);
                set_state(s, cs, AV_SYNC_BOOTSTRAP, NULL);
            } else {
                set_state(s, cs, AV_SYNC_DEGRADED, e.msg);
            }
            return;
        }
        set_state(s, cs, AV_SYNC_CATCH_UP, NULL);
        return;
    case AV_SYNC_DEGRADED:
        /* retry from the last good position */
        set_state(s, cs, cs->bootstrapped ? AV_SYNC_CATCH_UP : AV_SYNC_BOOTSTRAP, NULL);
        return;
    case AV_SYNC_CATCH_UP:
    case AV_SYNC_LIVE: {
        int batches = 0;
        while (cs->cur.block < target && batches++ < 16 && !atomic_load(&s->stop)) {
            uint64_t range = s->cfg.max_log_range ? s->cfg.max_log_range : 1000;
            uint64_t to = cs->cur.block + range;
            if (to > target) {
                to = target;
            }
            bool reorg = false;
            if (process_range(s, cs, to, &reorg, &e) != AV_OK) {
                set_state(s, cs, AV_SYNC_DEGRADED, e.msg);
                return;
            }
            if (reorg) {
                set_state(s, cs, AV_SYNC_REORG, NULL);
                return;
            }
        }
        set_state(s, cs, target - cs->cur.block <= s->cfg.live_lag_blocks ? AV_SYNC_LIVE : AV_SYNC_CATCH_UP,
                  NULL);
        /* reconciliation */
        if (cs->state == AV_SYNC_LIVE && now >= cs->next_probe) {
            bool mismatch = false;
            cs->next_probe = now + s->cfg.reconcile_interval_s;
            if (count_probe(s, cs, &mismatch, &e) == AV_OK && (mismatch || now >= cs->next_sweep)) {
                cs->next_sweep = now + s->cfg.sweep_interval_s;
                if (mismatch) {
                    av_log(AV_LOG_WARN, "sync", "collection %s: count mismatch, sweeping", d->name);
                }
                if (full_scan(s, cs, cs->cur.block, &e) != AV_OK) {
                    set_state(s, cs, AV_SYNC_DEGRADED, e.msg);
                }
            }
        }
        return;
    }
    }
}

typedef struct {
    av_sync *s;
} refresh_ctx;

static bool refresh_cb(av_collection *c, void *ctx)
{
    av_sync *s = ((refresh_ctx *) ctx)->s;
    const char *name = av_collection_def(c)->name;
    for (size_t i = 0; i < s->n_colls; i++) {
        if (s->colls[i].c == c) {
            s->colls[i].seen = true;
            return true;
        }
    }
    if (s->n_colls < MAX_COLLECTIONS) {
        pthread_mutex_lock(&s->mu);
        coll_state *cs = &s->colls[s->n_colls++];
        memset(cs, 0, sizeof(*cs));
        cs->c = c;
        snprintf(cs->name, sizeof(cs->name), "%s", name);
        cs->state = AV_SYNC_INIT;
        cs->seen = true;
        pthread_mutex_unlock(&s->mu);
    }
    return true;
}

static void refresh_collections(av_sync *s)
{
    for (size_t i = 0; i < s->n_colls; i++) {
        s->colls[i].seen = false;
    }
    refresh_ctx ctx = {s};
    av_registry_each(s->cfg.registry, refresh_cb, &ctx);
    /* drop collections deleted from the registry */
    pthread_mutex_lock(&s->mu);
    size_t w = 0;
    for (size_t i = 0; i < s->n_colls; i++) {
        if (s->colls[i].seen) {
            s->colls[w++] = s->colls[i];
        }
    }
    s->n_colls = w;
    pthread_mutex_unlock(&s->mu);
}

static uint64_t observe_head(av_sync *s)
{
    uint64_t h = 0;
    av_err e;
    if (s->ws == NULL && !s->ws_failed && s->ws_url[0] != '\0') {
        s->ws = av_ws_open(s->ws_url, s->cfg.access_key, &e);
        if (s->ws == NULL) {
            s->ws_failed = true; /* fall back to polling (retried by av_ws_open next start) */
        }
    }
    if (s->ws != NULL) {
        int r = av_ws_wait(s->ws, s->cfg.poll_interval_ms, &h);
        if (r < 0) {
            av_ws_close(s->ws);
            s->ws = NULL;
        }
        if (r == 1) {
            return h;
        }
    } else {
        struct timespec ts = {s->cfg.poll_interval_ms / 1000,
                              (long) (s->cfg.poll_interval_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
    if (av_rpc_block_number(s->cfg.rpc, &h, &e) != AV_OK) {
        return 0;
    }
    return h;
}

static void *sync_main(void *arg)
{
    av_sync *s = arg;
    while (!atomic_load(&s->stop)) {
        uint64_t head = observe_head(s);
        if (head != 0) {
            atomic_store(&s->head, head);
        }
        head = atomic_load(&s->head);
        uint64_t target = head > s->cfg.confirmations ? head - s->cfg.confirmations : 0;
        refresh_collections(s);
        for (size_t i = 0; i < s->n_colls && !atomic_load(&s->stop); i++) {
            step_collection(s, &s->colls[i], target);
        }
    }
    if (s->ws != NULL) {
        av_ws_close(s->ws);
    }
    return NULL;
}

av_status av_sync_start(const av_sync_config *cfg, av_sync **out, av_err *e)
{
    av_sync *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    s->cfg = *cfg;
    if (s->cfg.poll_interval_ms <= 0) {
        s->cfg.poll_interval_ms = 1000;
    }
    if (s->cfg.ttl_budget == 0) {
        s->cfg.ttl_budget = 10000;
    }
    if (s->cfg.reconcile_interval_s <= 0) {
        s->cfg.reconcile_interval_s = 60;
    }
    if (s->cfg.sweep_interval_s <= 0) {
        s->cfg.sweep_interval_s = 6 * 3600;
    }
    if (cfg->ws_url != NULL) {
        snprintf(s->ws_url, sizeof(s->ws_url), "%s", cfg->ws_url);
    }
    pthread_mutex_init(&s->mu, NULL);
    atomic_store(&s->stop, false);
    if (pthread_create(&s->thread, NULL, sync_main, s) != 0) {
        pthread_mutex_destroy(&s->mu);
        free(s);
        av_err_set(e, "internal", "cannot start sync thread");
        return AV_ERR;
    }
    *out = s;
    return AV_OK;
}

void av_sync_stop(av_sync *s)
{
    if (s == NULL) {
        return;
    }
    atomic_store(&s->stop, true);
    pthread_join(s->thread, NULL);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

size_t av_sync_status_list(av_sync *s, av_sync_coll_status *out, size_t max)
{
    uint64_t head = atomic_load(&s->head);
    pthread_mutex_lock(&s->mu);
    size_t n = s->n_colls;
    for (size_t i = 0; i < n && i < max; i++) {
        coll_state *cs = &s->colls[i];
        av_sync_coll_status *o = &out[i];
        memset(o, 0, sizeof(*o));
        snprintf(o->collection, sizeof(o->collection), "%s", cs->name);
        o->state = cs->state;
        o->as_of_block = av_collection_as_of(cs->c);
        o->head_block = head;
        o->lag_blocks = head > o->as_of_block ? head - o->as_of_block : 0;
        o->live_entities = av_collection_live_count(cs->c);
        o->reorgs = cs->reorgs;
        o->reconcile_divergence = cs->divergence;
        snprintf(o->last_error, sizeof(o->last_error), "%s", cs->last_error);
    }
    pthread_mutex_unlock(&s->mu);
    return n;
}

void av_sync_rest_provider(av_sync_visit_fn visit, void *visit_ctx, void *provider_ctx)
{
    av_sync *s = (av_sync *) provider_ctx;
    if (s == NULL || visit == NULL)
        return;
    av_sync_coll_status st[MAX_COLLECTIONS];
    size_t n = av_sync_status_list(s, st, MAX_COLLECTIONS);
    for (size_t i = 0; i < n; i++) {
        av_sync_status item = {
            .name = st[i].collection,
            .state = av_sync_state_name(st[i].state),
            .as_of_block = st[i].as_of_block,
            .head_block = st[i].head_block,
            .last_error = st[i].last_error[0] ? st[i].last_error : NULL,
        };
        visit(&item, visit_ctx);
    }
}

bool av_sync_ready(av_sync *s)
{
    bool ready = true;
    pthread_mutex_lock(&s->mu);
    for (size_t i = 0; i < s->n_colls; i++) {
        if (!s->colls[i].bootstrapped) {
            ready = false;
        }
    }
    pthread_mutex_unlock(&s->mu);
    return ready;
}

uint64_t av_sync_head(av_sync *s)
{
    return atomic_load(&s->head);
}
