/*
 * test_main.c: greatest runner for all unit suites. Run `av_unit -s <suite>`.
 */
#include "greatest.h"

extern SUITE(pgcompat);
extern SUITE(kernels);
extern SUITE(vectorbytes);
extern SUITE(text_io);
extern SUITE(ttl);
extern SUITE(rlp);
extern SUITE(keccak);
extern SUITE(arkiv_parse);
extern SUITE(arkiv_rpc);
extern SUITE(value);
extern SUITE(filter_parser);
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
    RUN_SUITE(text_io);
    RUN_SUITE(ttl);
    RUN_SUITE(rlp);
    RUN_SUITE(keccak);
    RUN_SUITE(arkiv_parse);
    RUN_SUITE(arkiv_rpc);
    RUN_SUITE(value);
    RUN_SUITE(filter_parser);
#ifdef AV_HAVE_HNSW_TEST
    RUN_SUITE(hnsw);
#endif
    GREATEST_MAIN_END();
}
