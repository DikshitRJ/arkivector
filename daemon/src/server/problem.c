/*
 * server/problem.c: av_err to RFC 9457 problem+json. See problem.h.
 */
#include "server/problem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "av_error_table.h"
#include "yyjson.h"

static const av_error_info *lookup(const char *code)
{
    if (code == NULL || code[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(av_error_table) / sizeof(av_error_table[0]); i++) {
        if (strcmp(av_error_table[i].code, code) == 0) {
            return &av_error_table[i];
        }
    }
    return NULL;
}

int av_problem_status(const char *code)
{
    const av_error_info *i = lookup(code);
    return i != NULL ? i->status : 500;
}

bool av_problem_known(const char *code)
{
    return lookup(code) != NULL;
}

size_t av_problem_table_size(void)
{
    return sizeof(av_error_table) / sizeof(av_error_table[0]);
}

const char *av_problem_table_code(size_t i)
{
    return i < av_problem_table_size() ? av_error_table[i].code : NULL;
}

static void fallback_body(av_http_resp *resp)
{
    static const char k_body[] = "{\"type\":\"urn:arkivector:error:internal\",\"title\":\"Internal\","
                                 "\"status\":500,\"code\":\"internal\","
                                 "\"retryable\":false}";
    resp->status = 500;
    resp->content_type = "application/problem+json";
    resp->body = malloc(sizeof(k_body));
    if (resp->body != NULL) {
        memcpy(resp->body, k_body, sizeof(k_body) - 1);
        resp->body_len = sizeof(k_body) - 1;
    } else {
        resp->body_len = 0;
    }
}

void av_problem_from_err(const av_err *e, av_http_resp *resp, const char *instance)
{
    const av_error_info *info = e != NULL ? lookup(e->code) : NULL;
    bool known = info != NULL;
    if (!known) {
        info = lookup("internal");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        fallback_body(resp);
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    char type[96];
    snprintf(type, sizeof(type), "%s%s", AV_ERROR_TYPE_PREFIX, info->code);
    yyjson_mut_obj_add_strcpy(doc, root, "type", type);
    yyjson_mut_obj_add_str(doc, root, "title", info->title);
    yyjson_mut_obj_add_int(doc, root, "status", info->status);
    const char *detail = (e != NULL && e->msg[0] != '\0') ? e->msg : NULL;
    if (!known && e != NULL && e->code[0] != '\0') {
        /* A code outside the contract is a daemon bug: report as internal, keep the text for the operator. */
        detail = e->msg[0] != '\0' ? e->msg : "unmapped error code";
    }
    if (detail != NULL) {
        yyjson_mut_obj_add_strcpy(doc, root, "detail", detail);
    }
    if (instance != NULL && instance[0] != '\0') {
        yyjson_mut_obj_add_strcpy(doc, root, "instance", instance);
    }
    yyjson_mut_obj_add_str(doc, root, "code", info->code);
    yyjson_mut_obj_add_bool(doc, root, "retryable", info->retryable != 0);
    if (resp->request_id != NULL && resp->request_id[0] != '\0') {
        yyjson_mut_obj_add_strcpy(doc, root, "request_id", resp->request_id);
    }
    if (known && e != NULL) {
        if (e->field[0] != '\0') {
            yyjson_mut_obj_add_strcpy(doc, root, "field", e->field);
        }
        if (e->position >= 0) {
            yyjson_mut_obj_add_int(doc, root, "position", e->position);
        }
        if (e->vb_error[0] != '\0') {
            yyjson_mut_obj_add_strcpy(doc, root, "vb_error", e->vb_error);
        }
        if (e->as_of_block != 0) {
            yyjson_mut_obj_add_uint(doc, root, "as_of_block", e->as_of_block);
        }
    }
    size_t len = 0;
    char *json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (json == NULL) {
        fallback_body(resp);
        return;
    }
    free(resp->body);
    resp->body = json;
    resp->body_len = len;
    resp->status = info->status;
    resp->content_type = "application/problem+json";
    if ((info->status == 429 || info->status == 503) && resp->retry_after == NULL) {
        resp->retry_after = "1";
    }
    if (known && e != NULL && e->as_of_block != 0) {
        resp->as_of_block = e->as_of_block;
        resp->has_as_of = true;
    }
}
