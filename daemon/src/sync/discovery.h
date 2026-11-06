/*
 * sync/discovery.h: Entity discovery strategies for curated and open collections.
 *
 * Strategies per ARCHITECTURE §9.5:
 * - Strategy A: Writer-scoped log filter (topic2 in writers).
 * - Strategy B: Calldata decoding of execute((uint8,bytes)[]) from block transactions.
 * - Strategy C: Batched membership probe via arkiv_query.
 */
#ifndef AV_SYNC_DISCOVERY_H
#define AV_SYNC_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/rpc.h"
#include "core/collection.h"
#include "log.h"

typedef struct av_discovery av_discovery;

av_discovery *av_discovery_create(av_rpc *rpc);
void av_discovery_destroy(av_discovery *d);

/*
 * Record a key as "not mine" for a given collection with an expiration block,
 * avoiding repeated membership queries until it expires.
 */
void av_discovery_cache_not_mine(av_discovery *d, const char *coll_name, const uint8_t key[32],
                                uint64_t expires_at);

/*
 * Check if a key is known to be "not mine" in the cache.
 */
bool av_discovery_is_not_mine(av_discovery *d, const char *coll_name, const uint8_t key[32],
                             uint64_t current_block);

/*
 * Filter candidate keys down to those that belong to the collection.
 * For open collections, executes Strategy B (calldata decode) with fallback
 * to Strategy C (membership probe query).
 */
av_status av_discovery_filter_keys(av_discovery *d, const av_coldef *coldef, uint64_t block_num,
                                   const uint8_t (*cand_keys)[32], size_t n_cand,
                                   uint8_t (*out_keys)[32], size_t *n_out, size_t max_out,
                                   av_err *e);

#endif /* AV_SYNC_DISCOVERY_H */
