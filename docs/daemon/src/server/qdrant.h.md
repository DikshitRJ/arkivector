# `daemon/src/server/qdrant.h`

Declares registration hooks for the Qdrant REST API compatibility layer.

## Interfaces
- `av_rest_register_qdrant(av_router *r, bool debug_routes, void *ctx)`: Registers all `/qdrant/*` compatibility endpoints.
