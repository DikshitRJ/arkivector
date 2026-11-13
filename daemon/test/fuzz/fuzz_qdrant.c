/*
 * fuzz_qdrant.c: libFuzzer target for the Qdrant JSON mapping (server/qdrant_map.c), which reads
 * untrusted request bodies of the /qdrant endpoints. Byte 0 selects the function, the rest is JSON:
 *   0: PUT .../points body -> REST entities (av_qdrant_points_to_entities)
 *   1: Qdrant filter -> JSON filter AST (av_qdrant_filter_to_json), then compiled (av_filter_from_json)
 *   2: {"attrs": TypedValue map, "doc": string, "with_payload": selector} -> payload (av_qdrant_payload)
 *   3: point id (av_qdrant_point_id) and its round trip through av_qdrant_id_json
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "filter/json_filter.h"
#include "server/qdrant_map.h"
#include "yyjson.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 256 * 1024)
        return 0;
    yyjson_doc *d = yyjson_read((const char *) data + 1, size - 1, 0);
    if (d == NULL)
        return 0;
    yyjson_val *root = yyjson_doc_get_root(d);
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    av_err e;
    av_err_clear(&e);
    switch (data[0] % 4) {
    case 0: {
        yyjson_mut_val *arr = NULL;
        size_t dropped = 0;
        if (av_qdrant_points_to_entities(root, md, &arr, &dropped, &e) == AV_OK) {
            if (arr == NULL || !yyjson_mut_is_arr(arr))
                abort();
            size_t len = 0;
            char *js = yyjson_mut_val_write(arr, 0, &len);
            free(js);
        } else if (e.code[0] == '\0') {
            abort(); /* every failure names an error code */
        }
        break;
    }
    case 1: {
        yyjson_mut_val *f = NULL;
        if (av_qdrant_filter_to_json(root, md, &f, &e) == AV_OK && f != NULL) {
            yyjson_doc *fd = yyjson_mut_val_imut_copy(f, NULL);
            if (fd != NULL) {
                av_arena *a = av_arena_create("fuzz", 64 * 1024, 64 * 1024 * 1024);
                av_filter *ast = NULL;
                av_err fe;
                av_err_clear(&fe);
                (void) av_filter_from_json(yyjson_doc_get_root(fd), NULL, a, &ast, &fe);
                av_arena_destroy(a);
                yyjson_doc_free(fd);
            }
        }
        break;
    }
    case 2: {
        yyjson_val *attrs = yyjson_obj_get(root, "attrs");
        yyjson_val *doc = yyjson_obj_get(root, "doc");
        yyjson_mut_val *ma = attrs != NULL ? yyjson_val_mut_copy(md, attrs) : NULL;
        yyjson_mut_val *p = av_qdrant_payload(md, ma, yyjson_get_str(doc), yyjson_get_len(doc),
                                              yyjson_obj_get(root, "with_payload"));
        if (p == NULL || !yyjson_mut_is_obj(p))
            abort();
        (void) av_qdrant_wants_payload(yyjson_obj_get(root, "with_payload"));
        break;
    }
    default: {
        char id[AV_QDRANT_ID_MAX];
        if (av_qdrant_point_id(root, id, &e) == AV_OK) {
            if (memchr(id, '\0', sizeof id) == NULL)
                abort();
            yyjson_mut_val *v = av_qdrant_id_json(md, id);
            if (v == NULL)
                abort();
        }
        break;
    }
    }
    yyjson_mut_doc_free(md);
    yyjson_doc_free(d);
    return 0;
}
