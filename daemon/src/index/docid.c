/*
 * index/docid.c: see docid.h.
 */
#include "index/docid.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t key[32];
    uint64_t updated_at;
    uint64_t expires_at;
    bool present;
} dense_ent;

typedef struct {
    av_docid id;
    bool used;
    uint8_t key[32];
} slot;

struct av_docmap {
    dense_ent *dense; /* indexed by docid */
    size_t dense_cap;
    av_docid high;
    slot *slots; /* open addressing, tombstone-free: deletions rehash the cluster */
    size_t slot_cap;
    size_t count;
};

static uint64_t key_hash(const uint8_t key[32])
{
    /* Keys are keccak outputs; mixing 8 bytes is enough, FNV over all 32 stays cheap. */
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 32; i++) {
        h ^= key[i];
        h *= 1099511628211ull;
    }
    return h ^ (h >> 29);
}

av_docmap *av_docmap_create(void)
{
    av_docmap *m = calloc(1, sizeof *m);
    if (m == NULL)
        return NULL;
    m->slot_cap = 64;
    m->slots = calloc(m->slot_cap, sizeof(slot));
    if (m->slots == NULL) {
        free(m);
        return NULL;
    }
    return m;
}

void av_docmap_destroy(av_docmap *m)
{
    if (m == NULL)
        return;
    free(m->dense);
    free(m->slots);
    free(m);
}

static slot *find_slot(const av_docmap *m, const uint8_t key[32])
{
    size_t mask = m->slot_cap - 1;
    for (size_t i = key_hash(key) & mask;; i = (i + 1) & mask) {
        slot *s = &m->slots[i];
        if (!s->used || memcmp(s->key, key, 32) == 0)
            return s;
    }
}

static bool grow_slots(av_docmap *m)
{
    size_t ncap = m->slot_cap * 2;
    slot *ns = calloc(ncap, sizeof(slot));
    if (ns == NULL)
        return false;
    slot *old = m->slots;
    size_t ocap = m->slot_cap;
    m->slots = ns;
    m->slot_cap = ncap;
    for (size_t i = 0; i < ocap; i++)
        if (old[i].used)
            *find_slot(m, old[i].key) = old[i];
    free(old);
    return true;
}

static bool ensure_dense(av_docmap *m, av_docid id)
{
    if ((size_t) id < m->dense_cap)
        return true;
    size_t ncap = m->dense_cap ? m->dense_cap : 64;
    while (ncap <= (size_t) id)
        ncap *= 2;
    dense_ent *nd = realloc(m->dense, ncap * sizeof *nd);
    if (nd == NULL)
        return false;
    memset(nd + m->dense_cap, 0, (ncap - m->dense_cap) * sizeof *nd);
    m->dense = nd;
    m->dense_cap = ncap;
    return true;
}

av_status av_docmap_assign(av_docmap *m, const uint8_t key[32], av_docid id, uint64_t updated_at,
                           uint64_t expires_at, av_err *e)
{
    if (m->count * 2 >= m->slot_cap && !grow_slots(m)) {
        av_err_set(e, "internal", "out of memory growing docid map");
        return AV_ERR;
    }
    if (!ensure_dense(m, id)) {
        av_err_set(e, "internal", "out of memory growing docid table");
        return AV_ERR;
    }
    slot *s = find_slot(m, key);
    if (!s->used) {
        s->used = true;
        memcpy(s->key, key, 32);
        m->count++;
    }
    s->id = id;
    dense_ent *d = &m->dense[id];
    memcpy(d->key, key, 32);
    d->updated_at = updated_at;
    d->expires_at = expires_at;
    d->present = true;
    if (id >= m->high)
        m->high = id + 1;
    return AV_OK;
}

void av_docmap_unmap_key(av_docmap *m, const uint8_t key[32])
{
    size_t mask = m->slot_cap - 1;
    slot *s = find_slot(m, key);
    if (!s->used)
        return;
    /* Backward-shift deletion keeps probe sequences intact without tombstones. */
    size_t i = (size_t) (s - m->slots);
    size_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!m->slots[j].used)
            break;
        size_t home = key_hash(m->slots[j].key) & mask;
        bool movable = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
        if (movable) {
            m->slots[i] = m->slots[j];
            i = j;
        }
    }
    memset(&m->slots[i], 0, sizeof(slot));
    m->count--;
}

void av_docmap_set_meta(av_docmap *m, av_docid id, uint64_t updated_at, uint64_t expires_at)
{
    if ((size_t) id < m->dense_cap && m->dense[id].present) {
        m->dense[id].updated_at = updated_at;
        m->dense[id].expires_at = expires_at;
    }
}

bool av_docmap_lookup(const av_docmap *m, const uint8_t key[32], av_docid *id)
{
    const slot *s = find_slot(m, key);
    if (!s->used)
        return false;
    *id = s->id;
    return true;
}

const uint8_t *av_docmap_key(const av_docmap *m, av_docid id)
{
    if ((size_t) id >= m->dense_cap || !m->dense[id].present)
        return NULL;
    return m->dense[id].key;
}

bool av_docmap_meta(const av_docmap *m, av_docid id, uint64_t *updated_at, uint64_t *expires_at)
{
    if ((size_t) id >= m->dense_cap || !m->dense[id].present)
        return false;
    *updated_at = m->dense[id].updated_at;
    *expires_at = m->dense[id].expires_at;
    return true;
}

size_t av_docmap_size(const av_docmap *m)
{
    return m->count;
}

av_docid av_docmap_high(const av_docmap *m)
{
    return m->high;
}
