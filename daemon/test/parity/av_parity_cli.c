/*
 * av_parity_cli: test helper for the pgvector parity suite (test/parity).
 * Reads JSON lines on stdin, writes one JSON line per input on stdout.
 *   {"op":"<->","type":"vector","a":"[1,2]","b":"[3,4]"}  -> {"ok":true,"d":"2.8284271247461903"}
 *   {"op":"in","type":"sparsevec","a":"{1:1.5}/3"}         -> {"ok":true,"text":"{1:1.5}/3"}
 * Failures: {"ok":false,"error":"<pgvector message>"}
 * The distance is printed with %.17g (as a string, so nan/inf survive JSON).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "codec/text.h"
#include "kernels/opclass.h"
#include "yyjson.h"

static void emit(bool ok, const char *key, const char *val)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *r = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, r);
    yyjson_mut_obj_add_bool(d, r, "ok", ok);
    yyjson_mut_obj_add_strcpy(d, r, key, val);
    char *s = yyjson_mut_write(d, 0, NULL);
    puts(s);
    free(s);
    yyjson_mut_doc_free(d);
}

static void handle(const char *line, av_arena *ar)
{
    yyjson_doc *doc = yyjson_read(line, strlen(line), 0);
    if (doc == NULL) {
        emit(false, "error", "av_parity_cli: bad json input");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *op = yyjson_get_str(yyjson_obj_get(root, "op"));
    const char *tn = yyjson_get_str(yyjson_obj_get(root, "type"));
    const char *sa = yyjson_get_str(yyjson_obj_get(root, "a"));
    const char *sb = yyjson_get_str(yyjson_obj_get(root, "b"));
    av_vtype t;
    av_err e;
    av_err_clear(&e);
    if (op == NULL || tn == NULL || sa == NULL || !av_vtype_parse(tn, &t)) {
        emit(false, "error", "av_parity_cli: missing/invalid op, type or a");
        yyjson_doc_free(doc);
        return;
    }
    void *va = NULL;
    if (av_text_to_pgvec(t, sa, -1, ar, &va, &e) != AV_OK) {
        emit(false, "error", e.msg);
        yyjson_doc_free(doc);
        return;
    }
    if (strcmp(op, "in") == 0) {
        char *txt = NULL;
        if (av_pgvec_to_text(t, va, ar, &txt, &e) != AV_OK)
            emit(false, "error", e.msg);
        else
            emit(true, "text", txt);
        yyjson_doc_free(doc);
        return;
    }
    void *vb = NULL;
    const av_opclass *oc = av_opclass_for_operator(t, op);
    if (oc == NULL || sb == NULL) {
        emit(false, "error", "av_parity_cli: unknown operator or missing b");
        yyjson_doc_free(doc);
        return;
    }
    if (av_text_to_pgvec(t, sb, -1, ar, &vb, &e) != AV_OK) {
        emit(false, "error", e.msg);
        yyjson_doc_free(doc);
        return;
    }
    char out[64];
    char errmsg[512];
    volatile int failed = 0;
    MemoryContext ctx = AvContextCreate(NULL, "parity_dist", 8192, 64u * 1024 * 1024);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    PG_TRY();
    {
        /* Call pgvector's own function (not oc->distance, which returns NaN on a dimension
         * mismatch instead of raising) so CheckDims errors match PostgreSQL verbatim. */
        PGFunction fn = (PGFunction) av_kernel_fn(t, oc->metric);
        double dist = DatumGetFloat8(DirectFunctionCall2(fn, PointerGetDatum(va), PointerGetDatum(vb)));
        snprintf(out, sizeof out, "%.17g", dist);
    }
    PG_CATCH();
    {
        snprintf(errmsg, sizeof errmsg, "%s", av_pg_error_data()->message);
        FlushErrorState();
        failed = 1;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    if (failed)
        emit(false, "error", errmsg);
    else
        emit(true, "d", out);
    yyjson_doc_free(doc);
}

int main(void)
{
    av_kernels_init();
    av_arena *ar = av_arena_create("parity", 1u << 20, 0);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (line[0] == '\0')
            continue;
        av_arena_reset(ar);
        handle(line, ar);
        fflush(stdout);
    }
    free(line);
    av_arena_destroy(ar);
    return 0;
}
