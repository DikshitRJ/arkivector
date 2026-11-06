/*
 * store/lmdb_store.c: LMDB persistence. See lmdb_store.h (threading) and schema.h (layout).
 */
#include "store/lmdb_store.h"

#include <errno.h>
#include <lmdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "store/schema.h"

#define NAME_CAP 64
#define MAX_DBS  4096u

enum { DB_ENT, DB_IDS, DB_CUR, DB_RING, DB_QUAR, DB_N };
static const char *const db_prefix[DB_N] = {"entities:", "ids:", "cursor:", "ring:", "quarantine:"};

typedef struct {
    char name[NAME_CAP];
    MDB_dbi dbi[DB_N];
} coll_dbs;

struct av_store {
    MDB_env *env;
    MDB_dbi meta;
    MDB_dbi colls;
    pthread_mutex_t mu; /* guards cache/n_cache/cap_cache */
    coll_dbs *cache;
    size_t n_cache, cap_cache;
};

struct av_txn {
    av_store *s;
    MDB_txn *txn;
    coll_dbs *pend; /* opened in this txn, published at commit */
    size_t n_pend, cap_pend;
    char (*dropped)[NAME_CAP];
    size_t n_drop, cap_drop;
};

/* ------------------------------------------------------------------ helpers */

static av_status mdb_fail(av_err *e, const char *op, int rc)
{
    av_err_set(e, "internal", "lmdb %s: %s", op, mdb_strerror(rc));
    return AV_ERR;
}

static av_status corrupt(av_err *e, const char *what)
{
    av_err_set(e, "internal", "store corruption: %s", what);
    return AV_ERR;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t) v);
    put_le32(p + 4, (uint32_t) (v >> 32));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t) get_le32(p) | ((uint64_t) get_le32(p + 4) << 32);
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t) (v >> (56 - 8 * i));
}

static uint64_t get_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | (uint64_t) p[i];
    return v;
}

static bool name_ok(const char *name, av_err *e)
{
    if (name == NULL || name[0] == '\0' || strlen(name) + 1 > NAME_CAP) {
        av_err_set(e, "internal", "invalid collection name for the store");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------ attribute blob */

av_status av_attrs_pack(const av_attr *attrs, size_t n, av_arena *a, uint8_t **out, size_t *len, av_err *e)
{
    if (n > UINT16_MAX) {
        av_err_set(e, "invalid_argument", "too many attributes (%zu)", n);
        return AV_ERR;
    }
    size_t total = 2;
    for (size_t i = 0; i < n; i++) {
        size_t nl = strlen(attrs[i].name);
        if (nl == 0 || nl > AV_ATTR_NAME_MAX || attrs[i].len > UINT32_MAX) {
            av_err_set(e, "invalid_argument", "attribute %zu has an invalid name or value length", i);
            return AV_ERR;
        }
        total += 3 + nl + 4 + attrs[i].len;
    }
    uint8_t *buf = av_arena_alloc(a, total, 1);
    if (buf == NULL) {
        av_err_set(e, "internal", "arena exhausted packing attributes");
        return AV_ERR;
    }
    uint8_t *p = buf;
    *p++ = (uint8_t) n;
    *p++ = (uint8_t) (n >> 8);
    for (size_t i = 0; i < n; i++) {
        size_t nl = strlen(attrs[i].name);
        *p++ = attrs[i].type;
        *p++ = attrs[i].indexed ? 1 : 0;
        *p++ = (uint8_t) nl;
        memcpy(p, attrs[i].name, nl);
        p += nl;
        put_le32(p, (uint32_t) attrs[i].len);
        p += 4;
        if (attrs[i].len > 0)
            memcpy(p, attrs[i].val, attrs[i].len);
        p += attrs[i].len;
    }
    *out = buf;
    *len = total;
    return AV_OK;
}

av_status av_attrs_unpack(const uint8_t *buf, size_t len, av_arena *a, av_attr **out, size_t *n, av_err *e)
{
    if (len < 2)
        return corrupt(e, "attribute blob too short");
    size_t count = (size_t) buf[0] | ((size_t) buf[1] << 8);
    size_t pos = 2;
    /* Every attribute needs at least 8 bytes, so count is bounded by len before allocating. */
    if (count > (len - 2) / 8)
        return corrupt(e, "attribute count exceeds blob size");
    av_attr *attrs = av_arena_calloc(a, (count ? count : 1) * sizeof(av_attr), 8);
    if (attrs == NULL) {
        av_err_set(e, "internal", "arena exhausted unpacking attributes");
        return AV_ERR;
    }
    for (size_t i = 0; i < count; i++) {
        if (len - pos < 3)
            return corrupt(e, "attribute header truncated");
        uint8_t type = buf[pos], indexed = buf[pos + 1];
        size_t nl = buf[pos + 2];
        pos += 3;
        if (nl == 0 || nl > AV_ATTR_NAME_MAX || len - pos < nl + 4)
            return corrupt(e, "attribute name truncated");
        memcpy(attrs[i].name, buf + pos, nl);
        attrs[i].name[nl] = '\0';
        pos += nl;
        size_t vl = get_le32(buf + pos);
        pos += 4;
        if (vl > len - pos)
            return corrupt(e, "attribute value truncated");
        uint8_t *val = av_arena_alloc(a, vl ? vl : 1, 8);
        if (val == NULL) {
            av_err_set(e, "internal", "arena exhausted unpacking attributes");
            return AV_ERR;
        }
        if (vl > 0)
            memcpy(val, buf + pos, vl);
        pos += vl;
        attrs[i].type = type;
        attrs[i].indexed = indexed != 0;
        attrs[i].val = val;
        attrs[i].len = vl;
    }
    if (pos != len)
        return corrupt(e, "trailing bytes after attribute blob");
    *out = attrs;
    *n = count;
    return AV_OK;
}

/* --------------------------------------------------------------------- rows */

static uint8_t *row_pack(const av_row *r, size_t *len)
{
    size_t total = AV_ROW_HEADER_SIZE + r->attrs_len;
    uint8_t *b = malloc(total);
    if (b == NULL)
        return NULL;
    b[0] = (uint8_t) AV_ROW_VERSION;
    b[1] = 0;
    put_le32(b + 2, r->docid);
    memcpy(b + 6, r->owner, 20);
    memcpy(b + 26, r->creator, 20);
    put_le64(b + 46, r->created_at);
    put_le64(b + 54, r->updated_at);
    put_le64(b + 62, r->expires_at);
    memcpy(b + 70, r->vec_hash, 32);
    put_le32(b + 102, (uint32_t) r->attrs_len);
    if (r->attrs_len > 0)
        memcpy(b + AV_ROW_HEADER_SIZE, r->attrs_blob, r->attrs_len);
    *len = total;
    return b;
}

/* attrs_blob in *r points into v. */
static av_status row_parse(const MDB_val *v, av_row *r, av_err *e)
{
    const uint8_t *b = v->mv_data;
    if (v->mv_size < AV_ROW_HEADER_SIZE || b[0] != AV_ROW_VERSION)
        return corrupt(e, "bad entity row header");
    size_t al = get_le32(b + 102);
    if (al != v->mv_size - AV_ROW_HEADER_SIZE)
        return corrupt(e, "entity row length mismatch");
    r->docid = get_le32(b + 2);
    memcpy(r->owner, b + 6, 20);
    memcpy(r->creator, b + 26, 20);
    r->created_at = get_le64(b + 46);
    r->updated_at = get_le64(b + 54);
    r->expires_at = get_le64(b + 62);
    memcpy(r->vec_hash, b + 70, 32);
    r->attrs_blob = b + AV_ROW_HEADER_SIZE;
    r->attrs_len = al;
    return AV_OK;
}

/* --------------------------------------------------------------- dbi cache */

static void db_name(char out[NAME_CAP + 16], int which, const char *coll)
{
    (void) snprintf(out, NAME_CAP + 16, "%s%s", db_prefix[which], coll);
}

static coll_dbs *cache_find(av_store *s, const char *name)
{
    for (size_t i = 0; i < s->n_cache; i++)
        if (strcmp(s->cache[i].name, name) == 0)
            return &s->cache[i];
    return NULL;
}

static av_status cache_add(av_store *s, const coll_dbs *d)
{
    coll_dbs *x = cache_find(s, d->name);
    if (x != NULL) {
        *x = *d;
        return AV_OK;
    }
    if (s->n_cache == s->cap_cache) {
        size_t nc = s->cap_cache ? s->cap_cache * 2 : 8;
        coll_dbs *nb = realloc(s->cache, nc * sizeof *nb);
        if (nb == NULL)
            return AV_ERR;
        s->cache = nb;
        s->cap_cache = nc;
    }
    s->cache[s->n_cache++] = *d;
    return AV_OK;
}

static av_status open_coll_dbs(MDB_txn *txn, const char *name, coll_dbs *d, av_err *e)
{
    memset(d, 0, sizeof *d);
    (void) snprintf(d->name, sizeof d->name, "%s", name);
    for (int i = 0; i < DB_N; i++) {
        char n[NAME_CAP + 16];
        db_name(n, i, name);
        int rc = mdb_dbi_open(txn, n, MDB_CREATE, &d->dbi[i]);
        if (rc != 0)
            return mdb_fail(e, "dbi_open", rc);
    }
    return AV_OK;
}

/* Resolve the DB handles of a collection: txn-pending first, then the shared cache. */
static av_status get_dbs(av_store *s, av_txn *t, const char *name, coll_dbs *out, av_err *e)
{
    if (!name_ok(name, e))
        return AV_ERR;
    if (t != NULL) {
        for (size_t i = 0; i < t->n_drop; i++)
            if (strcmp(t->dropped[i], name) == 0) {
                av_err_set(e, "collection_not_found", "collection '%s' was dropped", name);
                return AV_ERR;
            }
        for (size_t i = 0; i < t->n_pend; i++)
            if (strcmp(t->pend[i].name, name) == 0) {
                *out = t->pend[i];
                return AV_OK;
            }
    }
    (void) pthread_mutex_lock(&s->mu);
    const coll_dbs *d = cache_find(s, name);
    if (d != NULL)
        *out = *d;
    (void) pthread_mutex_unlock(&s->mu);
    if (d == NULL) {
        av_err_set(e, "collection_not_found", "collection '%s' is not in the store", name);
        return AV_ERR;
    }
    return AV_OK;
}

/* ------------------------------------------------------------- open / close */

static av_status mkdir_p(const char *dir, av_err *e)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        av_err_set(e, "internal", "mkdir %s: %s", dir, strerror(errno));
        return AV_ERR;
    }
    return AV_OK;
}

av_status av_store_open(const char *dir, size_t map_size, av_store **out, av_err *e)
{
    if (dir == NULL || out == NULL) {
        av_err_set(e, "internal", "av_store_open: null argument");
        return AV_ERR;
    }
    if (mkdir_p(dir, e) != AV_OK)
        return AV_ERR;
    av_store *s = calloc(1, sizeof *s);
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    (void) pthread_mutex_init(&s->mu, NULL);
    int rc = mdb_env_create(&s->env);
    if (rc != 0) {
        free(s);
        return mdb_fail(e, "env_create", rc);
    }
    MDB_txn *txn = NULL;
    if ((rc = mdb_env_set_maxdbs(s->env, MAX_DBS)) != 0 ||
        (rc = mdb_env_set_mapsize(s->env, map_size ? map_size : AV_STORE_DEFAULT_MAP_SIZE)) != 0 ||
        (rc = mdb_env_open(s->env, dir, MDB_NOTLS, 0644)) != 0 ||
        (rc = mdb_txn_begin(s->env, NULL, 0, &txn)) != 0) {
        (void) mdb_fail(e, "env_open", rc);
        goto fail;
    }
    if ((rc = mdb_dbi_open(txn, "meta", MDB_CREATE, &s->meta)) != 0 ||
        (rc = mdb_dbi_open(txn, "collections", MDB_CREATE, &s->colls)) != 0) {
        (void) mdb_fail(e, "dbi_open", rc);
        goto fail_txn;
    }
    MDB_val k = {strlen("schema_version"), (void *) "schema_version"}, v;
    rc = mdb_get(txn, s->meta, &k, &v);
    if (rc == MDB_NOTFOUND) {
        uint8_t ver[4];
        put_le32(ver, AV_STORE_SCHEMA_VERSION);
        v.mv_size = 4;
        v.mv_data = ver;
        if ((rc = mdb_put(txn, s->meta, &k, &v, 0)) != 0) {
            (void) mdb_fail(e, "put", rc);
            goto fail_txn;
        }
    } else if (rc != 0) {
        (void) mdb_fail(e, "get", rc);
        goto fail_txn;
    } else if (v.mv_size != 4 || get_le32(v.mv_data) != AV_STORE_SCHEMA_VERSION) {
        av_err_set(e, "internal", "store schema version %u is not supported (expected %u)",
                   v.mv_size == 4 ? get_le32(v.mv_data) : 0u, AV_STORE_SCHEMA_VERSION);
        goto fail_txn;
    }
    MDB_cursor *cur;
    if ((rc = mdb_cursor_open(txn, s->colls, &cur)) != 0) {
        (void) mdb_fail(e, "cursor_open", rc);
        goto fail_txn;
    }
    MDB_val ck, cv;
    for (rc = mdb_cursor_get(cur, &ck, &cv, MDB_FIRST); rc == 0;
         rc = mdb_cursor_get(cur, &ck, &cv, MDB_NEXT)) {
        char name[NAME_CAP];
        coll_dbs d;
        if (ck.mv_size == 0 || ck.mv_size >= NAME_CAP) {
            (void) corrupt(e, "collection name length");
            mdb_cursor_close(cur);
            goto fail_txn;
        }
        memcpy(name, ck.mv_data, ck.mv_size);
        name[ck.mv_size] = '\0';
        if (open_coll_dbs(txn, name, &d, e) != AV_OK || cache_add(s, &d) != AV_OK) {
            if (e != NULL && e->code[0] == '\0')
                av_err_set(e, "internal", "out of memory");
            mdb_cursor_close(cur);
            goto fail_txn;
        }
    }
    mdb_cursor_close(cur);
    if (rc != MDB_NOTFOUND) {
        (void) mdb_fail(e, "cursor_get", rc);
        goto fail_txn;
    }
    if ((rc = mdb_txn_commit(txn)) != 0) {
        txn = NULL;
        (void) mdb_fail(e, "commit", rc);
        goto fail;
    }
    *out = s;
    return AV_OK;
fail_txn:
    mdb_txn_abort(txn);
fail:
    mdb_env_close(s->env);
    free(s->cache);
    (void) pthread_mutex_destroy(&s->mu);
    free(s);
    return AV_ERR;
}

void av_store_close(av_store *s)
{
    if (s == NULL)
        return;
    mdb_env_close(s->env);
    free(s->cache);
    (void) pthread_mutex_destroy(&s->mu);
    free(s);
}

/* ---------------------------------------------------------------- write txn */

av_status av_store_begin(av_store *s, av_txn **t, av_err *e)
{
    av_txn *x = calloc(1, sizeof *x);
    if (x == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    int rc = mdb_txn_begin(s->env, NULL, 0, &x->txn);
    if (rc != 0) {
        free(x);
        return mdb_fail(e, "txn_begin", rc);
    }
    x->s = s;
    *t = x;
    return AV_OK;
}

static void txn_free(av_txn *t)
{
    free(t->pend);
    free(t->dropped);
    free(t);
}

av_status av_store_commit(av_txn *t, av_err *e)
{
    av_store *s = t->s;
    int rc = mdb_txn_commit(t->txn);
    if (rc != 0) {
        txn_free(t);
        return mdb_fail(e, "commit", rc);
    }
    av_status st = AV_OK;
    (void) pthread_mutex_lock(&s->mu);
    for (size_t i = 0; i < t->n_drop; i++) {
        for (size_t j = 0; j < s->n_cache; j++)
            if (strcmp(s->cache[j].name, t->dropped[i]) == 0) {
                s->cache[j] = s->cache[--s->n_cache];
                break;
            }
    }
    for (size_t i = 0; i < t->n_pend; i++)
        if (cache_add(s, &t->pend[i]) != AV_OK)
            st = AV_ERR;
    (void) pthread_mutex_unlock(&s->mu);
    txn_free(t);
    if (st != AV_OK)
        av_err_set(e, "internal", "out of memory publishing collection handles");
    return st;
}

void av_store_abort(av_txn *t)
{
    if (t == NULL)
        return;
    mdb_txn_abort(t->txn);
    txn_free(t);
}

/* -------------------------------------------------------------- collections */

av_status av_store_put_collection(av_txn *t, const char *name, const char *json, size_t len, av_err *e)
{
    if (!name_ok(name, e))
        return AV_ERR;
    coll_dbs d;
    if (open_coll_dbs(t->txn, name, &d, e) != AV_OK)
        return AV_ERR;
    bool known = false;
    for (size_t i = 0; i < t->n_pend; i++)
        if (strcmp(t->pend[i].name, name) == 0) {
            t->pend[i] = d;
            known = true;
        }
    if (!known) {
        if (t->n_pend == t->cap_pend) {
            size_t nc = t->cap_pend ? t->cap_pend * 2 : 4;
            coll_dbs *nb = realloc(t->pend, nc * sizeof *nb);
            if (nb == NULL) {
                av_err_set(e, "internal", "out of memory");
                return AV_ERR;
            }
            t->pend = nb;
            t->cap_pend = nc;
        }
        t->pend[t->n_pend++] = d;
    }
    for (size_t i = 0; i < t->n_drop; i++)
        if (strcmp(t->dropped[i], name) == 0)
            memmove(t->dropped[i], t->dropped[i + 1], (t->n_drop - i - 1) * NAME_CAP), t->n_drop--;
    MDB_val k = {strlen(name), (void *) name}, v = {len, (void *) json};
    int rc = mdb_put(t->txn, t->s->colls, &k, &v, 0);
    return rc == 0 ? AV_OK : mdb_fail(e, "put_collection", rc);
}

av_status av_store_del_collection(av_txn *t, const char *name, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, name, &d, e) != AV_OK)
        return AV_ERR;
    for (int i = 0; i < DB_N; i++) {
        int rc = mdb_drop(t->txn, d.dbi[i], 1);
        if (rc != 0)
            return mdb_fail(e, "drop", rc);
    }
    MDB_val k = {strlen(name), (void *) name};
    int rc = mdb_del(t->txn, t->s->colls, &k, NULL);
    if (rc != 0 && rc != MDB_NOTFOUND)
        return mdb_fail(e, "del_collection", rc);
    k.mv_size = strlen("next_docid:") + strlen(name);
    char mk[NAME_CAP + 16];
    (void) snprintf(mk, sizeof mk, "next_docid:%s", name);
    k.mv_data = mk;
    rc = mdb_del(t->txn, t->s->meta, &k, NULL);
    if (rc != 0 && rc != MDB_NOTFOUND)
        return mdb_fail(e, "del_meta", rc);
    for (size_t i = 0; i < t->n_pend; i++)
        if (strcmp(t->pend[i].name, name) == 0) {
            t->pend[i] = t->pend[--t->n_pend];
            break;
        }
    if (t->n_drop == t->cap_drop) {
        size_t nc = t->cap_drop ? t->cap_drop * 2 : 4;
        char (*nb)[NAME_CAP] = realloc(t->dropped, nc * NAME_CAP);
        if (nb == NULL) {
            av_err_set(e, "internal", "out of memory");
            return AV_ERR;
        }
        t->dropped = nb;
        t->cap_drop = nc;
    }
    (void) snprintf(t->dropped[t->n_drop++], NAME_CAP, "%s", name);
    return AV_OK;
}

av_status av_store_list_collections(av_store *s, av_store_coll_cb cb, void *ctx, av_err *e)
{
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    MDB_cursor *cur;
    if ((rc = mdb_cursor_open(txn, s->colls, &cur)) != 0) {
        mdb_txn_abort(txn);
        return mdb_fail(e, "cursor_open", rc);
    }
    MDB_val k, v;
    av_status st = AV_OK;
    for (rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST); rc == 0; rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT)) {
        char name[NAME_CAP];
        if (k.mv_size == 0 || k.mv_size >= NAME_CAP) {
            st = corrupt(e, "collection name length");
            break;
        }
        memcpy(name, k.mv_data, k.mv_size);
        name[k.mv_size] = '\0';
        if (!cb(name, v.mv_data, v.mv_size, ctx))
            break;
    }
    if (st == AV_OK && rc != 0 && rc != MDB_NOTFOUND)
        st = mdb_fail(e, "cursor_get", rc);
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return st;
}

/* ----------------------------------------------------------------- entities */

av_status av_store_put_entity(av_txn *t, const char *coll, const uint8_t key[32], const av_row *row,
                              av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    size_t n;
    uint8_t *buf = row_pack(row, &n);
    if (buf == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    MDB_val k = {32, (void *) key}, v = {n, buf};
    int rc = mdb_put(t->txn, d.dbi[DB_ENT], &k, &v, 0);
    free(buf);
    return rc == 0 ? AV_OK : mdb_fail(e, "put_entity", rc);
}

av_status av_store_del_entity(av_txn *t, const char *coll, const uint8_t key[32], av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_val k = {32, (void *) key};
    int rc = mdb_del(t->txn, d.dbi[DB_ENT], &k, NULL);
    return (rc == 0 || rc == MDB_NOTFOUND) ? AV_OK : mdb_fail(e, "del_entity", rc);
}

static av_status get_entity_in(MDB_txn *txn, MDB_dbi dbi, const uint8_t key[32], av_arena *a, av_row *out,
                               bool *found, av_err *e)
{
    MDB_val k = {32, (void *) key}, v;
    int rc = mdb_get(txn, dbi, &k, &v);
    if (rc == MDB_NOTFOUND) {
        *found = false;
        return AV_OK;
    }
    if (rc != 0)
        return mdb_fail(e, "get_entity", rc);
    if (row_parse(&v, out, e) != AV_OK)
        return AV_ERR;
    uint8_t *copy = av_arena_alloc(a, out->attrs_len ? out->attrs_len : 1, 8);
    if (copy == NULL) {
        av_err_set(e, "internal", "arena exhausted");
        return AV_ERR;
    }
    if (out->attrs_len > 0)
        memcpy(copy, out->attrs_blob, out->attrs_len);
    out->attrs_blob = copy;
    *found = true;
    return AV_OK;
}

av_status av_store_get_entity(av_store *s, const char *coll, const uint8_t key[32], av_arena *a, av_row *out,
                              bool *found, av_err *e)
{
    coll_dbs d;
    if (get_dbs(s, NULL, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    av_status st = get_entity_in(txn, d.dbi[DB_ENT], key, a, out, found, e);
    mdb_txn_abort(txn);
    return st;
}

av_status av_store_txn_get_entity(av_txn *t, const char *coll, const uint8_t key[32], av_arena *a,
                                  av_row *out, bool *found, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    return get_entity_in(t->txn, d.dbi[DB_ENT], key, a, out, found, e);
}

av_status av_store_scan_entities(av_store *s, const char *coll, av_store_row_cb cb, void *ctx, av_err *e)
{
    coll_dbs d;
    if (get_dbs(s, NULL, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    MDB_cursor *cur;
    if ((rc = mdb_cursor_open(txn, d.dbi[DB_ENT], &cur)) != 0) {
        mdb_txn_abort(txn);
        return mdb_fail(e, "cursor_open", rc);
    }
    av_status st = AV_OK;
    MDB_val k, v;
    for (rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST); rc == 0; rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT)) {
        av_row row;
        if (k.mv_size != 32) {
            st = corrupt(e, "entity key length");
            break;
        }
        if (row_parse(&v, &row, e) != AV_OK) {
            st = AV_ERR;
            break;
        }
        if (!cb(k.mv_data, &row, ctx))
            break;
    }
    if (st == AV_OK && rc != 0 && rc != MDB_NOTFOUND)
        st = mdb_fail(e, "cursor_get", rc);
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return st;
}

static av_status count_db(av_store *s, const char *coll, int which, size_t *n, av_err *e)
{
    coll_dbs d;
    if (get_dbs(s, NULL, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    MDB_stat st;
    rc = mdb_stat(txn, d.dbi[which], &st);
    mdb_txn_abort(txn);
    if (rc != 0)
        return mdb_fail(e, "stat", rc);
    *n = st.ms_entries;
    return AV_OK;
}

av_status av_store_count_entities(av_store *s, const char *coll, size_t *n, av_err *e)
{
    return count_db(s, coll, DB_ENT, n, e);
}

av_status av_store_count_quarantine(av_store *s, const char *coll, size_t *n, av_err *e)
{
    return count_db(s, coll, DB_QUAR, n, e);
}

/* ---------------------------------------------------------------------- ids */

av_status av_store_put_id(av_txn *t, const char *coll, const char *id, size_t idlen, const uint8_t key[32],
                          av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    if (idlen == 0 || idlen > AV_ATTR_STR_MAX) {
        av_err_set(e, "invalid_argument", "external id length %zu out of range", idlen);
        return AV_ERR;
    }
    MDB_val k = {idlen, (void *) id}, v = {32, (void *) key};
    int rc = mdb_put(t->txn, d.dbi[DB_IDS], &k, &v, 0);
    return rc == 0 ? AV_OK : mdb_fail(e, "put_id", rc);
}

av_status av_store_del_id(av_txn *t, const char *coll, const char *id, size_t idlen, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_val k = {idlen, (void *) id};
    int rc = mdb_del(t->txn, d.dbi[DB_IDS], &k, NULL);
    return (rc == 0 || rc == MDB_NOTFOUND) ? AV_OK : mdb_fail(e, "del_id", rc);
}

static av_status get_id_in(MDB_txn *txn, MDB_dbi dbi, const char *id, size_t idlen, uint8_t key[32],
                           bool *found, av_err *e)
{
    MDB_val k = {idlen, (void *) id}, v;
    int rc = mdb_get(txn, dbi, &k, &v);
    if (rc == MDB_NOTFOUND) {
        *found = false;
        return AV_OK;
    }
    if (rc != 0)
        return mdb_fail(e, "get_id", rc);
    if (v.mv_size != 32)
        return corrupt(e, "id map value length");
    memcpy(key, v.mv_data, 32);
    *found = true;
    return AV_OK;
}

av_status av_store_get_id(av_store *s, const char *coll, const char *id, size_t idlen, uint8_t key[32],
                          bool *found, av_err *e)
{
    coll_dbs d;
    if (get_dbs(s, NULL, coll, &d, e) != AV_OK)
        return AV_ERR;
    if (idlen == 0 || idlen > 511) {
        *found = false;
        return AV_OK;
    }
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    av_status st = get_id_in(txn, d.dbi[DB_IDS], id, idlen, key, found, e);
    mdb_txn_abort(txn);
    return st;
}

av_status av_store_txn_get_id(av_txn *t, const char *coll, const char *id, size_t idlen, uint8_t key[32],
                              bool *found, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    if (idlen == 0 || idlen > 511) {
        *found = false;
        return AV_OK;
    }
    return get_id_in(t->txn, d.dbi[DB_IDS], id, idlen, key, found, e);
}

/* ------------------------------------------------------------ cursor / ring */

av_status av_store_put_cursor(av_txn *t, const char *coll, const av_cursor *c, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    uint8_t buf[40];
    put_le64(buf, c->block);
    memcpy(buf + 8, c->hash, 32);
    MDB_val k = {1, (void *) "c"}, v = {sizeof buf, buf};
    int rc = mdb_put(t->txn, d.dbi[DB_CUR], &k, &v, 0);
    return rc == 0 ? AV_OK : mdb_fail(e, "put_cursor", rc);
}

av_status av_store_get_cursor(av_store *s, const char *coll, av_cursor *c, bool *found, av_err *e)
{
    coll_dbs d;
    if (get_dbs(s, NULL, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    MDB_val k = {1, (void *) "c"}, v;
    rc = mdb_get(txn, d.dbi[DB_CUR], &k, &v);
    av_status st = AV_OK;
    if (rc == MDB_NOTFOUND) {
        *found = false;
    } else if (rc != 0) {
        st = mdb_fail(e, "get_cursor", rc);
    } else if (v.mv_size != 40) {
        st = corrupt(e, "cursor value length");
    } else {
        c->block = get_le64(v.mv_data);
        memcpy(c->hash, (const uint8_t *) v.mv_data + 8, 32);
        *found = true;
    }
    mdb_txn_abort(txn);
    return st;
}

av_status av_store_ring_put(av_txn *t, const char *coll, uint64_t block, const uint8_t hash[32],
                            const uint8_t (*keys)[32], size_t n_keys, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    if (n_keys > (SIZE_MAX - 36) / 32 || n_keys > UINT32_MAX) {
        av_err_set(e, "internal", "ring entry too large");
        return AV_ERR;
    }
    size_t n = 36 + n_keys * 32;
    uint8_t *buf = malloc(n);
    if (buf == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    memcpy(buf, hash, 32);
    put_le32(buf + 32, (uint32_t) n_keys);
    if (n_keys > 0)
        memcpy(buf + 36, keys, n_keys * 32);
    uint8_t kb[8];
    put_be64(kb, block);
    MDB_val k = {8, kb}, v = {n, buf};
    int rc = mdb_put(t->txn, d.dbi[DB_RING], &k, &v, 0);
    free(buf);
    return rc == 0 ? AV_OK : mdb_fail(e, "ring_put", rc);
}

av_status av_store_ring_get(av_store *s, const char *coll, uint64_t block, uint8_t hash[32], av_arena *a,
                            uint8_t (**keys)[32], size_t *n_keys, bool *found, av_err *e)
{
    coll_dbs d;
    if (get_dbs(s, NULL, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    uint8_t kb[8];
    put_be64(kb, block);
    MDB_val k = {8, kb}, v;
    rc = mdb_get(txn, d.dbi[DB_RING], &k, &v);
    av_status st = AV_OK;
    if (rc == MDB_NOTFOUND) {
        *found = false;
    } else if (rc != 0) {
        st = mdb_fail(e, "ring_get", rc);
    } else {
        const uint8_t *b = v.mv_data;
        size_t n = v.mv_size >= 36 ? get_le32(b + 32) : 0;
        if (v.mv_size < 36 || n != (v.mv_size - 36) / 32 || (v.mv_size - 36) % 32 != 0) {
            st = corrupt(e, "ring entry length");
        } else {
            uint8_t (*ks)[32] = av_arena_alloc(a, n ? n * 32 : 1, 8);
            if (ks == NULL) {
                av_err_set(e, "internal", "arena exhausted");
                st = AV_ERR;
            } else {
                memcpy(hash, b, 32);
                if (n > 0)
                    memcpy(ks, b + 36, n * 32);
                *keys = ks;
                *n_keys = n;
                *found = true;
            }
        }
    }
    mdb_txn_abort(txn);
    return st;
}

av_status av_store_ring_list(av_store *s, const char *coll, av_arena *a, av_ring_entry **out, size_t *n_out,
                             av_err *e)
{
    coll_dbs d;
    if (get_dbs(s, NULL, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    MDB_stat stt;
    rc = mdb_stat(txn, d.dbi[DB_RING], &stt);
    if (rc != 0) {
        mdb_txn_abort(txn);
        return mdb_fail(e, "stat", rc);
    }
    if (stt.ms_entries == 0) {
        mdb_txn_abort(txn);
        *out = NULL;
        *n_out = 0;
        return AV_OK;
    }
    av_ring_entry *entries =
        av_arena_alloc(a, sizeof(av_ring_entry) * stt.ms_entries, _Alignof(av_ring_entry));
    if (entries == NULL) {
        mdb_txn_abort(txn);
        av_err_set(e, "internal", "arena exhausted");
        return AV_ERR;
    }
    MDB_cursor *cur;
    rc = mdb_cursor_open(txn, d.dbi[DB_RING], &cur);
    if (rc != 0) {
        mdb_txn_abort(txn);
        return mdb_fail(e, "cursor_open", rc);
    }
    MDB_val k, v;
    size_t count = 0;
    rc = mdb_cursor_get(cur, &k, &v, MDB_LAST);
    while (rc == 0 && count < stt.ms_entries) {
        if (k.mv_size != 8 || v.mv_size < 36) {
            mdb_cursor_close(cur);
            mdb_txn_abort(txn);
            return corrupt(e, "ring entry format");
        }
        uint64_t blk = get_be64(k.mv_data);
        const uint8_t *b = v.mv_data;
        size_t nk = get_le32(b + 32);
        if (v.mv_size != 36 + nk * 32) {
            mdb_cursor_close(cur);
            mdb_txn_abort(txn);
            return corrupt(e, "ring entry key length");
        }
        entries[count].block = blk;
        memcpy(entries[count].hash, b, 32);
        entries[count].n_keys = nk;
        if (nk > 0) {
            uint8_t (*ks)[32] = av_arena_alloc(a, nk * 32, 8);
            if (ks == NULL) {
                mdb_cursor_close(cur);
                mdb_txn_abort(txn);
                av_err_set(e, "internal", "arena exhausted");
                return AV_ERR;
            }
            memcpy(ks, b + 36, nk * 32);
            entries[count].keys = ks;
        } else {
            entries[count].keys = NULL;
        }
        count++;
        rc = mdb_cursor_get(cur, &k, &v, MDB_PREV);
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    *out = entries;
    *n_out = count;
    return AV_OK;
}

av_status av_store_ring_trim(av_txn *t, const char *coll, size_t keep_last, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_stat stt;
    int rc = mdb_stat(t->txn, d.dbi[DB_RING], &stt);
    if (rc != 0)
        return mdb_fail(e, "stat", rc);
    if (stt.ms_entries <= keep_last)
        return AV_OK;
    size_t drop = stt.ms_entries - keep_last;
    MDB_cursor *cur;
    if ((rc = mdb_cursor_open(t->txn, d.dbi[DB_RING], &cur)) != 0)
        return mdb_fail(e, "cursor_open", rc);
    MDB_val k, v;
    for (size_t i = 0; i < drop; i++) {
        rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
        if (rc == 0)
            rc = mdb_cursor_del(cur, 0);
        if (rc != 0) {
            mdb_cursor_close(cur);
            return mdb_fail(e, "ring_trim", rc);
        }
    }
    mdb_cursor_close(cur);
    return AV_OK;
}

av_status av_store_ring_drop_above(av_txn *t, const char *coll, uint64_t from_block, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    if (from_block == UINT64_MAX)
        return AV_OK;
    MDB_cursor *cur;
    int rc = mdb_cursor_open(t->txn, d.dbi[DB_RING], &cur);
    if (rc != 0)
        return mdb_fail(e, "cursor_open", rc);
    uint8_t kb[8];
    put_be64(kb, from_block + 1);
    for (;;) {
        MDB_val k = {8, kb}, v;
        rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
        if (rc == MDB_NOTFOUND)
            break;
        if (rc == 0)
            rc = mdb_cursor_del(cur, 0);
        if (rc != 0) {
            mdb_cursor_close(cur);
            return mdb_fail(e, "ring_drop", rc);
        }
    }
    mdb_cursor_close(cur);
    return AV_OK;
}

/* --------------------------------------------------------------- quarantine */

av_status av_store_quarantine_put(av_txn *t, const char *coll, const uint8_t key[32], uint64_t updated_at,
                                  const char *reason, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    size_t rl = strlen(reason);
    if (rl > 256)
        rl = 256;
    uint8_t buf[8 + 256];
    put_le64(buf, updated_at);
    /* Stored as length-delimited bytes (value length = 8 + rl), not as a C string. */
    memcpy(buf + 8, reason, rl); // NOLINT(bugprone-not-null-terminated-result)
    MDB_val k = {32, (void *) key}, v = {8 + rl, buf};
    int rc = mdb_put(t->txn, d.dbi[DB_QUAR], &k, &v, 0);
    return rc == 0 ? AV_OK : mdb_fail(e, "quarantine", rc);
}

av_status av_store_quarantine(av_txn *t, const char *coll, const uint8_t key[32], const char *reason,
                              av_err *e)
{
    return av_store_quarantine_put(t, coll, key, 0, reason, e);
}

av_status av_store_unquarantine(av_txn *t, const char *coll, const uint8_t key[32], av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_val k = {32, (void *) key};
    int rc = mdb_del(t->txn, d.dbi[DB_QUAR], &k, NULL);
    return (rc == 0 || rc == MDB_NOTFOUND) ? AV_OK : mdb_fail(e, "unquarantine", rc);
}

av_status av_store_txn_get_quarantine(av_txn *t, const char *coll, const uint8_t key[32],
                                      uint64_t *updated_at, bool *found, av_err *e)
{
    coll_dbs d;
    if (get_dbs(t->s, t, coll, &d, e) != AV_OK)
        return AV_ERR;
    MDB_val k = {32, (void *) key}, v;
    int rc = mdb_get(t->txn, d.dbi[DB_QUAR], &k, &v);
    if (rc == MDB_NOTFOUND) {
        *found = false;
        return AV_OK;
    }
    if (rc != 0)
        return mdb_fail(e, "get_quarantine", rc);
    if (v.mv_size < 8)
        return corrupt(e, "quarantine value length");
    *updated_at = get_le64(v.mv_data);
    *found = true;
    return AV_OK;
}

/* ------------------------------------------------------------------- docids */

static void next_docid_key(char out[NAME_CAP + 16], const char *coll)
{
    (void) snprintf(out, NAME_CAP + 16, "next_docid:%s", coll);
}

av_status av_store_put_next_docid(av_txn *t, const char *coll, uint32_t next, av_err *e)
{
    if (!name_ok(coll, e))
        return AV_ERR;
    char kb[NAME_CAP + 16];
    next_docid_key(kb, coll);
    uint8_t vb[4];
    put_le32(vb, next);
    MDB_val k = {strlen(kb), kb}, v = {4, vb};
    int rc = mdb_put(t->txn, t->s->meta, &k, &v, 0);
    return rc == 0 ? AV_OK : mdb_fail(e, "put_next_docid", rc);
}

av_status av_store_get_next_docid(av_store *s, const char *coll, uint32_t *next, bool *found, av_err *e)
{
    if (!name_ok(coll, e))
        return AV_ERR;
    MDB_txn *txn;
    int rc = mdb_txn_begin(s->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return mdb_fail(e, "txn_begin", rc);
    char kb[NAME_CAP + 16];
    next_docid_key(kb, coll);
    MDB_val k = {strlen(kb), kb}, v;
    rc = mdb_get(txn, s->meta, &k, &v);
    av_status st = AV_OK;
    if (rc == MDB_NOTFOUND) {
        *found = false;
    } else if (rc != 0) {
        st = mdb_fail(e, "get_next_docid", rc);
    } else if (v.mv_size != 4) {
        st = corrupt(e, "next_docid length");
    } else {
        *next = get_le32(v.mv_data);
        *found = true;
    }
    mdb_txn_abort(txn);
    return st;
}
