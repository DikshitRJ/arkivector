/*
 * query/executor.c: query execution and hit formatting.
 */
#include "query/executor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec/base64.h"
#include "codec/hex.h"
#include "filter/value.h"
#include "filter/value_json.h"
#include "index/flat.h"
#include "store/schema.h"

static double current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

av_status av_execute_search(av_collection *col, av_store *store, const av_search_params *params,
                            const roaring_bitmap_t *cand, av_search_result *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    double t0 = current_time_ms();

    const av_coldef *def = av_collection_def(col);
    const av_segment *seg = av_collection_segment(col);
    const av_docmap *docmap = av_collection_docmap(col);

    size_t k = params->k > 0 ? params->k : 10;
    size_t offset = params->offset;

    av_hit *raw_hits = malloc(k * sizeof(av_hit));
    if (raw_hits == NULL) {
        av_err_set(e, "internal", "out of memory for raw hits");
        return AV_ERR;
    }

    av_query_plan plan;
    (void) av_plan_query(col, params->query_vec, params->oc, params->exact, cand,
                         av_collection_live_count(col), &plan, e);

    size_t n_raw = 0;
    bool hnsw_done = false;
    if ((plan.strategy == AV_STRAT_HNSW || plan.strategy == AV_STRAT_HNSW_ITERATIVE) &&
        av_collection_has_hnsw(col)) {
        size_t limit = offset + k;
        int ef_search = params->ef_search > 0 ? params->ef_search : 40;
        if (plan.strategy == AV_STRAT_HNSW_ITERATIVE && plan.selectivity > 0.0) {
            int bumped = (int) ceil(1.5 * (double) limit / plan.selectivity);
            if (bumped > ef_search)
                ef_search = bumped;
        }
        if (ef_search < (int) limit)
            ef_search = (int) limit;
        if (ef_search > 1000)
            ef_search = 1000;
        int iter_mode = 1; /* relaxed_order */
        if (params->iterative_scan != NULL) {
            if (strcmp(params->iterative_scan, "off") == 0)
                iter_mode = 0;
            else if (strcmp(params->iterative_scan, "strict_order") == 0)
                iter_mode = 2;
        }
        av_hit *buf = malloc(limit * sizeof *buf);
        size_t nb = 0;
        av_err he;
        av_err_clear(&he);
        if (buf != NULL &&
            av_collection_search_hnsw(col, params->query_vec, params->oc, cand, ef_search, iter_mode,
                                      (int) params->max_scan_tuples, limit, buf, &nb, &he) == AV_OK) {
            size_t avail =
                cand != NULL ? (size_t) roaring_bitmap_get_cardinality(cand) : av_collection_live_count(col);
            /* S2 -> S1 fallback: the graph walk found fewer than offset + k of the candidates */
            if (nb >= limit || nb >= avail) {
                if (nb > offset) {
                    n_raw = nb - offset < k ? nb - offset : k;
                    memcpy(raw_hits, buf + offset, n_raw * sizeof *raw_hits);
                }
                hnsw_done = true;
            }
        }
        free(buf);
        if (!hnsw_done) {
            plan.strategy = AV_STRAT_EXACT_FLAT;
            plan.exact = true;
        }
    }
    if (!hnsw_done) {
        /* Without a filter, score the live docids only (the segment also holds replaced versions). */
        const roaring_bitmap_t *scan = cand != NULL ? cand : av_collection_live(col);
        if (av_flat_scan(seg, docmap, params->query_vec, params->oc, scan, offset, k, raw_hits, &n_raw, e) !=
            AV_OK) {
            free(raw_hits);
            return AV_ERR;
        }
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        free(raw_hits);
        av_err_set(e, "internal", "out of memory for json doc");
        return AV_ERR;
    }

    out->doc = doc;
    out->hits = calloc(n_raw > 0 ? n_raw : 1, sizeof(av_search_hit));
    if (out->hits == NULL && n_raw > 0) {
        free(raw_hits);
        yyjson_mut_doc_free(doc);
        av_err_set(e, "internal", "out of memory for hits");
        return AV_ERR;
    }

    av_arena *row_arena = av_arena_create("search_row", 64 * 1024, 0);
    size_t retained = 0;

    for (size_t i = 0; i < n_raw; i++) {
        av_hit h = raw_hits[i];
        double distance = h.distance;
        double score =
            (params->oc != NULL && strcmp(params->oc->op, "<=>") == 0) ? (1.0 - distance) : (-distance);

        if (isnan(distance))
            continue; /* zero-norm cosine operands: JSON cannot carry NaN */
        if (params->has_max_distance && distance > params->max_distance)
            continue;
        if (params->has_min_score && score < params->min_score)
            continue;

        const uint8_t *key = av_docmap_key(docmap, h.id);
        if (key == NULL)
            continue;

        av_search_hit *dst = &out->hits[retained];
        memcpy(dst->key, key, 32);
        dst->distance = distance;
        dst->score = score;

        /* Look up row in store if store is provided */
        if (store != NULL) {
            av_row row;
            bool found = false;
            av_arena_reset(row_arena);
            if (av_store_get_entity(store, def->name, key, row_arena, &row, &found, e) == AV_OK && found) {
                memcpy(dst->owner, row.owner, 20);
                memcpy(dst->creator, row.creator, 20);
                dst->created_at = row.created_at;
                dst->updated_at = row.updated_at;
                dst->expires_at = row.expires_at;

                av_attr *attrs = NULL;
                size_t n_attrs = 0;
                if (av_attrs_unpack(row.attrs_blob, row.attrs_len, row_arena, &attrs, &n_attrs, e) == AV_OK) {
                    yyjson_mut_val *attrs_obj = yyjson_mut_obj(doc);
                    for (size_t a = 0; a < n_attrs; a++) {
                        const av_attr *at = &attrs[a];
                        if (strcmp(at->name, AV_ATTR_ID) == 0) {
                            size_t copy_len = at->len < 128 ? at->len : 128;
                            memcpy(dst->id, at->val, copy_len);
                            dst->id[copy_len] = '\0';
                            dst->has_id = true;
                        } else if (strcmp(at->name, AV_ATTR_DOC) == 0 &&
                                   (params->include_flags & AV_INCLUDE_DOCUMENT)) {
                            dst->document = malloc(at->len + 1);
                            if (dst->document != NULL) {
                                memcpy(dst->document, at->val, at->len);
                                dst->document[at->len] = '\0';
                                dst->doc_len = at->len;
                            }
                        } else if (strcmp(at->name, AV_ATTR_VEC) == 0 &&
                                   (params->include_flags & AV_INCLUDE_VECTOR)) {
                            char *b64 = NULL;
                            size_t b64_len = 0;
                            av_err err;
                            av_err_clear(&err);
                            if (av_base64_encode(at->val, at->len, row_arena, &b64, &b64_len, &err) ==
                                AV_OK) {
                                dst->vector_bytes = strdup(b64);
                            }
                        } else if (strncmp(at->name, "av.", 3) != 0 &&
                                   strcmp(at->name, AV_ATTR_SCHEMA) != 0) {
                            if (params->include_flags & AV_INCLUDE_ATTRIBUTES) {
                                yyjson_mut_val *tv = av_attr_to_typed_json(doc, at, row_arena);
                                /* yyjson does not copy keys added with obj_add_val: copy it, the source may be short-lived */
                                {
                                    yyjson_mut_val *k_ = yyjson_mut_strcpy(doc, at->name);
                                    if (k_ != NULL && tv != NULL)
                                        yyjson_mut_obj_add(attrs_obj, k_, tv);
                                }
                            }
                        }
                    }
                    if (params->include_flags & AV_INCLUDE_ATTRIBUTES) {
                        dst->attributes_obj = attrs_obj;
                    }
                }
            }
        }
        retained++;
    }

    av_arena_destroy(row_arena);
    free(raw_hits);

    out->n_hits = retained;
    out->as_of_block = av_collection_as_of(col);
    out->sync_state = (out->as_of_block == 0 ? "bootstrap" : "live");
    out->open_collection = (def->membership == AV_MEMBERSHIP_OPEN);
    out->explain = params->explain;

    double t1 = current_time_ms();
    out->plan = plan;
    out->plan.elapsed_ms = t1 - t0;

    return AV_OK;
}

void av_search_result_free(av_search_result *res)
{
    if (res == NULL)
        return;
    if (res->hits != NULL) {
        for (size_t i = 0; i < res->n_hits; i++) {
            free(res->hits[i].document);
            free(res->hits[i].vector_bytes);
        }
        free(res->hits);
        res->hits = NULL;
    }
    if (res->doc != NULL) {
        yyjson_mut_doc_free(res->doc);
        res->doc = NULL;
    }
}
