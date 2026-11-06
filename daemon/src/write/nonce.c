/*
 * write/nonce.c: Local nonce tracker with chain resync via RPC.
 */
#include "write/nonce.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "yyjson.h"

struct av_nonce_tracker {
    uint8_t address[20];
    av_rpc *rpc;
    pthread_mutex_t mu;
    uint64_t tracked_nonce;
    bool initialized;
};

av_nonce_tracker *av_nonce_tracker_create(const uint8_t address[20], av_rpc *rpc)
{
    av_nonce_tracker *nt = calloc(1, sizeof(*nt));
    if (nt == NULL)
        return NULL;
    if (address != NULL)
        memcpy(nt->address, address, 20);
    nt->rpc = rpc;
    pthread_mutex_init(&nt->mu, NULL);
    nt->tracked_nonce = 0;
    nt->initialized = false;
    return nt;
}

void av_nonce_tracker_destroy(av_nonce_tracker *nt)
{
    if (nt == NULL)
        return;
    pthread_mutex_destroy(&nt->mu);
    free(nt);
}

av_status av_nonce_tracker_resync(av_nonce_tracker *nt, av_err *e)
{
    if (nt == NULL) {
        av_err_set(e, "invalid_argument", "nonce tracker is NULL");
        return AV_ERR;
    }

    pthread_mutex_lock(&nt->mu);
    if (nt->rpc == NULL) {
        nt->initialized = true;
        pthread_mutex_unlock(&nt->mu);
        return AV_OK;
    }

    char addr_hex[43];
    av_addr_to_hex(nt->address, addr_hex);

    char params_json[128];
    snprintf(params_json, sizeof(params_json), "[\"%s\", \"pending\"]", addr_hex);

    yyjson_doc *resp_doc = NULL;
    yyjson_val *res = NULL;
    int rpc_code = 0;
    av_status st =
        av_rpc_call(nt->rpc, "eth_getTransactionCount", params_json, &resp_doc, &res, &rpc_code, e);
    if (st != AV_OK || resp_doc == NULL) {
        pthread_mutex_unlock(&nt->mu);
        return AV_ERR;
    }

    const char *hex_nonce = yyjson_get_str(res);
    if (hex_nonce != NULL) {
        uint64_t val = 0;
        if (av_qty_parse(hex_nonce, &val)) {
            nt->tracked_nonce = val;
            nt->initialized = true;
        }
    }
    yyjson_doc_free(resp_doc);
    pthread_mutex_unlock(&nt->mu);
    return AV_OK;
}

av_status av_nonce_tracker_next(av_nonce_tracker *nt, uint64_t *out_nonce, av_err *e)
{
    if (nt == NULL || out_nonce == NULL) {
        av_err_set(e, "invalid_argument", "invalid arguments to av_nonce_tracker_next");
        return AV_ERR;
    }

    pthread_mutex_lock(&nt->mu);
    if (!nt->initialized) {
        pthread_mutex_unlock(&nt->mu);
        if (av_nonce_tracker_resync(nt, e) != AV_OK)
            return AV_ERR;
        pthread_mutex_lock(&nt->mu);
    }

    *out_nonce = nt->tracked_nonce++;
    pthread_mutex_unlock(&nt->mu);
    return AV_OK;
}

uint64_t av_nonce_tracker_current(const av_nonce_tracker *nt)
{
    if (nt == NULL)
        return 0;
    av_nonce_tracker *mut_nt = (av_nonce_tracker *) nt;
    pthread_mutex_lock(&mut_nt->mu);
    uint64_t n = mut_nt->tracked_nonce;
    pthread_mutex_unlock(&mut_nt->mu);
    return n;
}
