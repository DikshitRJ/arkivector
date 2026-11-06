/*
 * filter/eval.h: AST -> Roaring bitmap evaluator over attribute postings index (D3).
 */
#ifndef AV_FILTER_EVAL_H
#define AV_FILTER_EVAL_H

#include <roaring.h>
#include <stdint.h>

#include "filter/ast.h"
#include "index/attr_index.h"
#include "index/docid.h"
#include "log.h"

/*
 * Evaluates the filter f against the attribute index and docmap, intersecting with
 * the live bitmap and pruning expired entities (expires_at_block <= as_of_block).
 *
 * If f is NULL or f->kind == AV_F_TRUE, returns all live, unexpired docids.
 *
 * Returns a newly allocated roaring_bitmap_t, which the caller must free with
 * roaring_bitmap_free(). Returns NULL on out of memory or error.
 */
roaring_bitmap_t *av_filter_eval(const av_filter *f, const av_attr_index *ix, const av_docmap *docmap,
                                 const roaring_bitmap_t *live, uint64_t as_of_block, av_err *e);

#endif /* AV_FILTER_EVAL_H */
