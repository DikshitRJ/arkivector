/*
 * write/writer.h: Managed upsert-by-id writer, delete and extend operations, and unsigned prepare.
 */
#ifndef AV_WRITE_WRITER_H
#define AV_WRITE_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/abi.h"
#include "arkiv/rpc.h"
#include "arkiv/signer.h"
#include "core/collection.h"
#include "core/registry.h"
#include "log.h"
#include "store/lmdb_store.h"
#include "write/batcher.h"
#include "write/nonce.h"
#include "yyjson.h"

typedef enum { AV_WAIT_NONE = 0, AV_WAIT_RECEIPT = 1, AV_WAIT_INDEXED = 2 } av_write_wait_mode;

typedef struct {
    uint64_t chain_id;
    uint8_t contract_addr[20];
    av_batcher_config batcher;
    uint64_t gas_limit;
    uint64_t max_fee_per_gas;
    uint64_t max_priority_fee_per_gas;
} av_writer_config;

typedef struct av_writer av_writer;

av_writer *av_writer_create(const av_writer_config *cfg, av_signer *signer, av_rpc *rpc, av_store *store,
                            av_registry *reg, av_err *e);
void av_writer_destroy(av_writer *w);

const av_signer *av_writer_get_signer(const av_writer *w);
const uint8_t *av_writer_get_address(const av_writer *w);

/* Managed upsert result */
typedef struct {
    char id[128];
    uint8_t key[32];
    const char *op; /* "create", "patch", "noop" */
} av_upsert_item_result;

typedef struct {
    av_upsert_item_result *results;
    size_t n_results;
    char (*tx_hashes)[67];
    size_t n_tx_hashes;
    uint64_t block_number;
    bool has_block_number;
} av_upsert_batch_result;

/* Parse TTL helper */
bool av_writer_parse_ttl(yyjson_val *ttl_val, uint64_t *expires_at, uint64_t *min_lifetime, av_err *e);

/* Managed upsert */
av_status av_writer_upsert(av_writer *w, av_collection *col, yyjson_val *entities_arr,
                           av_write_wait_mode wait_mode, int timeout_ms, av_arena *arena,
                           av_upsert_batch_result *out, av_err *e);

/* Tx result for delete / extend */
typedef struct {
    size_t affected;
    char (*tx_hashes)[67];
    size_t n_tx_hashes;
    uint64_t block_number;
} av_tx_response;

/* Delete operations */
av_status av_writer_delete_refs(av_writer *w, av_collection *col, const char *const *refs, size_t n_refs,
                                av_arena *arena, av_tx_response *out, av_err *e);
av_status av_writer_delete_filter(av_writer *w, av_collection *col, const roaring_bitmap_t *matching,
                                  av_arena *arena, av_tx_response *out, av_err *e);

/* Extend operations */
av_status av_writer_extend_refs(av_writer *w, av_collection *col, const char *const *refs, size_t n_refs,
                                uint64_t expires_at, uint64_t min_lifetime, av_arena *arena,
                                av_tx_response *out, av_err *e);
av_status av_writer_extend_filter(av_writer *w, av_collection *col, const roaring_bitmap_t *matching,
                                  uint64_t expires_at, uint64_t min_lifetime, av_arena *arena,
                                  av_tx_response *out, av_err *e);

/* Unsigned prepare */
typedef struct {
    char to[43];
    char *data;     /* hex calldata "0x49650044..." */
    char value[10]; /* "0x0" */
    uint64_t nonce;
    uint64_t gas_estimate;
} av_prepared_tx;

typedef struct {
    uint64_t chain_id;
    av_prepared_tx *transactions;
    size_t n_transactions;
    av_upsert_item_result *results;
    size_t n_results;
} av_prepare_result;

av_status av_writer_prepare(av_writer *w, av_collection *col, yyjson_val *entities_arr,
                            const uint8_t from_addr[20], av_arena *arena, av_prepare_result *out, av_err *e);

#endif /* AV_WRITE_WRITER_H */
