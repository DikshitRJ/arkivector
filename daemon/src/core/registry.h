/*
 * core/registry.h: name -> collection map. Definitions are persisted in LMDB
 * (av_store_put_collection) and every stored collection is loaded at startup.
 *
 * Thread safety: all functions are thread-safe (internal rwlock). av_registry_get returns a
 * retained collection: the caller must av_collection_release it. A collection removed with
 * av_registry_remove stays valid for holders until they release it; its segment file is
 * deleted with the last reference.
 */
#ifndef AV_CORE_REGISTRY_H
#define AV_CORE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "core/collection.h"

typedef struct av_registry av_registry;

/* Loads all persisted collections (rebuilding their in-memory state). data_dir may be NULL. */
av_registry *av_registry_create(av_store *store, const char *data_dir, av_err *e);
void av_registry_destroy(av_registry *r);

/* Persist def, then build the collection. collection_exists when the name is taken. *out is retained. */
av_status av_registry_add(av_registry *r, const av_coldef *def, av_collection **out, av_err *e);
/* NULL when unknown (no error). The result is retained. */
av_collection *av_registry_get(av_registry *r, const char *name);
/* collection_not_found when unknown. Deletes the stored definition, rows and databases. */
av_status av_registry_remove(av_registry *r, const char *name, av_err *e);

size_t av_registry_count(av_registry *r);
/* Visit collections sorted by name; each is retained for the duration of the call only. false stops. */
typedef bool (*av_registry_cb)(av_collection *c, void *ctx);
void av_registry_each(av_registry *r, av_registry_cb cb, void *ctx);

#endif /* AV_CORE_REGISTRY_H */
