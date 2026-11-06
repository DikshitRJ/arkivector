/*
 * fuzz_http.c: libFuzzer target for the incremental HTTP/1.1 request parser (av_httpp_*).
 * Byte 0 picks a split size so the same input is also fed in fragments, exercising the
 * incremental state machine and pipelining.
 */
#include <stdint.h>
#include <stdlib.h>

#include "server/http.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 256 * 1024) {
        return 0;
    }
    size_t chunk = (size_t) (data[0] % 64) + 1;
    const char *p = (const char *) data + 1;
    size_t left = size - 1;
    av_httpp *hp = av_httpp_create(64 * 1024);
    if (hp == NULL) {
        return 0;
    }
    int requests = 0;
    while (left > 0 && requests < 64) {
        size_t n = left < chunk ? left : chunk;
        size_t consumed = 0;
        av_httpp_result r = av_httpp_feed(hp, p, n, &consumed);
        if (consumed > n) {
            abort();
        }
        if (r == AV_HTTPP_ERROR) {
            av_err e;
            av_err_clear(&e);
            int status = av_httpp_error(hp, &e);
            if (status < 400 || status > 599) {
                abort();
            }
            break;
        }
        if (r == AV_HTTPP_REQUEST) {
            const av_http_req *req = av_httpp_request(hp);
            if (req == NULL) {
                abort();
            }
            (void) av_httpp_keep_alive(hp);
            (void) av_httpp_take_continue(hp);
            av_httpp_next(hp);
            requests++;
        } else if (consumed != n) {
            abort(); /* NEED_MORE must consume everything */
        }
        p += consumed;
        left -= consumed;
        if (r == AV_HTTPP_NEED_MORE && consumed == 0) {
            break;
        }
    }
    av_httpp_destroy(hp);
    return 0;
}
