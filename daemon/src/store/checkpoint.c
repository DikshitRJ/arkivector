/*
 * store/checkpoint.c: see checkpoint.h.
 */
#include "store/checkpoint.h"

#include <string.h>

#include "store/schema.h"

typedef struct {
    av_arena *arena;
    av_checkpoint_row_cb cb;
    void *ctx;
    av_err *err;
    av_status st;
} load_ctx;

static bool row_trampoline(const uint8_t key[32], const av_row *row, void *p)
{
    load_ctx *c = p;
    av_arena_reset(c->arena);
    av_loaded_row lr;
    memset(&lr, 0, sizeof lr);
    lr.key = key;
    lr.row = *row;
    if (av_attrs_unpack(row->attrs_blob, row->attrs_len, c->arena, &lr.attrs, &lr.n_attrs, c->err) != AV_OK) {
        c->st = AV_ERR;
        return false;
    }
    for (size_t i = 0; i < lr.n_attrs; i++)
        if (strcmp(lr.attrs[i].name, AV_ATTR_VEC) == 0) {
            lr.vec = lr.attrs[i].val;
            lr.vec_len = lr.attrs[i].len;
        }
    return c->cb(&lr, c->ctx);
}

av_status av_checkpoint_load_rows(av_store *s, const char *coll, av_checkpoint_row_cb cb, void *ctx, av_err *e)
{
    load_ctx c = {av_arena_create("checkpoint", (size_t) 1 << 20, 0), cb, ctx, e, AV_OK};
    if (c.arena == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    av_status st = av_store_scan_entities(s, coll, row_trampoline, &c, e);
    av_arena_destroy(c.arena);
    return st != AV_OK ? st : c.st;
}

av_status av_checkpoint_load_meta(av_store *s, const char *coll, av_checkpoint_meta *out, av_err *e)
{
    memset(out, 0, sizeof *out);
    if (av_store_get_cursor(s, coll, &out->cursor, &out->has_cursor, e) != AV_OK)
        return AV_ERR;
    return av_store_get_next_docid(s, coll, &out->next_docid, &out->has_next_docid, e);
}
