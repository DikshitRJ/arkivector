/*
 * server/mcp.h: Model Context Protocol (MCP) Streamable HTTP endpoint (POST /mcp).
 */
#ifndef AV_SERVER_MCP_H
#define AV_SERVER_MCP_H

#include <stdbool.h>
#include "server/router.h"

void av_rest_register_mcp(av_router *r, bool debug_routes, void *ctx);

#endif /* AV_SERVER_MCP_H */
