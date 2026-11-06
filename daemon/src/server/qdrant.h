/*
 * server/qdrant.h: Qdrant REST API v1.x compatibility endpoints (/qdrant).
 */
#ifndef AV_SERVER_QDRANT_H
#define AV_SERVER_QDRANT_H

#include <stdbool.h>

#include "server/router.h"

void av_rest_register_qdrant(av_router *r, bool debug_routes, void *ctx);

#endif /* AV_SERVER_QDRANT_H */
