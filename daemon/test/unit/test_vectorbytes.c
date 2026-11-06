/*
 * test_vectorbytes.c: VectorBytes codec against spec/golden/vectorbytes.json, plus
 * hex and base64 round trips and rejections.
 */
#include "postgres.h"
#include "varatt.h"

#include <string.h>

#include "codec/base64.h"
#include "codec/hex.h"
#include "codec/vectorbytes.h"
#include "halfutils.h"
#include "halfvec.h"
#include "sparsevec.h"
#include "utils/varbit.h"
#include "vector.h"
#include "yyjson.h"

#include "greatest.h"

#define GOLDEN_PATH AV_REPO_ROOT "/spec/golden/vectorbytes.json"

static yyjson_doc *golden_doc(void)
{
    static yyjson_doc *doc;
    if (doc == NULL)
        doc = yyjson_read_file(GOLDEN_PATH, 0, NULL, NULL);
    return doc;
}

static uint8_t *hex_to_bytes(av_arena *a, const char *hex, size_t *n)
{
    uint8_t *out = NULL;
    av_err e;
    av_err_clear(&e);
    if (av_hex_decode(hex, strlen(hex), a, &out, n, &e) != AV_OK)
        return NULL;
    return out;
}

/* Build the pgvector struct of a golden case from its JSON description. */
static void *build_struct(av_arena *a, yyjson_val *c, av_vtype t, bool *ok)
{
    *ok = true;
    yyjson_val *vals = yyjson_obj_get(c, "input_values");
    if (vals == NULL)
        vals = yyjson_obj_get(c, "values");
    size_t n = vals != NULL ? yyjson_arr_size(vals) : 0;
    int32_t dim = (int32_t) yyjson_get_int(yyjson_obj_get(c, "dim"));
    size_t idx, max;
    yyjson_val *v;
    if (t == AV_VT_VECTOR) {
        Vector *x = av_arena_calloc(a, offsetof(Vector, x) + n * 4, 8);
        SET_VARSIZE(x, offsetof(Vector, x) + n * 4);
        x->dim = (int16) dim;
        yyjson_arr_foreach(vals, idx, max, v) x->x[idx] = (float) yyjson_get_real(v);
        return x;
    }
    if (t == AV_VT_HALFVEC) {
        HalfVector *x = av_arena_calloc(a, offsetof(HalfVector, x) + n * 2, 8);
        SET_VARSIZE(x, offsetof(HalfVector, x) + n * 2);
        x->dim = (int16) dim;
        yyjson_arr_foreach(vals, idx, max, v) x->x[idx] = Float4ToHalfUnchecked((float) yyjson_get_real(v));
        return x;
    }
    if (t == AV_VT_BIT) {
        const char *bits = yyjson_get_str(yyjson_obj_get(c, "bits"));
        size_t nb = strlen(bits);
        size_t nbytes = (nb + 7) / 8;
        VarBit *x = av_arena_calloc(a, VARHDRSZ + VARBITHDRSZ + nbytes, 8);
        SET_VARSIZE(x, VARHDRSZ + VARBITHDRSZ + nbytes);
        x->bit_len = (int32) nb;
        for (size_t i = 0; i < nb; i++)
            if (bits[i] == '1')
                x->bit_dat[i / 8] = (bits8) (x->bit_dat[i / 8] | (0x80u >> (i % 8)));
        return x;
    }
    yyjson_val *ind = yyjson_obj_get(c, "indices");
    size_t nnz = yyjson_arr_size(ind);
    size_t sz = offsetof(SparseVector, indices) + nnz * 8;
    SparseVector *x = av_arena_calloc(a, sz, 8);
    SET_VARSIZE(x, sz);
    x->dim = dim;
    x->nnz = (int32) nnz;
    float *sv = (float *) (x->indices + nnz);
    yyjson_arr_foreach(ind, idx, max, v) x->indices[idx] = (int32) yyjson_get_int(v);
    yyjson_arr_foreach(vals, idx, max, v) sv[idx] = (float) yyjson_get_real(v);
    return x;
}

static bool decoded_matches(yyjson_val *c, av_vtype t, const void *p)
{
    yyjson_val *vals = yyjson_obj_get(c, "values");
    size_t idx, max;
    yyjson_val *v;
    if (t == AV_VT_VECTOR) {
        const Vector *x = p;
        if (x->dim != (int16) yyjson_get_int(yyjson_obj_get(c, "dim")))
            return false;
        yyjson_arr_foreach(vals, idx, max, v) if ((double) x->x[idx] != yyjson_get_real(v)) return false;
        return true;
    }
    if (t == AV_VT_HALFVEC) {
        const HalfVector *x = p;
        if (x->dim != (int16) yyjson_get_int(yyjson_obj_get(c, "dim")))
            return false;
        yyjson_arr_foreach(vals, idx, max,
                           v) if ((double) HalfToFloat4(x->x[idx]) != yyjson_get_real(v)) return false;
        return true;
    }
    if (t == AV_VT_BIT) {
        const VarBit *x = p;
        const char *bits = yyjson_get_str(yyjson_obj_get(c, "bits"));
        if ((size_t) x->bit_len != strlen(bits))
            return false;
        for (size_t i = 0; i < strlen(bits); i++) {
            int bit = (x->bit_dat[i / 8] >> (7 - i % 8)) & 1;
            if (bit != (bits[i] == '1'))
                return false;
        }
        return true;
    }
    const SparseVector *x = p;
    yyjson_val *ind = yyjson_obj_get(c, "indices");
    if (x->dim != (int32) yyjson_get_int(yyjson_obj_get(c, "dim")) || (size_t) x->nnz != yyjson_arr_size(ind))
        return false;
    const float *sv = (const float *) (x->indices + x->nnz);
    yyjson_arr_foreach(ind, idx, max, v) if (x->indices[idx] != (int32) yyjson_get_int(v)) return false;
    yyjson_arr_foreach(vals, idx, max, v) if ((double) sv[idx] != yyjson_get_real(v)) return false;
    return true;
}

TEST golden_cases(void)
{
    yyjson_doc *doc = golden_doc();
    ASSERTm("cannot load " GOLDEN_PATH, doc != NULL);
    yyjson_val *cases = yyjson_obj_get(yyjson_doc_get_root(doc), "cases");
    size_t idx, max, nvalid = 0, ninvalid = 0;
    yyjson_val *c;
    yyjson_arr_foreach(cases, idx, max, c)
    {
        const char *name = yyjson_get_str(yyjson_obj_get(c, "name"));
        av_arena *a = av_arena_create("t-vb", 4096, 0);
        size_t n = 0;
        uint8_t *blob = hex_to_bytes(a, yyjson_get_str(yyjson_obj_get(c, "hex")), &n);
        ASSERTm(name, blob != NULL);
        av_err e;
        av_err_clear(&e);
        if (yyjson_get_bool(yyjson_obj_get(c, "valid"))) {
            nvalid++;
            av_vtype t;
            ASSERTm(name, av_vtype_parse(yyjson_get_str(yyjson_obj_get(c, "type")), &t));
            void *pg = NULL;
            av_vb_info info;
            ASSERT_EQm(name, AV_OK, av_vb_decode(blob, n, a, &pg, &info, &e));
            ASSERT_EQm(name, t, info.type);
            ASSERTm(name, ((uintptr_t) pg & 7) == 0);
            ASSERTm(name, (size_t) VARSIZE(pg) == n);
            ASSERTm(name, decoded_matches(c, t, pg));
            bool ok;
            void *src = build_struct(a, c, t, &ok);
            uint8_t *enc = NULL;
            size_t enclen = 0;
            bool norm = yyjson_get_bool(yyjson_obj_get(c, "normalized"));
            ASSERT_EQm(name, AV_OK, av_vb_encode(src, t, norm, a, &enc, &enclen, &e));
            ASSERT_EQm(name, n, enclen);
            ASSERTm(name, memcmp(enc, blob, n) == 0);
            /* base64 column matches too */
            char *b64 = NULL;
            size_t b64n = 0;
            ASSERT_EQm(name, AV_OK, av_base64_encode(blob, n, a, &b64, &b64n, &e));
            ASSERT_STR_EQm(name, yyjson_get_str(yyjson_obj_get(c, "base64")), b64);
        } else {
            ninvalid++;
            const char *want = yyjson_get_str(yyjson_obj_get(c, "error"));
            ASSERT_EQm(name, AV_ERR, av_vb_validate(blob, n, NULL, &e));
            ASSERT_STR_EQm(name, "invalid_vector", e.code);
            ASSERT_STR_EQm(name, want, e.vb_error);
            void *pg = NULL;
            av_err_clear(&e);
            ASSERT_EQm(name, AV_ERR, av_vb_decode(blob, n, a, &pg, NULL, &e));
            ASSERT_STR_EQm(name, want, e.vb_error);
        }
        av_arena_destroy(a);
    }
    ASSERT(nvalid >= 10 && ninvalid >= 19);
    PASS();
}

TEST validate_edges(void)
{
    av_err e;
    ASSERT_EQ(AV_ERR, av_vb_validate(NULL, 0, NULL, &e));
    ASSERT_STR_EQ("VB_TOO_SHORT", e.vb_error);
    /* sparsevec with 8..15 bytes is too short even though it has the common header */
    const uint8_t sp[8] = {'A', 'V', 0x14, 0, 5, 0, 0, 0};
    ASSERT_EQ(AV_ERR, av_vb_validate(sp, sizeof(sp), NULL, &e));
    ASSERT_STR_EQ("VB_TOO_SHORT", e.vb_error);
    /* NULL error object is allowed */
    ASSERT_EQ(AV_ERR, av_vb_validate(sp, sizeof(sp), NULL, NULL));
    /* negative vector dim */
    const uint8_t neg[8] = {'A', 'V', 0x11, 0, 0xff, 0xff, 0, 0};
    ASSERT_EQ(AV_ERR, av_vb_validate(neg, sizeof(neg), NULL, &e));
    ASSERT_STR_EQ("VB_DIM_OUT_OF_RANGE", e.vb_error);
    /* sparse negative index */
    const uint8_t si[24] = {'A', 'V', 0x14, 0, 5,    0,    0,    0,    1, 0, 0,    0,
                            0,   0,   0,    0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0x80, 0x3f};
    ASSERT_EQ(AV_ERR, av_vb_validate(si, sizeof(si), NULL, &e));
    ASSERT_STR_EQ("VB_SPARSE_INDEX_OUT_OF_RANGE", e.vb_error);
    /* encode rejects a NaN in a struct */
    av_arena *a = av_arena_create("t-vb", 4096, 0);
    Vector *x = av_arena_calloc(a, offsetof(Vector, x) + 4, 8);
    SET_VARSIZE(x, offsetof(Vector, x) + 4);
    x->dim = 1;
    x->x[0] = __builtin_nanf("");
    uint8_t *out;
    size_t outn;
    ASSERT_EQ(AV_ERR, av_vb_encode(x, AV_VT_VECTOR, false, a, &out, &outn, &e));
    ASSERT_STR_EQ("VB_NON_FINITE", e.vb_error);
    av_arena_destroy(a);
    PASS();
}

TEST encoded_size_limits(void)
{
    size_t s;
    ASSERT(av_vb_encoded_size(AV_VT_VECTOR, 16000, 0, &s) && s == 64008);
    ASSERT(!av_vb_encoded_size(AV_VT_VECTOR, 16001, 0, &s));
    ASSERT(av_vb_encoded_size(AV_VT_BIT, 64000, 0, &s) && s == 8008);
    ASSERT(!av_vb_encoded_size(AV_VT_BIT, 0, 0, &s));
    ASSERT(av_vb_encoded_size(AV_VT_SPARSEVEC, 1000000000, 16000, &s) && s == 16 + 8 * 16000);
    ASSERT(!av_vb_encoded_size(AV_VT_SPARSEVEC, 10, 11, &s));
    ASSERT(!av_vb_encoded_size(AV_VT_SPARSEVEC, 1000000001, 0, &s));
    PASS();
}

TEST hex_roundtrip_and_rejects(void)
{
    av_arena *a = av_arena_create("t-hex", 4096, 0);
    av_err e;
    const uint8_t in[] = {0x00, 0x0f, 0xa5, 0xff};
    char *h;
    size_t hn;
    ASSERT_EQ(AV_OK, av_hex_encode(in, sizeof(in), a, &h, &hn, &e));
    ASSERT_STR_EQ("000fa5ff", h);
    ASSERT_EQ(8u, hn);
    uint8_t *b;
    size_t bn;
    ASSERT_EQ(AV_OK, av_hex_decode(h, hn, a, &b, &bn, &e));
    ASSERT(bn == 4 && memcmp(b, in, 4) == 0);
    ASSERT_EQ(AV_OK, av_hex_decode("0x000FA5ff", 10, a, &b, &bn, &e));
    ASSERT(bn == 4 && memcmp(b, in, 4) == 0);
    ASSERT_EQ(AV_OK, av_hex_decode("", 0, a, &b, &bn, &e));
    ASSERT_EQ(0u, bn);
    ASSERT_EQ(AV_OK, av_hex_decode("0x", 2, a, &b, &bn, &e));
    ASSERT_EQ(0u, bn);
    static const char *bad[] = {"abc",    "0xabc", "zz",  "ab cd",           "ab\n",
                                "0x0x00", "-1",    "ab ", "\xc3\xa9\xc3\xa9"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        av_err_clear(&e);
        ASSERT_EQm(bad[i], AV_ERR, av_hex_decode(bad[i], strlen(bad[i]), a, &b, &bn, &e));
        ASSERT_STR_EQ("invalid_argument", e.code);
    }
    /* embedded NUL is a bad character, not a terminator */
    ASSERT_EQ(AV_ERR, av_hex_decode("a\0", 2, a, &b, &bn, &e));
    av_arena_destroy(a);
    PASS();
}

TEST base64_roundtrip_and_rejects(void)
{
    av_arena *a = av_arena_create("t-b64", 4096, 0);
    av_err e;
    static const struct {
        const char *plain;
        const char *enc;
    } rfc[] = {{"", ""},
               {"f", "Zg=="},
               {"fo", "Zm8="},
               {"foo", "Zm9v"},
               {"foob", "Zm9vYg=="},
               {"fooba", "Zm9vYmE="},
               {"foobar", "Zm9vYmFy"}};
    for (size_t i = 0; i < sizeof(rfc) / sizeof(rfc[0]); i++) {
        char *s;
        size_t sn;
        ASSERT_EQ(AV_OK,
                  av_base64_encode((const uint8_t *) rfc[i].plain, strlen(rfc[i].plain), a, &s, &sn, &e));
        ASSERT_STR_EQ(rfc[i].enc, s);
        uint8_t *b;
        size_t bn;
        ASSERT_EQ(AV_OK, av_base64_decode(rfc[i].enc, strlen(rfc[i].enc), a, &b, &bn, &e));
        ASSERT(bn == strlen(rfc[i].plain) && memcmp(b, rfc[i].plain, bn) == 0);
    }
    /* all 256 byte values round trip */
    uint8_t all[256];
    for (size_t i = 0; i < 256; i++)
        all[i] = (uint8_t) i;
    char *s;
    size_t sn;
    uint8_t *b;
    size_t bn;
    ASSERT_EQ(AV_OK, av_base64_encode(all, sizeof(all), a, &s, &sn, &e));
    ASSERT_EQ(AV_OK, av_base64_decode(s, sn, a, &b, &bn, &e));
    ASSERT(bn == 256 && memcmp(b, all, 256) == 0);
    static const char *bad[] = {"Zg=",  "Zg",   "Zg=A",  "Z===", "====", "Zm9v\n",   "Zm 9", "Zm9v ",
                                "Zh==", "Zm9=", "Zm9v=", "Zm-v", "Zm_v", "Zg==Zg==", "=Zg=", "Zm9vYg==Zg=="};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        av_err_clear(&e);
        ASSERT_EQm(bad[i], AV_ERR, av_base64_decode(bad[i], strlen(bad[i]), a, &b, &bn, &e));
        ASSERT_STR_EQ("invalid_argument", e.code);
    }
    av_arena_destroy(a);
    PASS();
}

SUITE(vectorbytes)
{
    RUN_TEST(golden_cases);
    RUN_TEST(validate_edges);
    RUN_TEST(encoded_size_limits);
    RUN_TEST(hex_roundtrip_and_rejects);
    RUN_TEST(base64_roundtrip_and_rejects);
}
