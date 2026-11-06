/*
 * core/registry.c: see registry.h.
 */
#include "core/registry.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct av_registry {
    av_store *store;
    char *data_dir;
    pthread_rwlock_t lock;
    av_collection **cols; /* sorted by name */
    size_t n, cap;
};

static size_t lower_bound(const av_registry *r, const char *name)
{
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(av_collection_def(r->cols[mid])->name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static av_collection *find_locked(const av_registry *r, const char *name, size_t *pos)
{
    size_t i = lower_bound(r, name);
    if (pos != NULL)
        *pos = i;
    return (i < r->n && strcmp(av_collection_def(r->cols[i])->name, name) == 0) ? r->cols[i] : NULL;
}

static av_status insert_locked(av_registry *r, av_collection *c, av_err *e)
{
    if (r->n == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 8;
        av_collection **nb = realloc(r->cols, nc * sizeof *nb);
        if (nb == NULL) {
            av_err_set(e, "internal", "out of memory");
            return AV_ERR;
        }
        r->cols = nb;
        r->cap = nc;
    }
    size_t pos = lower_bound(r, av_collection_def(c)->name);
    memmove(&r->cols[pos + 1], &r->cols[pos], (r->n - pos) * sizeof *r->cols);
    r->cols[pos] = c;
    r->n++;
    return AV_OK;
}

typedef struct {
    av_registry *r;
    av_err *e;
    av_status st;
} load_ctx;

static bool load_one(const char *name, const char *json, size_t len, void *p)
{
    load_ctx *l = p;
    av_arena *a = av_arena_create("coldef", (size_t) 1 << 14, 0);
    if (a == NULL) {
        av_err_set(l->e, "internal", "out of memory");
        l->st = AV_ERR;
        return false;
    }
    av_coldef def;
    av_err pe;
    av_err_clear(&pe);
    if (av_coldef_from_text(json, len, a, true, &def, &pe) != AV_OK || strcmp(def.name, name) != 0) {
        av_err_set(l->e, "internal", "stored definition of collection '%s' is invalid: %s", name,
                   pe.msg[0] ? pe.msg : "name mismatch");
        l->st = AV_ERR;
        av_arena_destroy(a);
        return false;
    }
    av_collection *c = av_collection_create(&def, l->r->store, l->r->data_dir, l->e);
    av_arena_destroy(a);
    if (c == NULL || insert_locked(l->r, c, l->e) != AV_OK) {
        av_collection_release(c);
        l->st = AV_ERR;
        return false;
    }
    return true;
}

av_registry *av_registry_create(av_store *store, const char *data_dir, av_err *e)
{
    av_registry *r = calloc(1, sizeof *r);
    if (r == NULL) {
        av_err_set(e, "internal", "out of memory");
        return NULL;
    }
    r->store = store;
    if (data_dir != NULL) {
        r->data_dir = strdup(data_dir);
        if (r->data_dir == NULL) {
            free(r);
            av_err_set(e, "internal", "out of memory");
            return NULL;
        }
    }
    (void) pthread_rwlock_init(&r->lock, NULL);
    load_ctx l = {r, e, AV_OK};
    if (av_store_list_collections(store, load_one, &l, e) != AV_OK || l.st != AV_OK) {
        av_registry_destroy(r);
        return NULL;
    }
    return r;
}

void av_registry_destroy(av_registry *r)
{
    if (r == NULL)
        return;
    for (size_t i = 0; i < r->n; i++)
        av_collection_release(r->cols[i]);
    free(r->cols);
    free(r->data_dir);
    (void) pthread_rwlock_destroy(&r->lock);
    free(r);
}

av_status av_registry_add(av_registry *r, const av_coldef *def, av_collection **out, av_err *e)
{
    (void) pthread_rwlock_wrlock(&r->lock);
    av_status st = AV_ERR;
    av_collection *c = NULL;
    char *json = NULL;
    if (find_locked(r, def->name, NULL) != NULL) {
        av_err_set(e, "collection_exists", "collection '%s' already exists", def->name);
        goto done;
    }
    size_t jl;
    json = av_coldef_to_json(def, &jl);
    if (json == NULL) {
        av_err_set(e, "internal", "cannot serialize definition");
        goto done;
    }
    av_txn *t;
    if (av_store_begin(r->store, &t, e) != AV_OK)
        goto done;
    if (av_store_put_collection(t, def->name, json, jl, e) != AV_OK) {
        av_store_abort(t);
        goto done;
    }
    if (av_store_commit(t, e) != AV_OK)
        goto done;
    c = av_collection_create(def, r->store, r->data_dir, e);
    if (c == NULL || insert_locked(r, c, e) != AV_OK) {
        /* Undo the persisted definition so a restart does not trip over it. */
        av_err ignore;
        if (av_store_begin(r->store, &t, &ignore) == AV_OK) {
            if (av_store_del_collection(t, def->name, &ignore) == AV_OK)
                (void) av_store_commit(t, &ignore);
            else
                av_store_abort(t);
        }
        av_collection_release(c);
        c = NULL;
        goto done;
    }
    av_collection_retain(c);
    *out = c;
    st = AV_OK;
done:
    free(json);
    (void) pthread_rwlock_unlock(&r->lock);
    return st;
}

av_collection *av_registry_get(av_registry *r, const char *name)
{
    (void) pthread_rwlock_rdlock(&r->lock);
    av_collection *c = find_locked(r, name, NULL);
    if (c != NULL)
        av_collection_retain(c);
    (void) pthread_rwlock_unlock(&r->lock);
    return c;
}

av_status av_registry_remove(av_registry *r, const char *name, av_err *e)
{
    (void) pthread_rwlock_wrlock(&r->lock);
    size_t pos;
    av_collection *c = find_locked(r, name, &pos);
    if (c == NULL) {
        (void) pthread_rwlock_unlock(&r->lock);
        av_err_set(e, "collection_not_found", "collection '%s' does not exist", name);
        return AV_ERR;
    }
    av_txn *t;
    if (av_store_begin(r->store, &t, e) != AV_OK) {
        (void) pthread_rwlock_unlock(&r->lock);
        return AV_ERR;
    }
    if (av_store_del_collection(t, name, e) != AV_OK) {
        av_store_abort(t);
        (void) pthread_rwlock_unlock(&r->lock);
        return AV_ERR;
    }
    if (av_store_commit(t, e) != AV_OK) {
        (void) pthread_rwlock_unlock(&r->lock);
        return AV_ERR;
    }
    memmove(&r->cols[pos], &r->cols[pos + 1], (r->n - pos - 1) * sizeof *r->cols);
    r->n--;
    (void) pthread_rwlock_unlock(&r->lock);
    av_collection_mark_dropped(c);
    av_collection_release(c);
    return AV_OK;
}

size_t av_registry_count(av_registry *r)
{
    (void) pthread_rwlock_rdlock(&r->lock);
    size_t n = r->n;
    (void) pthread_rwlock_unlock(&r->lock);
    return n;
}

void av_registry_each(av_registry *r, av_registry_cb cb, void *ctx)
{
    (void) pthread_rwlock_rdlock(&r->lock);
    size_t n = r->n;
    av_collection **snap = malloc((n ? n : 1) * sizeof *snap);
    if (snap != NULL)
        for (size_t i = 0; i < n; i++) {
            snap[i] = r->cols[i];
            av_collection_retain(snap[i]);
        }
    (void) pthread_rwlock_unlock(&r->lock);
    if (snap == NULL)
        return;
    bool go = true;
    for (size_t i = 0; i < n; i++) {
        if (go)
            go = cb(snap[i], ctx);
        av_collection_release(snap[i]);
    }
    free(snap);
}
