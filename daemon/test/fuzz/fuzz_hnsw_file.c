/*
 * fuzz_hnsw_file.c: libFuzzer target for the HNSW band file loader (av_hnsw_file_decode ->
 * av_segment_hnsw_restore), which reads untrusted bytes from disk at startup (issue #11).
 * Byte 0 bit 0 enables the CRC checks (off by default, so mutations reach the structural
 * validator); the rest is the file. Expected parameters: vector(3), vector_l2_ops, m 4,
 * ef_construction 16, band 7 of 100-block bands, committed docid bound 1000 (the unit test
 * hnsw_file_round_trip writes a matching seed with AV_WRITE_HNSW_SEED). An accepted graph is
 * searched and extended, which must not crash either.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "index/hnsw_index.h"
#include "kernels/opclass.h"
#include "postgres.h"
#include "varatt.h"
#include "vector.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 4 * 1024 * 1024)
        return 0;
    bool crc = (data[0] & 1) != 0;
    av_hnsw_file_expect ex = {AV_VT_VECTOR, 3, 4, 16, "vector_l2_ops", 7, 100, 1000};
    const av_opclass *oc = av_opclass_find("vector_l2_ops");
    av_segment *g = NULL;
    av_hnsw_file_info fi;
    av_err e;
    av_err_clear(&e);
    if (av_hnsw_file_decode(data + 1, size - 1, &ex, oc, crc, (size_t) 1 << 24, &g, &fi, &e) != AV_OK) {
        if (g != NULL || e.code[0] == '\0')
            abort();
        return 0;
    }
    union {
        Vector v;
        char buf[64];
    } q;
    SET_VARSIZE(&q.v, (uint32) VECTOR_SIZE(3));
    q.v.dim = 3;
    q.v.unused = 0;
    q.v.x[0] = 0.25f;
    q.v.x[1] = -1.0f;
    q.v.x[2] = 2.0f;
    av_hit hits[8];
    size_t n = 0;
    (void) av_segment_search_hnsw(g, &q.v, 40, 1, 500, NULL, 0, 8, hits, &n, &e);
    (void) av_segment_insert(g, 999, &q.v, &e);
    (void) av_segment_search_hnsw(g, &q.v, 40, 2, 500, NULL, 0, 8, hits, &n, &e);
    av_segment_destroy(g);
    return 0;
}
