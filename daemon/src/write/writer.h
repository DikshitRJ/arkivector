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
#include "write/fees.h"
#include "write/nonce.h"
#include "yyjson.h"

typedef enum { AV_WAIT_NONE = 0, AV_WAIT_RECEIPT = 1, AV_WAIT_INDEXED = 2 } av_write_wait_mode;

typedef struct {
    uint64_t chain_id;
    uint8_t contract_addr[20];
    av_batcher_config batcher;
    uint64_t gas_limit;                /* fallback base gas when eth_estimateGas fails (default 60000) */
    uint64_t max_fee_per_gas;          /* static fee, last resort of the fee chain (default 20 gwei) */
    uint64_t max_priority_fee_per_gas; /* static tip, last resort of the fee chain (default 1.5 gwei) */
    uint64_t max_fee_cap;              /* wei, 0 = no cap (write.max_fee_cap_gwei) */
    double priority_fee_percentile;    /* eth_feeHistory percentile (write.priority_fee_percentile, 50) */
    uint32_t fee_history_blocks;       /* eth_feeHistory block count (write.fee_history_blocks, 20) */
    uint32_t gas_margin_pct;           /* margin over eth_estimateGas (write.gas_margin_pct, 20) */
    uint32_t replace_after_blocks;     /* resend a stuck tx after this many blocks (0 = default 10) */
    uint32_t max_replacements;         /* resends per transaction (default 3; set no_replace to disable) */
    bool no_replace;
} av_writer_config;

/*
 * Concurrency: one writer is shared by every HTTP worker. Entity-nonce reservation, transaction
 * nonce allocation, signing and eth_sendRawTransaction run under the writer's submit mutex, so the
 * order of entity keys predicted for concurrent requests matches the order their transactions take
 * on chain. Receipt waits run outside the mutex.
 *
 * Entity keys (GitHub issue #13): creates are keyed by the signer's on-chain entityNonce (eth_call
 * entityNonce(address) at "pending"), advanced locally per create. The local floor covers this
 * daemon's own transactions still in flight; the chain value covers creates made elsewhere by the
 * same wallet. With wait "receipt"/"indexed" the keys come from the EntityCreated logs of the
 * receipts, so they are exact even when the prediction was not.
 */

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
    /* Fee suggestion (CCR-0001): the same quote the managed writer would use right now. */
    uint64_t max_fee_per_gas;
    uint64_t max_priority_fee_per_gas;
    const char *fee_source; /* "fee_history" | "gas_price" | "static" */
} av_prepare_result;

av_status av_writer_prepare(av_writer *w, av_collection *col, yyjson_val *entities_arr,
                            const uint8_t from_addr[20], av_arena *arena, av_prepare_result *out, av_err *e);

/*
 * Submit transactions signed outside the daemon (CCR-0001, GitHub issue #12):
 * POST /v1/collections/{name}/entities:submit.
 *
 * Exactly one of raw (signed EIP-1559 transactions, hex) or hashes (transactions a wallet already
 * broadcast, e.g. MetaMask eth_sendTransaction) is given. Raw transactions are decoded and checked
 * (chain id, `to` = the Arkiv contract, value 0, execute(...) calldata, sender is a writer of the
 * collection) before anything is broadcast, then sent in order with eth_sendRawTransaction. With
 * wait receipt/indexed the daemon polls the receipts (no fee replacement: it cannot re-sign) and
 * returns the keys of the EntityCreated logs; indexed also waits for the collection to publish the
 * highest receipt block. No signer is needed.
 */
typedef struct {
    char (*tx_hashes)[67];
    size_t n_tx_hashes;
    uint64_t block_number;
    bool has_block_number;
    uint8_t (*created_keys)[32];
    size_t n_created_keys;
} av_submit_result;

av_status av_writer_submit(av_writer *w, av_collection *col, const char *const *raw_hex,
                           const size_t *raw_hex_len, size_t n_raw, const char *const *hashes,
                           size_t n_hashes, av_write_wait_mode wait_mode, int timeout_ms, av_arena *arena,
                           av_submit_result *out, av_err *e);

#endif /* AV_WRITE_WRITER_H */
