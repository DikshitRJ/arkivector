/*
 * test_main.c: greatest runner for all unit suites. Run `av_unit -s <suite>`.
 */
#include "greatest.h"

extern SUITE(pgcompat);
extern SUITE(kernels);
extern SUITE(vectorbytes);
#ifdef AV_HAVE_HNSW_TEST
extern SUITE(hnsw);
#endif

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(pgcompat);
    RUN_SUITE(kernels);
    RUN_SUITE(vectorbytes);
#ifdef AV_HAVE_HNSW_TEST
    RUN_SUITE(hnsw);
#endif
    GREATEST_MAIN_END();
}
