/*
 * test_text_io.c: table-driven port of the literal-input, output and distance-function
 * cases of pgvector's regression tests (pgvector/test/sql + test/expected):
 *   vector_type.sql, halfvec.sql, sparsevec.sql, bit.sql.
 *
 * The case table below was generated from those files by a one-off script. Each row is
 * a statement of the form
 *     SELECT 'lit'::T;  |  SELECT 'lit'::T(n);        (text input, then text output)
 *     SELECT fn('a', 'b');  |  SELECT 'a' <op> 'b';  (distance function on parsed structs)
 * together with the expected output or the exact ERROR message from the .out file.
 * Skipped on purpose: array casts, arithmetic, aggregates, comparison operators, anything
 * needing other SQL functions, and typmod_in errors (vector(0), vector(16001): they belong
 * to the SQL type-modifier parser, not to the literal parser).
 *
 * Distance results: PostgreSQL prints float8 with the shortest round-trip digits, so
 * strtod() of the expected text is the exact double pgvector produced. We accept equality
 * or a relative difference of 1e-12 (the copied kernels may run in a different SIMD tier
 * than the one used to generate the upstream expected files). NaN matches NaN and
 * "Infinity"/"-Infinity" are matched exactly.
 *
 * varbit(n) casts in bit.sql are parsed with no typmod (varbit(n) means "at most n bits"
 * and the API's typmod means "exactly n bits"); the length-mismatch outcomes are the same.
 * Rows labelled (handwritten) are not from the pgvector tests: bit_in/bit_out literals
 * that bit.sql does not cover, and a few boundary/failure cases.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"

#include "fmgr.h"
#include "utils/elog.h"
#include "utils/memutils.h"

#include "codec/text.h"
#include "core/arena.h"
#include "kernels/dispatch.h"
#include "log.h"

#include "greatest.h"

extern Datum l2_distance(PG_FUNCTION_ARGS);
extern Datum inner_product(PG_FUNCTION_ARGS);
extern Datum vector_negative_inner_product(PG_FUNCTION_ARGS);
extern Datum cosine_distance(PG_FUNCTION_ARGS);
extern Datum l1_distance(PG_FUNCTION_ARGS);
extern Datum halfvec_l2_distance(PG_FUNCTION_ARGS);
extern Datum halfvec_inner_product(PG_FUNCTION_ARGS);
extern Datum halfvec_negative_inner_product(PG_FUNCTION_ARGS);
extern Datum halfvec_cosine_distance(PG_FUNCTION_ARGS);
extern Datum halfvec_l1_distance(PG_FUNCTION_ARGS);
extern Datum sparsevec_l2_distance(PG_FUNCTION_ARGS);
extern Datum sparsevec_inner_product(PG_FUNCTION_ARGS);
extern Datum sparsevec_negative_inner_product(PG_FUNCTION_ARGS);
extern Datum sparsevec_cosine_distance(PG_FUNCTION_ARGS);
extern Datum sparsevec_l1_distance(PG_FUNCTION_ARGS);
extern Datum hamming_distance(PG_FUNCTION_ARGS);
extern Datum jaccard_distance(PG_FUNCTION_ARGS);

typedef enum { C_IN, C_DIST } case_kind;

typedef struct {
    av_vtype t;
    case_kind kind;
    const char *sql;  /* original statement (test name) */
    const char *fn;   /* C_DIST: copied function name */
    const char *a;    /* literal (first operand) */
    const char *b;    /* C_DIST: second operand literal */
    int typmod;       /* C_IN: dimension from ::T(n), else -1 */
    int is_err;       /* 1: expect ERROR with message exp */
    const char *exp;  /* expected text output, float8 text, or ERROR message */
} text_case;

static const struct {
    const char *name;
    PGFunction fn;
} dist_fns[] = {
    {"l2_distance", l2_distance},
    {"inner_product", inner_product},
    {"vector_negative_inner_product", vector_negative_inner_product},
    {"cosine_distance", cosine_distance},
    {"l1_distance", l1_distance},
    {"halfvec_l2_distance", halfvec_l2_distance},
    {"halfvec_inner_product", halfvec_inner_product},
    {"halfvec_negative_inner_product", halfvec_negative_inner_product},
    {"halfvec_cosine_distance", halfvec_cosine_distance},
    {"halfvec_l1_distance", halfvec_l1_distance},
    {"sparsevec_l2_distance", sparsevec_l2_distance},
    {"sparsevec_inner_product", sparsevec_inner_product},
    {"sparsevec_negative_inner_product", sparsevec_negative_inner_product},
    {"sparsevec_cosine_distance", sparsevec_cosine_distance},
    {"sparsevec_l1_distance", sparsevec_l1_distance},
    {"hamming_distance", hamming_distance},
    {"jaccard_distance", jaccard_distance},
};

/* clang-format off */
static const text_case cases[] = {
    { AV_VT_VECTOR, C_IN, "SELECT '[1,2,3]'::vector;", "", "[1,2,3]", "", -1, 0, "[1,2,3]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[-1,-2,-3]'::vector;", "", "[-1,-2,-3]", "", -1, 0, "[-1,-2,-3]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1.,2.,3.]'::vector;", "", "[1.,2.,3.]", "", -1, 0, "[1,2,3]" },
    { AV_VT_VECTOR, C_IN, "SELECT ' [ 1,  2 ,    3  ] '::vector;", "", " [ 1,  2 ,    3  ] ", "", -1, 0, "[1,2,3]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1.23456]'::vector;", "", "[1.23456]", "", -1, 0, "[1.23456]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[hello,1]'::vector;", "", "[hello,1]", "", -1, 1, "invalid input syntax for type vector: \"[hello,1]\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[NaN,1]'::vector;", "", "[NaN,1]", "", -1, 1, "NaN not allowed in vector" },
    { AV_VT_VECTOR, C_IN, "SELECT '[Infinity,1]'::vector;", "", "[Infinity,1]", "", -1, 1, "infinite value not allowed in vector" },
    { AV_VT_VECTOR, C_IN, "SELECT '[-Infinity,1]'::vector;", "", "[-Infinity,1]", "", -1, 1, "infinite value not allowed in vector" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1.5e38,-1.5e38]'::vector;", "", "[1.5e38,-1.5e38]", "", -1, 0, "[1.5e+38,-1.5e+38]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1.5e+38,-1.5e+38]'::vector;", "", "[1.5e+38,-1.5e+38]", "", -1, 0, "[1.5e+38,-1.5e+38]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1.5e-38,-1.5e-38]'::vector;", "", "[1.5e-38,-1.5e-38]", "", -1, 0, "[1.5e-38,-1.5e-38]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[4e38,1]'::vector;", "", "[4e38,1]", "", -1, 1, "\"4e38\" is out of range for type vector" },
    { AV_VT_VECTOR, C_IN, "SELECT '[-4e38,1]'::vector;", "", "[-4e38,1]", "", -1, 1, "\"-4e38\" is out of range for type vector" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1e-46,1]'::vector;", "", "[1e-46,1]", "", -1, 0, "[0,1]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[-1e-46,1]'::vector;", "", "[-1e-46,1]", "", -1, 0, "[-0,1]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1,2,3'::vector;", "", "[1,2,3", "", -1, 1, "invalid input syntax for type vector: \"[1,2,3\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1,2,3]9'::vector;", "", "[1,2,3]9", "", -1, 1, "invalid input syntax for type vector: \"[1,2,3]9\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '1,2,3'::vector;", "", "1,2,3", "", -1, 1, "invalid input syntax for type vector: \"1,2,3\"" },
    { AV_VT_VECTOR, C_IN, "SELECT ''::vector;", "", "", "", -1, 1, "invalid input syntax for type vector: \"\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '['::vector;", "", "[", "", -1, 1, "invalid input syntax for type vector: \"[\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[ '::vector;", "", "[ ", "", -1, 1, "invalid input syntax for type vector: \"[ \"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[,'::vector;", "", "[,", "", -1, 1, "invalid input syntax for type vector: \"[,\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[]'::vector;", "", "[]", "", -1, 1, "vector must have at least 1 dimension" },
    { AV_VT_VECTOR, C_IN, "SELECT '[ ]'::vector;", "", "[ ]", "", -1, 1, "vector must have at least 1 dimension" },
    { AV_VT_VECTOR, C_IN, "SELECT '[,]'::vector;", "", "[,]", "", -1, 1, "invalid input syntax for type vector: \"[,]\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1,]'::vector;", "", "[1,]", "", -1, 1, "invalid input syntax for type vector: \"[1,]\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1a]'::vector;", "", "[1a]", "", -1, 1, "invalid input syntax for type vector: \"[1a]\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1,,3]'::vector;", "", "[1,,3]", "", -1, 1, "invalid input syntax for type vector: \"[1,,3]\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1, ,3]'::vector;", "", "[1, ,3]", "", -1, 1, "invalid input syntax for type vector: \"[1, ,3]\"" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1,2,3]'::vector(3);", "", "[1,2,3]", "", 3, 0, "[1,2,3]" },
    { AV_VT_VECTOR, C_IN, "SELECT '[1,2,3]'::vector(2);", "", "[1,2,3]", "", 2, 1, "expected 2 dimensions, not 3" },
    { AV_VT_VECTOR, C_DIST, "SELECT l2_distance('[0,0]'::vector, '[3,4]');", "l2_distance", "[0,0]", "[3,4]", -1, 0, "5" },
    { AV_VT_VECTOR, C_DIST, "SELECT l2_distance('[0,0]'::vector, '[0,1]');", "l2_distance", "[0,0]", "[0,1]", -1, 0, "1" },
    { AV_VT_VECTOR, C_DIST, "SELECT l2_distance('[1,2]'::vector, '[3]');", "l2_distance", "[1,2]", "[3]", -1, 1, "different vector dimensions 2 and 1" },
    { AV_VT_VECTOR, C_DIST, "SELECT l2_distance('[3e38]'::vector, '[-3e38]');", "l2_distance", "[3e38]", "[-3e38]", -1, 0, "Infinity" },
    { AV_VT_VECTOR, C_DIST, "SELECT l2_distance('[1,1,1,1,1,1,1,1,1]'::vector, '[1,1,1,1,1,1,1,4,5]');", "l2_distance", "[1,1,1,1,1,1,1,1,1]", "[1,1,1,1,1,1,1,4,5]", -1, 0, "5" },
    { AV_VT_VECTOR, C_DIST, "SELECT '[0,0]'::vector <-> '[3,4]';", "l2_distance", "[0,0]", "[3,4]", -1, 0, "5" },
    { AV_VT_VECTOR, C_DIST, "SELECT inner_product('[1,2]'::vector, '[3,4]');", "inner_product", "[1,2]", "[3,4]", -1, 0, "11" },
    { AV_VT_VECTOR, C_DIST, "SELECT inner_product('[1,2]'::vector, '[3]');", "inner_product", "[1,2]", "[3]", -1, 1, "different vector dimensions 2 and 1" },
    { AV_VT_VECTOR, C_DIST, "SELECT inner_product('[3e38]'::vector, '[3e38]');", "inner_product", "[3e38]", "[3e38]", -1, 0, "Infinity" },
    { AV_VT_VECTOR, C_DIST, "SELECT inner_product('[1,1,1,1,1,1,1,1,1]'::vector, '[1,2,3,4,5,6,7,8,9]');", "inner_product", "[1,1,1,1,1,1,1,1,1]", "[1,2,3,4,5,6,7,8,9]", -1, 0, "45" },
    { AV_VT_VECTOR, C_DIST, "SELECT '[1,2]'::vector <#> '[3,4]';", "vector_negative_inner_product", "[1,2]", "[3,4]", -1, 0, "-11" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,2]'::vector, '[2,4]');", "cosine_distance", "[1,2]", "[2,4]", -1, 0, "0" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,2]'::vector, '[0,0]');", "cosine_distance", "[1,2]", "[0,0]", -1, 0, "NaN" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,1]'::vector, '[1,1]');", "cosine_distance", "[1,1]", "[1,1]", -1, 0, "0" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,0]'::vector, '[0,2]');", "cosine_distance", "[1,0]", "[0,2]", -1, 0, "1" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,1]'::vector, '[-1,-1]');", "cosine_distance", "[1,1]", "[-1,-1]", -1, 0, "2" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,2]'::vector, '[3]');", "cosine_distance", "[1,2]", "[3]", -1, 1, "different vector dimensions 2 and 1" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,1]'::vector, '[1.1,1.1]');", "cosine_distance", "[1,1]", "[1.1,1.1]", -1, 0, "0" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,1]'::vector, '[-1.1,-1.1]');", "cosine_distance", "[1,1]", "[-1.1,-1.1]", -1, 0, "2" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[3e38]'::vector, '[3e38]');", "cosine_distance", "[3e38]", "[3e38]", -1, 0, "NaN" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,2,3,4,5,6,7,8,9]'::vector, '[1,2,3,4,5,6,7,8,9]');", "cosine_distance", "[1,2,3,4,5,6,7,8,9]", "[1,2,3,4,5,6,7,8,9]", -1, 0, "0" },
    { AV_VT_VECTOR, C_DIST, "SELECT cosine_distance('[1,2,3,4,5,6,7,8,9]'::vector, '[-1,-2,-3,-4,-5,-6,-7,-8,-9]');", "cosine_distance", "[1,2,3,4,5,6,7,8,9]", "[-1,-2,-3,-4,-5,-6,-7,-8,-9]", -1, 0, "2" },
    { AV_VT_VECTOR, C_DIST, "SELECT '[1,2]'::vector <=> '[2,4]';", "cosine_distance", "[1,2]", "[2,4]", -1, 0, "0" },
    { AV_VT_VECTOR, C_DIST, "SELECT l1_distance('[0,0]'::vector, '[3,4]');", "l1_distance", "[0,0]", "[3,4]", -1, 0, "7" },
    { AV_VT_VECTOR, C_DIST, "SELECT l1_distance('[0,0]'::vector, '[0,1]');", "l1_distance", "[0,0]", "[0,1]", -1, 0, "1" },
    { AV_VT_VECTOR, C_DIST, "SELECT l1_distance('[1,2]'::vector, '[3]');", "l1_distance", "[1,2]", "[3]", -1, 1, "different vector dimensions 2 and 1" },
    { AV_VT_VECTOR, C_DIST, "SELECT l1_distance('[3e38]'::vector, '[-3e38]');", "l1_distance", "[3e38]", "[-3e38]", -1, 0, "Infinity" },
    { AV_VT_VECTOR, C_DIST, "SELECT l1_distance('[1,2,3,4,5,6,7,8,9]'::vector, '[1,2,3,4,5,6,7,8,9]');", "l1_distance", "[1,2,3,4,5,6,7,8,9]", "[1,2,3,4,5,6,7,8,9]", -1, 0, "0" },
    { AV_VT_VECTOR, C_DIST, "SELECT l1_distance('[1,2,3,4,5,6,7,8,9]'::vector, '[0,3,2,5,4,7,6,9,8]');", "l1_distance", "[1,2,3,4,5,6,7,8,9]", "[0,3,2,5,4,7,6,9,8]", -1, 0, "9" },
    { AV_VT_VECTOR, C_DIST, "SELECT '[0,0]'::vector <+> '[3,4]';", "l1_distance", "[0,0]", "[3,4]", -1, 0, "7" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1,2,3]'::halfvec;", "", "[1,2,3]", "", -1, 0, "[1,2,3]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[-1,-2,-3]'::halfvec;", "", "[-1,-2,-3]", "", -1, 0, "[-1,-2,-3]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1.,2.,3.]'::halfvec;", "", "[1.,2.,3.]", "", -1, 0, "[1,2,3]" },
    { AV_VT_HALFVEC, C_IN, "SELECT ' [ 1,  2 ,    3  ] '::halfvec;", "", " [ 1,  2 ,    3  ] ", "", -1, 0, "[1,2,3]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1.23456]'::halfvec;", "", "[1.23456]", "", -1, 0, "[1.234375]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[hello,1]'::halfvec;", "", "[hello,1]", "", -1, 1, "invalid input syntax for type halfvec: \"[hello,1]\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[NaN,1]'::halfvec;", "", "[NaN,1]", "", -1, 1, "NaN not allowed in halfvec" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[Infinity,1]'::halfvec;", "", "[Infinity,1]", "", -1, 1, "infinite value not allowed in halfvec" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[-Infinity,1]'::halfvec;", "", "[-Infinity,1]", "", -1, 1, "infinite value not allowed in halfvec" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[65519,-65519]'::halfvec;", "", "[65519,-65519]", "", -1, 0, "[65504,-65504]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[65520,-65520]'::halfvec;", "", "[65520,-65520]", "", -1, 1, "\"65520\" is out of range for type halfvec" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1e-8,-1e-8]'::halfvec;", "", "[1e-8,-1e-8]", "", -1, 0, "[0,-0]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[4e38,1]'::halfvec;", "", "[4e38,1]", "", -1, 1, "\"4e38\" is out of range for type halfvec" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1e-46,1]'::halfvec;", "", "[1e-46,1]", "", -1, 0, "[0,1]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1,2,3'::halfvec;", "", "[1,2,3", "", -1, 1, "invalid input syntax for type halfvec: \"[1,2,3\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1,2,3]9'::halfvec;", "", "[1,2,3]9", "", -1, 1, "invalid input syntax for type halfvec: \"[1,2,3]9\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '1,2,3'::halfvec;", "", "1,2,3", "", -1, 1, "invalid input syntax for type halfvec: \"1,2,3\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT ''::halfvec;", "", "", "", -1, 1, "invalid input syntax for type halfvec: \"\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '['::halfvec;", "", "[", "", -1, 1, "invalid input syntax for type halfvec: \"[\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[ '::halfvec;", "", "[ ", "", -1, 1, "invalid input syntax for type halfvec: \"[ \"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[,'::halfvec;", "", "[,", "", -1, 1, "invalid input syntax for type halfvec: \"[,\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[]'::halfvec;", "", "[]", "", -1, 1, "halfvec must have at least 1 dimension" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[ ]'::halfvec;", "", "[ ]", "", -1, 1, "halfvec must have at least 1 dimension" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[,]'::halfvec;", "", "[,]", "", -1, 1, "invalid input syntax for type halfvec: \"[,]\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1,]'::halfvec;", "", "[1,]", "", -1, 1, "invalid input syntax for type halfvec: \"[1,]\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1a]'::halfvec;", "", "[1a]", "", -1, 1, "invalid input syntax for type halfvec: \"[1a]\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1,,3]'::halfvec;", "", "[1,,3]", "", -1, 1, "invalid input syntax for type halfvec: \"[1,,3]\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1, ,3]'::halfvec;", "", "[1, ,3]", "", -1, 1, "invalid input syntax for type halfvec: \"[1, ,3]\"" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1,2,3]'::halfvec(3);", "", "[1,2,3]", "", 3, 0, "[1,2,3]" },
    { AV_VT_HALFVEC, C_IN, "SELECT '[1,2,3]'::halfvec(2);", "", "[1,2,3]", "", 2, 1, "expected 2 dimensions, not 3" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l2_distance('[0,0]'::halfvec, '[3,4]');", "halfvec_l2_distance", "[0,0]", "[3,4]", -1, 0, "5" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l2_distance('[0,0]'::halfvec, '[0,1]');", "halfvec_l2_distance", "[0,0]", "[0,1]", -1, 0, "1" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l2_distance('[1,2]'::halfvec, '[3]');", "halfvec_l2_distance", "[1,2]", "[3]", -1, 1, "different halfvec dimensions 2 and 1" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l2_distance('[1,1,1,1,1,1,1,1,1]'::halfvec, '[1,1,1,1,1,1,1,4,5]');", "halfvec_l2_distance", "[1,1,1,1,1,1,1,1,1]", "[1,1,1,1,1,1,1,4,5]", -1, 0, "5" },
    { AV_VT_HALFVEC, C_DIST, "SELECT '[0,0]'::halfvec <-> '[3,4]';", "halfvec_l2_distance", "[0,0]", "[3,4]", -1, 0, "5" },
    { AV_VT_HALFVEC, C_DIST, "SELECT inner_product('[1,2]'::halfvec, '[3,4]');", "halfvec_inner_product", "[1,2]", "[3,4]", -1, 0, "11" },
    { AV_VT_HALFVEC, C_DIST, "SELECT inner_product('[1,2]'::halfvec, '[3]');", "halfvec_inner_product", "[1,2]", "[3]", -1, 1, "different halfvec dimensions 2 and 1" },
    { AV_VT_HALFVEC, C_DIST, "SELECT inner_product('[65504]'::halfvec, '[65504]');", "halfvec_inner_product", "[65504]", "[65504]", -1, 0, "4290774016" },
    { AV_VT_HALFVEC, C_DIST, "SELECT inner_product('[1,1,1,1,1,1,1,1,1]'::halfvec, '[1,2,3,4,5,6,7,8,9]');", "halfvec_inner_product", "[1,1,1,1,1,1,1,1,1]", "[1,2,3,4,5,6,7,8,9]", -1, 0, "45" },
    { AV_VT_HALFVEC, C_DIST, "SELECT '[1,2]'::halfvec <#> '[3,4]';", "halfvec_negative_inner_product", "[1,2]", "[3,4]", -1, 0, "-11" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,2]'::halfvec, '[2,4]');", "halfvec_cosine_distance", "[1,2]", "[2,4]", -1, 0, "0" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,2]'::halfvec, '[0,0]');", "halfvec_cosine_distance", "[1,2]", "[0,0]", -1, 0, "NaN" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,1]'::halfvec, '[1,1]');", "halfvec_cosine_distance", "[1,1]", "[1,1]", -1, 0, "0" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,0]'::halfvec, '[0,2]');", "halfvec_cosine_distance", "[1,0]", "[0,2]", -1, 0, "1" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,1]'::halfvec, '[-1,-1]');", "halfvec_cosine_distance", "[1,1]", "[-1,-1]", -1, 0, "2" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,2]'::halfvec, '[3]');", "halfvec_cosine_distance", "[1,2]", "[3]", -1, 1, "different halfvec dimensions 2 and 1" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,1]'::halfvec, '[1.1,1.1]');", "halfvec_cosine_distance", "[1,1]", "[1.1,1.1]", -1, 0, "0" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,1]'::halfvec, '[-1.1,-1.1]');", "halfvec_cosine_distance", "[1,1]", "[-1.1,-1.1]", -1, 0, "2" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,2,3,4,5,6,7,8,9]'::halfvec, '[1,2,3,4,5,6,7,8,9]');", "halfvec_cosine_distance", "[1,2,3,4,5,6,7,8,9]", "[1,2,3,4,5,6,7,8,9]", -1, 0, "0" },
    { AV_VT_HALFVEC, C_DIST, "SELECT cosine_distance('[1,2,3,4,5,6,7,8,9]'::halfvec, '[-1,-2,-3,-4,-5,-6,-7,-8,-9]');", "halfvec_cosine_distance", "[1,2,3,4,5,6,7,8,9]", "[-1,-2,-3,-4,-5,-6,-7,-8,-9]", -1, 0, "2" },
    { AV_VT_HALFVEC, C_DIST, "SELECT '[1,2]'::halfvec <=> '[2,4]';", "halfvec_cosine_distance", "[1,2]", "[2,4]", -1, 0, "0" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l1_distance('[0,0]'::halfvec, '[3,4]');", "halfvec_l1_distance", "[0,0]", "[3,4]", -1, 0, "7" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l1_distance('[0,0]'::halfvec, '[0,1]');", "halfvec_l1_distance", "[0,0]", "[0,1]", -1, 0, "1" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l1_distance('[1,2]'::halfvec, '[3]');", "halfvec_l1_distance", "[1,2]", "[3]", -1, 1, "different halfvec dimensions 2 and 1" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l1_distance('[1,2,3,4,5,6,7,8,9]'::halfvec, '[1,2,3,4,5,6,7,8,9]');", "halfvec_l1_distance", "[1,2,3,4,5,6,7,8,9]", "[1,2,3,4,5,6,7,8,9]", -1, 0, "0" },
    { AV_VT_HALFVEC, C_DIST, "SELECT l1_distance('[1,2,3,4,5,6,7,8,9]'::halfvec, '[0,3,2,5,4,7,6,9,8]');", "halfvec_l1_distance", "[1,2,3,4,5,6,7,8,9]", "[0,3,2,5,4,7,6,9,8]", -1, 0, "9" },
    { AV_VT_HALFVEC, C_DIST, "SELECT '[0,0]'::halfvec <+> '[3,4]';", "halfvec_l1_distance", "[0,0]", "[3,4]", -1, 0, "7" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1.5,3:3.5}/5'::sparsevec;", "", "{1:1.5,3:3.5}/5", "", -1, 0, "{1:1.5,3:3.5}/5" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:-2,3:-4}/5'::sparsevec;", "", "{1:-2,3:-4}/5", "", -1, 0, "{1:-2,3:-4}/5" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:2.,3:4.}/5'::sparsevec;", "", "{1:2.,3:4.}/5", "", -1, 0, "{1:2,3:4}/5" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT ' { 1 : 1.5 ,  3  :  3.5  } / 5 '::sparsevec;", "", " { 1 : 1.5 ,  3  :  3.5  } / 5 ", "", -1, 0, "{1:1.5,3:3.5}/5" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1.23456}/1'::sparsevec;", "", "{1:1.23456}/1", "", -1, 0, "{1:1.23456}/1" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:hello,2:1}/2'::sparsevec;", "", "{1:hello,2:1}/2", "", -1, 1, "invalid input syntax for type sparsevec: \"{1:hello,2:1}/2\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:NaN,2:1}/2'::sparsevec;", "", "{1:NaN,2:1}/2", "", -1, 1, "NaN not allowed in sparsevec" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:Infinity,2:1}/2'::sparsevec;", "", "{1:Infinity,2:1}/2", "", -1, 1, "infinite value not allowed in sparsevec" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:-Infinity,2:1}/2'::sparsevec;", "", "{1:-Infinity,2:1}/2", "", -1, 1, "infinite value not allowed in sparsevec" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1.5e38,2:-1.5e38}/2'::sparsevec;", "", "{1:1.5e38,2:-1.5e38}/2", "", -1, 0, "{1:1.5e+38,2:-1.5e+38}/2" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1.5e+38,2:-1.5e+38}/2'::sparsevec;", "", "{1:1.5e+38,2:-1.5e+38}/2", "", -1, 0, "{1:1.5e+38,2:-1.5e+38}/2" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1.5e-38,2:-1.5e-38}/2'::sparsevec;", "", "{1:1.5e-38,2:-1.5e-38}/2", "", -1, 0, "{1:1.5e-38,2:-1.5e-38}/2" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:4e38,2:1}/2'::sparsevec;", "", "{1:4e38,2:1}/2", "", -1, 1, "\"4e38\" is out of range for type sparsevec" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:-4e38,2:1}/2'::sparsevec;", "", "{1:-4e38,2:1}/2", "", -1, 1, "\"-4e38\" is out of range for type sparsevec" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1e-46,2:1}/2'::sparsevec;", "", "{1:1e-46,2:1}/2", "", -1, 1, "\"1e-46\" is out of range for type sparsevec" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:-1e-46,2:1}/2'::sparsevec;", "", "{1:-1e-46,2:1}/2", "", -1, 1, "\"-1e-46\" is out of range for type sparsevec" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT ''::sparsevec;", "", "", "", -1, 1, "invalid input syntax for type sparsevec: \"\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{'::sparsevec;", "", "{", "", -1, 1, "invalid input syntax for type sparsevec: \"{\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{ '::sparsevec;", "", "{ ", "", -1, 1, "invalid input syntax for type sparsevec: \"{ \"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{:'::sparsevec;", "", "{:", "", -1, 1, "invalid input syntax for type sparsevec: \"{:\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{,'::sparsevec;", "", "{,", "", -1, 1, "invalid input syntax for type sparsevec: \"{,\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}'::sparsevec;", "", "{}", "", -1, 1, "invalid input syntax for type sparsevec: \"{}\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/'::sparsevec;", "", "{}/", "", -1, 1, "invalid input syntax for type sparsevec: \"{}/\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/1'::sparsevec;", "", "{}/1", "", -1, 0, "{}/1" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/1a'::sparsevec;", "", "{}/1a", "", -1, 1, "invalid input syntax for type sparsevec: \"{}/1a\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{ }/1'::sparsevec;", "", "{ }/1", "", -1, 0, "{}/1" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{:}/1'::sparsevec;", "", "{:}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{:}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{,}/1'::sparsevec;", "", "{,}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{,}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1,}/1'::sparsevec;", "", "{1,}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{1,}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{:1}/1'::sparsevec;", "", "{:1}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{:1}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:}/1'::sparsevec;", "", "{1:}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{1:}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1a:1}/1'::sparsevec;", "", "{1a:1}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{1a:1}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1a}/1'::sparsevec;", "", "{1:1a}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{1:1a}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1,}/1'::sparsevec;", "", "{1:1,}/1", "", -1, 1, "invalid input syntax for type sparsevec: \"{1:1,}/1\"" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:0,2:1,3:0}/3'::sparsevec;", "", "{1:0,2:1,3:0}/3", "", -1, 0, "{2:1}/3" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{2:1,1:1}/2'::sparsevec;", "", "{2:1,1:1}/2", "", -1, 0, "{1:1,2:1}/2" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1,1:1}/2'::sparsevec;", "", "{1:1,1:1}/2", "", -1, 1, "sparsevec indices must not contain duplicates" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:0,1:1}/2'::sparsevec;", "", "{1:0,1:1}/2", "", -1, 1, "sparsevec indices must not contain duplicates" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1,1:0}/2'::sparsevec;", "", "{1:1,1:0}/2", "", -1, 1, "sparsevec indices must not contain duplicates" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{1:1,2:1,1:1}/2'::sparsevec;", "", "{1:1,2:1,1:1}/2", "", -1, 1, "sparsevec indices must not contain duplicates" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/5'::sparsevec;", "", "{}/5", "", -1, 0, "{}/5" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/-1'::sparsevec;", "", "{}/-1", "", -1, 1, "sparsevec must have at least 1 dimension" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/1000000000'::sparsevec;", "", "{}/1000000000", "", -1, 0, "{}/1000000000" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/1000000001'::sparsevec;", "", "{}/1000000001", "", -1, 1, "sparsevec cannot have more than 1000000000 dimensions" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/2147483648'::sparsevec;", "", "{}/2147483648", "", -1, 1, "sparsevec cannot have more than 1000000000 dimensions" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/-2147483649'::sparsevec;", "", "{}/-2147483649", "", -1, 1, "sparsevec must have at least 1 dimension" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/9223372036854775808'::sparsevec;", "", "{}/9223372036854775808", "", -1, 1, "sparsevec cannot have more than 1000000000 dimensions" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/-9223372036854775809'::sparsevec;", "", "{}/-9223372036854775809", "", -1, 1, "sparsevec must have at least 1 dimension" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{2147483647:1}/1'::sparsevec;", "", "{2147483647:1}/1", "", -1, 1, "sparsevec index out of bounds" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{2147483648:1}/1'::sparsevec;", "", "{2147483648:1}/1", "", -1, 1, "sparsevec index out of bounds" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{-2147483648:1}/1'::sparsevec;", "", "{-2147483648:1}/1", "", -1, 1, "sparsevec index out of bounds" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{-2147483649:1}/1'::sparsevec;", "", "{-2147483649:1}/1", "", -1, 1, "sparsevec index out of bounds" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{0:1}/1'::sparsevec;", "", "{0:1}/1", "", -1, 1, "sparsevec index out of bounds" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{2:1}/1'::sparsevec;", "", "{2:1}/1", "", -1, 1, "sparsevec index out of bounds" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/3'::sparsevec(3);", "", "{}/3", "", 3, 0, "{}/3" },
    { AV_VT_SPARSEVEC, C_IN, "SELECT '{}/3'::sparsevec(2);", "", "{}/3", "", 2, 1, "expected 2 dimensions, not 3" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l2_distance('{}/2'::sparsevec, '{1:3,2:4}/2');", "sparsevec_l2_distance", "{}/2", "{1:3,2:4}/2", -1, 0, "5" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l2_distance('{1:3}/2'::sparsevec, '{2:4}/2');", "sparsevec_l2_distance", "{1:3}/2", "{2:4}/2", -1, 0, "5" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l2_distance('{2:4}/2'::sparsevec, '{1:3}/2');", "sparsevec_l2_distance", "{2:4}/2", "{1:3}/2", -1, 0, "5" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l2_distance('{1:3,2:4}/2'::sparsevec, '{}/2');", "sparsevec_l2_distance", "{1:3,2:4}/2", "{}/2", -1, 0, "5" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l2_distance('{}/2'::sparsevec, '{2:1}/2');", "sparsevec_l2_distance", "{}/2", "{2:1}/2", -1, 0, "1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT '{}/2'::sparsevec <-> '{1:3,2:4}/2';", "sparsevec_l2_distance", "{}/2", "{1:3,2:4}/2", -1, 0, "5" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{1:1,2:2}/2'::sparsevec, '{1:2,2:4}/2');", "sparsevec_inner_product", "{1:1,2:2}/2", "{1:2,2:4}/2", -1, 0, "10" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{1:1,2:2}/2'::sparsevec, '{1:3}/1');", "sparsevec_inner_product", "{1:1,2:2}/2", "{1:3}/1", -1, 1, "different sparsevec dimensions 2 and 1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{1:1,3:3}/4'::sparsevec, '{2:2,4:4}/4');", "sparsevec_inner_product", "{1:1,3:3}/4", "{2:2,4:4}/4", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{2:2,4:4}/4'::sparsevec, '{1:1,3:3}/4');", "sparsevec_inner_product", "{2:2,4:4}/4", "{1:1,3:3}/4", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{1:1,3:3,5:5}/5'::sparsevec, '{2:4,3:6,4:8}/5');", "sparsevec_inner_product", "{1:1,3:3,5:5}/5", "{2:4,3:6,4:8}/5", -1, 0, "18" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{1:1}/2'::sparsevec, '{}/2');", "sparsevec_inner_product", "{1:1}/2", "{}/2", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{}/2'::sparsevec, '{1:1}/2');", "sparsevec_inner_product", "{}/2", "{1:1}/2", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{1:3e38}/1'::sparsevec, '{1:3e38}/1');", "sparsevec_inner_product", "{1:3e38}/1", "{1:3e38}/1", -1, 0, "Infinity" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT inner_product('{1:1,3:3,5:5}/5'::sparsevec, '{2:4,3:6,4:8}/5');", "sparsevec_inner_product", "{1:1,3:3,5:5}/5", "{2:4,3:6,4:8}/5", -1, 0, "18" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT '{1:1,2:2}/2'::sparsevec <#> '{1:3,2:4}/2';", "sparsevec_negative_inner_product", "{1:1,2:2}/2", "{1:3,2:4}/2", -1, 0, "-11" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1,2:2}/2'::sparsevec, '{1:2,2:4}/2');", "sparsevec_cosine_distance", "{1:1,2:2}/2", "{1:2,2:4}/2", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1,2:2}/2'::sparsevec, '{}/2');", "sparsevec_cosine_distance", "{1:1,2:2}/2", "{}/2", -1, 0, "NaN" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1,2:1}/2'::sparsevec, '{1:1,2:1}/2');", "sparsevec_cosine_distance", "{1:1,2:1}/2", "{1:1,2:1}/2", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1}/2'::sparsevec, '{2:2}/2');", "sparsevec_cosine_distance", "{1:1}/2", "{2:2}/2", -1, 0, "1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1,2:1}/2'::sparsevec, '{1:-1,2:-1}/2');", "sparsevec_cosine_distance", "{1:1,2:1}/2", "{1:-1,2:-1}/2", -1, 0, "2" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:2}/2'::sparsevec, '{2:2}/2');", "sparsevec_cosine_distance", "{1:2}/2", "{2:2}/2", -1, 0, "1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{2:2}/2'::sparsevec, '{1:2}/2');", "sparsevec_cosine_distance", "{2:2}/2", "{1:2}/2", -1, 0, "1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1,2:2}/2'::sparsevec, '{1:3}/1');", "sparsevec_cosine_distance", "{1:1,2:2}/2", "{1:3}/1", -1, 1, "different sparsevec dimensions 2 and 1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1,2:1}/2'::sparsevec, '{1:1.1,2:1.1}/2');", "sparsevec_cosine_distance", "{1:1,2:1}/2", "{1:1.1,2:1.1}/2", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:1,2:1}/2'::sparsevec, '{1:-1.1,2:-1.1}/2');", "sparsevec_cosine_distance", "{1:1,2:1}/2", "{1:-1.1,2:-1.1}/2", -1, 0, "2" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{1:3e38}/1'::sparsevec, '{1:3e38}/1');", "sparsevec_cosine_distance", "{1:3e38}/1", "{1:3e38}/1", -1, 0, "NaN" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT cosine_distance('{}/1'::sparsevec, '{}/1');", "sparsevec_cosine_distance", "{}/1", "{}/1", -1, 0, "NaN" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT '{1:1,2:2}/2'::sparsevec <=> '{1:2,2:4}/2';", "sparsevec_cosine_distance", "{1:1,2:2}/2", "{1:2,2:4}/2", -1, 0, "0" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l1_distance('{}/2'::sparsevec, '{1:3,2:4}/2');", "sparsevec_l1_distance", "{}/2", "{1:3,2:4}/2", -1, 0, "7" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l1_distance('{}/2'::sparsevec, '{2:1}/2');", "sparsevec_l1_distance", "{}/2", "{2:1}/2", -1, 0, "1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l1_distance('{1:1,2:2}/2'::sparsevec, '{1:3}/1');", "sparsevec_l1_distance", "{1:1,2:2}/2", "{1:3}/1", -1, 1, "different sparsevec dimensions 2 and 1" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l1_distance('{1:3e38}/1'::sparsevec, '{1:-3e38}/1');", "sparsevec_l1_distance", "{1:3e38}/1", "{1:-3e38}/1", -1, 0, "Infinity" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l1_distance('{1:1,3:3,5:5,7:7}/8'::sparsevec, '{2:2,4:4,6:6,8:8}/8');", "sparsevec_l1_distance", "{1:1,3:3,5:5,7:7}/8", "{2:2,4:4,6:6,8:8}/8", -1, 0, "36" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT l1_distance('{1:1,3:3,5:5,7:7,9:9}/9'::sparsevec, '{2:2,4:4,6:6,8:8}/9');", "sparsevec_l1_distance", "{1:1,3:3,5:5,7:7,9:9}/9", "{2:2,4:4,6:6,8:8}/9", -1, 0, "45" },
    { AV_VT_SPARSEVEC, C_DIST, "SELECT '{}/2'::sparsevec <+> '{1:3,2:4}/2';", "sparsevec_l1_distance", "{}/2", "{1:3,2:4}/2", -1, 0, "7" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('111', '111');", "hamming_distance", "111", "111", -1, 0, "0" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('111', '110');", "hamming_distance", "111", "110", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('111', '100');", "hamming_distance", "111", "100", -1, 0, "2" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('111', '000');", "hamming_distance", "111", "000", -1, 0, "3" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('10101010101010101010', '01010101010101010101');", "hamming_distance", "10101010101010101010", "01010101010101010101", -1, 0, "20" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('101010101010101010101010101010101010101010101010101010101010101010101010101...", "hamming_distance", "101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", "101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", -1, 0, "0" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('101010101010101010101010101010101010101010101010101010101010101010101010101...", "hamming_distance", "101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010", -1, 0, "513" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('110000000000000000000000000000000000000000000000000000000000000000000000000...", "hamming_distance", "110000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011", "100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001", -1, 0, "2" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('', '');", "hamming_distance", "", "", -1, 0, "0" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('111', '00');", "hamming_distance", "111", "00", -1, 1, "different bit lengths 3 and 2" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('111', '000'::varbit(4));", "hamming_distance", "111", "000", -1, 0, "3" },
    { AV_VT_BIT, C_DIST, "SELECT hamming_distance('111', '0000'::varbit(4));", "hamming_distance", "111", "0000", -1, 1, "different bit lengths 3 and 4" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '1111');", "jaccard_distance", "1111", "1111", -1, 0, "0" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '1110');", "jaccard_distance", "1111", "1110", -1, 0, "0.25" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '1100');", "jaccard_distance", "1111", "1100", -1, 0, "0.5" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '1000');", "jaccard_distance", "1111", "1000", -1, 0, "0.75" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '0000');", "jaccard_distance", "1111", "0000", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1100', '1000');", "jaccard_distance", "1100", "1000", -1, 0, "0.5" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1000', '0000');", "jaccard_distance", "1000", "0000", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('0000', '0000');", "jaccard_distance", "0000", "0000", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('10101010101010101010', '01010101010101010101');", "jaccard_distance", "10101010101010101010", "01010101010101010101", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('101010101010101010101010101010101010101010101010101010101010101010101010101...", "jaccard_distance", "101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", "101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", -1, 0, "0" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('101010101010101010101010101010101010101010101010101010101010101010101010101...", "jaccard_distance", "101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('110000000000000000000000000000000000000000000000000000000000000000000000000...", "jaccard_distance", "110000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011", "100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001", -1, 0, "0.5" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('', '');", "jaccard_distance", "", "", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '000');", "jaccard_distance", "1111", "000", -1, 1, "different bit lengths 4 and 3" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '0000'::varbit(5));", "jaccard_distance", "1111", "0000", -1, 0, "1" },
    { AV_VT_BIT, C_DIST, "SELECT jaccard_distance('1111', '00000'::varbit(5));", "jaccard_distance", "1111", "00000", -1, 1, "different bit lengths 4 and 5" },
    /* handwritten: bit_in/bit_out and typmod, not in pgvector's bit.sql */
    { AV_VT_BIT, C_IN, "(handwritten) '101'::bit", "", "101", "", -1, 0, "101" },
    { AV_VT_BIT, C_IN, "(handwritten) ''::bit", "", "", "", -1, 0, "" },
    { AV_VT_BIT, C_IN, "(handwritten) 'xA5'::bit", "", "xA5", "", -1, 0, "10100101" },
    { AV_VT_BIT, C_IN, "(handwritten) '101'::bit(3)", "", "101", "", 3, 0, "101" },
    { AV_VT_BIT, C_IN, "(handwritten) '101'::bit(4)", "", "101", "", 4, 1, "bit string length 3 does not match type bit(4)" },
    { AV_VT_BIT, C_IN, "(handwritten) '102'::bit", "", "102", "", -1, 1, "\"2\" is not a valid binary digit" },
    { AV_VT_BIT, C_IN, "(handwritten) 'xG'::bit", "", "xG", "", -1, 1, "\"G\" is not a valid hexadecimal digit" },
};
/* clang-format on */

#define N_CASES (sizeof(cases) / sizeof(cases[0]))

/* Returns the reason when a case is a known difference from pgvector, else NULL. */
static const char *known_diff(const text_case *c)
{
    (void) c;
    return NULL;
}

static PGFunction find_fn(const char *name)
{
    for (size_t i = 0; i < sizeof(dist_fns) / sizeof(dist_fns[0]); i++)
        if (strcmp(dist_fns[i].name, name) == 0)
            return dist_fns[i].fn;
    return NULL;
}

/* Calls fn(a, b). Returns 0 and *res on success, 1 and the error message in msg on ERROR. */
static int call_dist(PGFunction fn, const void *a, const void *b, double *res, char *msg, size_t msgsz)
{
    MemoryContext ctx = AvContextCreate(NULL, "t_dist", 8192, 0);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    volatile int failed = 0;
    volatile double r = 0;
    PG_TRY();
    {
        r = DatumGetFloat8(DirectFunctionCall2(fn, PointerGetDatum(a), PointerGetDatum(b)));
    }
    PG_CATCH();
    {
        snprintf(msg, msgsz, "%s", av_pg_error_data()->message);
        FlushErrorState();
        failed = 1;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    *res = r;
    return failed;
}

static int float_matches(const char *expect, double got)
{
    double want = strtod(expect, NULL); /* parses NaN, Infinity, -Infinity too */
    if (isnan(want) || isnan(got))
        return isnan(want) && isnan(got);
    if (isinf(want) || isinf(got))
        return want == got;
    if (want == got)
        return 1;
    double scale = fmax(fabs(want), fabs(got));
    return fabs(want - got) <= 1e-12 * scale;
}

TEST run_case(const text_case *c)
{
    const char *why = known_diff(c);
    if (why != NULL)
        SKIPm(why);

    av_arena *ar = av_arena_create("t_text", 64 * 1024, 64u * 1024 * 1024);
    ASSERT(ar != NULL);
    av_err e;
    av_err_clear(&e);

    if (c->kind == C_IN) {
        void *v = NULL;
        av_status st = av_text_to_pgvec(c->t, c->a, c->typmod, ar, &v, &e);
        if (c->is_err) {
            if (st != AV_ERR || strcmp(e.code, "invalid_vector") != 0 || strcmp(e.msg, c->exp) != 0) {
                fprintf(stderr, "\n  want ERROR: %s\n  got  st=%d code=%s msg=%s\n", c->exp, (int) st, e.code, e.msg);
                av_arena_destroy(ar);
                FAILm("input error mismatch");
            }
        } else {
            if (st != AV_OK) {
                fprintf(stderr, "\n  unexpected error: %s\n", e.msg);
                av_arena_destroy(ar);
                FAILm("input rejected");
            }
            char *txt = NULL;
            st = av_pgvec_to_text(c->t, v, ar, &txt, &e);
            if (st != AV_OK || strcmp(txt, c->exp) != 0) {
                fprintf(stderr, "\n  want: %s\n  got:  %s\n", c->exp, st == AV_OK ? txt : e.msg);
                av_arena_destroy(ar);
                FAILm("output mismatch");
            }
        }
    } else {
        PGFunction fn = find_fn(c->fn);
        void *a = NULL, *b = NULL;
        if (fn == NULL || av_text_to_pgvec(c->t, c->a, -1, ar, &a, &e) != AV_OK ||
            av_text_to_pgvec(c->t, c->b, -1, ar, &b, &e) != AV_OK) {
            av_arena_destroy(ar);
            FAILm("test setup: unknown function or operand does not parse");
        }
        double r;
        char msg[512];
        int failed = call_dist(fn, a, b, &r, msg, sizeof(msg));
        if (c->is_err) {
            if (!failed || strcmp(msg, c->exp) != 0) {
                fprintf(stderr, "\n  want ERROR: %s\n  got  %s\n", c->exp, failed ? msg : "(a value)");
                av_arena_destroy(ar);
                FAILm("distance error mismatch");
            }
        } else if (failed || !float_matches(c->exp, r)) {
            fprintf(stderr, "\n  want: %s\n  got:  %.17g%s%s\n", c->exp, r, failed ? " ERROR " : "", failed ? msg : "");
            av_arena_destroy(ar);
            FAILm("distance mismatch");
        }
    }
    av_arena_destroy(ar);
    PASS();
}

/* Boundary and failure cases of the wrapper itself. */
TEST dimension_limits(void)
{
    static char big[16000 * 2 + 16];
    av_arena *ar = av_arena_create("t_lim", 64 * 1024, 64u * 1024 * 1024);
    av_err e;
    void *v = NULL;
    char *p = big;
    *p++ = '[';
    for (int i = 0; i < 16000; i++) {
        *p++ = '1';
        *p++ = ',';
    }
    p[-1] = ']';
    *p = '\0';
    av_err_clear(&e);
    ASSERT_EQ(AV_OK, av_text_to_pgvec(AV_VT_VECTOR, big, 16000, ar, &v, &e));
    ASSERT_EQ(AV_OK, av_text_to_pgvec(AV_VT_VECTOR, big, -1, ar, &v, &e));
    ASSERT_EQ(AV_ERR, av_text_to_pgvec(AV_VT_VECTOR, big, 15999, ar, &v, &e));
    ASSERT_STR_EQ("invalid_vector", e.code);
    /* one more element: 16001 dimensions */
    static char over[16001 * 2 + 16];
    snprintf(over, sizeof(over), "%.*s,1]", (int) strlen(big) - 1, big);
    av_err_clear(&e);
    ASSERT_EQ(AV_ERR, av_text_to_pgvec(AV_VT_VECTOR, over, -1, ar, &v, &e));
    ASSERT_STR_EQ("vector cannot have more than 16000 dimensions", e.msg);
    av_arena_destroy(ar);
    PASS();
}

TEST bad_arguments(void)
{
    av_arena *ar = av_arena_create("t_bad", 64 * 1024, 1024 * 1024);
    av_err e;
    void *v = NULL;
    char *s = NULL;
    av_err_clear(&e);
    ASSERT_EQ(AV_ERR, av_text_to_pgvec((av_vtype) 99, "[1]", -1, ar, &v, &e));
    ASSERT_STR_EQ("invalid_vector", e.code);
    ASSERT_EQ(AV_ERR, av_text_to_pgvec(AV_VT_VECTOR, NULL, -1, ar, &v, &e));
    ASSERT_EQ(AV_ERR, av_pgvec_to_text((av_vtype) 99, "x", ar, &s, &e));
    ASSERT_EQ(AV_ERR, av_pgvec_to_text(AV_VT_VECTOR, NULL, ar, &s, &e));
    /* an error must not leak state into the next call */
    ASSERT_EQ(AV_ERR, av_text_to_pgvec(AV_VT_VECTOR, "[1,", -1, ar, &v, &e));
    ASSERT_EQ(AV_OK, av_text_to_pgvec(AV_VT_VECTOR, "[1,2]", -1, ar, &v, &e));
    ASSERT_EQ(AV_OK, av_pgvec_to_text(AV_VT_VECTOR, v, ar, &s, &e));
    ASSERT_STR_EQ("[1,2]", s);
    ASSERT(av_pgvec_size(v) >= 8);
    av_arena_destroy(ar);
    PASS();
}

SUITE(text_io)
{
    av_kernels_init(); /* HalfvecInit/BitvecInit: the distance functions need their dispatch set up */
    for (size_t i = 0; i < N_CASES; i++) {
        greatest_set_test_suffix(cases[i].sql);
        RUN_TEST1(run_case, &cases[i]);
    }
    greatest_set_test_suffix(NULL);
    RUN_TEST(dimension_limits);
    RUN_TEST(bad_arguments);
}
