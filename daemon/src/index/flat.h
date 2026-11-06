/*
 * index/flat.h: exact flat scan with dispatched pgvector kernels (D3).
 */
#ifndef AV_INDEX_FLAT_H
#define AV_INDEX_FLAT_H

#include <roaring.h>
#include <stddef.h>
#include <stdint.h>

#include "index/docid.h"
#include "index/segment.h"
#include "kernels/opclass.h"
#include "log.h"

/*
 * Scan segment for query vector using opclass, constrained to cand bitmap.
 * docmap is used to break ties by ascending key.
 * Finds the top (offset + k) nearest neighbors and returns the k elements starting at offset.
 * out must hold at least k elements.
 */
av_status av_flat_scan(const av_segment *s, const av_docmap *docmap, const void *query,
                       const av_opclass *oc, const roaring_bitmap_t *cand,
                       size_t offset, size_t k, av_hit *out, size_t *n_out, av_err *e);

#endif /* AV_INDEX_FLAT_H */
