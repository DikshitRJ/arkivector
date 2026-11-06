/*
 * fuzz_rpc_response.c: libFuzzer target for the parsers of untrusted node responses.
 * Byte 0 selects the parser, the rest is JSON text:
 *   0: one entity object        (av_entity_from_json)
 *   1: eth_getLogs result array (av_events_parse)
 *   2: one log object           (av_event_parse)
 *   3: entity array, as in an arkiv_query "data" member
 * Parsed entities are deep-copied with av_entity_copy to exercise the canonical-bytes invariants.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "arkiv/logs.h"
#include "yyjson.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check_entity(const av_entity *e, av_arena *a)
{
    av_entity c;
    av_err err;
    if (av_entity_copy(e, a, &c, &err) != AV_OK)
        return;
    for (size_t i = 0; i < c.n_attrs; i++) {
        const av_attr *at = &c.attrs[i];
        if (at->type != AV_AT_STR && at->type != AV_AT_BYTES && at->len != 32)
            abort();
        if (at->type == AV_AT_STR && at->len > AV_ATTR_STR_MAX)
            abort();
        if (strlen(at->name) == 0 || strlen(at->name) > AV_ATTR_NAME_MAX)
            abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 512 * 1024)
        return 0;
    yyjson_doc *d = yyjson_read((const char *) data + 1, size - 1, 0);
    if (d == NULL)
        return 0;
    yyjson_val *root = yyjson_doc_get_root(d);
    av_arena *a = av_arena_create("fuzz", 64 * 1024, 64 * 1024 * 1024);
    av_err err;
    switch (data[0] % 4) {
    case 0: {
        av_entity e;
        if (av_entity_from_json(root, a, &e, &err) == AV_OK)
            check_entity(&e, a);
        break;
    }
    case 1: {
        av_event *evs;
        size_t n;
        if (av_events_parse(root, a, &evs, &n, &err) == AV_OK && n > 0 && evs[0].kind == 0)
            abort();
        break;
    }
    case 2: {
        av_event ev;
        bool known;
        (void) av_event_parse(root, &ev, &known, &err);
        break;
    }
    default: {
        size_t idx, max;
        yyjson_val *v;
        if (yyjson_is_arr(root))
            yyjson_arr_foreach(root, idx, max, v)
            {
                av_entity e;
                if (av_entity_from_json(v, a, &e, &err) == AV_OK)
                    check_entity(&e, a);
            }
        break;
    }
    }
    av_arena_destroy(a);
    yyjson_doc_free(d);
    return 0;
}
