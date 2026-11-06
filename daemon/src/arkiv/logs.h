/*
 * arkiv/logs.h: Arkiv contract events decoded from eth_getLogs / eth_subscribe("logs") entries.
 *
 * Layout per RESEARCH/arkiv-docs/03-evm-mutations-and-contract-spec/events-and-logs.md §3.
 * Ownership: events returned by av_events_parse live in the caller's arena.
 * Thread-safe (the topic0 table is initialised once with pthread_once).
 */
#ifndef AV_ARKIV_LOGS_H
#define AV_ARKIV_LOGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "log.h"

struct yyjson_val;

typedef enum {
    AV_EV_CREATED = 1,
    AV_EV_PATCHED,
    AV_EV_DELETED,
    AV_EV_EXTENDED,
    AV_EV_TRANSFERRED
} av_event_kind;

typedef struct av_event {
    av_event_kind kind;
    uint8_t key[32];
    uint8_t owner[20];      /* Created/Patched/Deleted/Extended */
    uint8_t prev_owner[20]; /* Transferred */
    uint8_t new_owner[20];  /* Transferred */
    uint64_t expires_at;    /* Created/Extended */
    uint32_t flags;         /* Created: creationFlags */
    uint64_t block;
    uint32_t log_index;
    uint8_t block_hash[32];
    uint8_t tx_hash[32];
    bool removed;
} av_event;

#define AV_EV_KINDS 5

/* 32-byte topic0 of an event kind (computed with keccak at first use and asserted equal to
   the documented constants; aborts on mismatch). NULL for an invalid kind. */
const uint8_t *av_event_topic0(av_event_kind k);

/* Decode one log object. *known = false (and AV_OK) when topic0 is not one of the five events.
   A known event with a malformed layout (topic count, data length, hex) is an AV_ERR
   ("arkiv_rpc_error"). Untrusted input: fuzzed by fuzz_rpc_response. */
av_status av_event_parse(struct yyjson_val *log, av_event *out, bool *known, av_err *e);

/* Decode an eth_getLogs result array into arena a; unknown topic0 entries are skipped. */
av_status av_events_parse(struct yyjson_val *arr, av_arena *a, av_event **evs, size_t *n, av_err *e);

#endif /* AV_ARKIV_LOGS_H */
