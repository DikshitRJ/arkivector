/*
 * core/coldef.h: collection definition (cross-lane contract, D2/D3).
 * Mirrors spec/openapi.yaml CollectionCreate; parsed/serialized by core/coldef.c.
 * The JSON form (av_coldef_to_json) is what the registry persists in LMDB.
 */
#ifndef AV_CORE_COLDEF_H
#define AV_CORE_COLDEF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/entity.h"
#include "codec/vtype.h"
#include "kernels/opclass.h"
#include "log.h"

#define AV_COLL_NAME_MAX    63
#define AV_SCHEMA_TAG_MAX   128
#define AV_MAX_WRITERS      6500
#define AV_MAX_SCHEMA_ATTRS 256

typedef enum { AV_MEMBERSHIP_CREATOR = 0, AV_MEMBERSHIP_OWNER = 1, AV_MEMBERSHIP_OPEN = 2 } av_membership;
typedef enum { AV_INDEX_HNSW = 0, AV_INDEX_NONE = 1 } av_index_method;
typedef enum { AV_MODE_FULL = 0, AV_MODE_LITE = 1 } av_coll_mode;

typedef struct {
    char name[AV_ATTR_NAME_MAX + 1];
    av_attr_type type;
} av_schema_attr;

typedef enum { AV_TTL_NONE = 0, AV_TTL_BLOCKS, AV_TTL_SECONDS, AV_TTL_DAYS, AV_TTL_EXPIRES_AT } av_ttl_kind;
typedef struct {
    av_ttl_kind kind;
    double value; /* blocks / seconds / days / absolute block */
} av_col_ttl;

typedef struct {
    char name[AV_COLL_NAME_MAX + 1];
    char schema_tag[AV_SCHEMA_TAG_MAX + 1];
    av_vtype vtype;
    int64_t dimensions;
    av_index_method method;
    const av_opclass *opclass;
    int m;               /* default 16 */
    int ef_construction; /* default 64 */
    uint8_t (*writers)[20];
    size_t n_writers;
    av_membership membership;
    av_schema_attr *attrs;
    size_t n_attrs;
    av_col_ttl default_ttl;
    bool publish_manifest;
    char model[129];
    av_coll_mode mode;
    bool has_manifest_key;
    uint8_t manifest_key[32];
} av_coldef;

/*
 * Parse and validate a CollectionCreate JSON object (yyjson_val *). Unknown fields ->
 * bad_request; out-of-range values -> invalid_argument with e->field set to the JSON
 * pointer. Validation per docs/api/collections.md: opclass type matches column type,
 * ef_construction >= 2*m, open requires no writers, name pattern, dims limits.
 * Memory for writers/attrs is allocated in arena a.
 */
struct yyjson_val;
av_status av_coldef_from_json(struct yyjson_val *obj, av_arena *a, av_coldef *out, av_err *e);

/*
 * Like av_coldef_from_json; internal != false additionally accepts the daemon-internal
 * "manifest_key" field (written by av_coldef_to_json so a persisted definition round-trips).
 */
av_status av_coldef_from_json_ex(struct yyjson_val *obj, av_arena *a, bool internal, av_coldef *out, av_err *e);

/* Parse JSON text (bad_request on a syntax error), then av_coldef_from_json_ex. */
av_status av_coldef_from_text(const char *json, size_t len, av_arena *a, bool internal, av_coldef *out, av_err *e);

/* Deep copy with malloc'd writers/attrs (so it outlives the parse arena). Release with av_coldef_release. */
av_status av_coldef_clone(const av_coldef *src, av_coldef *dst, av_err *e);
void av_coldef_release(av_coldef *d); /* frees what av_coldef_clone allocated */

/* Serialize to a JSON object string (malloc'd, caller frees). */
char *av_coldef_to_json(const av_coldef *d, size_t *len);

/* Schema type of an attribute name (AV_AT_TOMBSTONE when not in the schema). */
av_attr_type av_coldef_attr_type(const av_coldef *d, const char *name);

bool av_coldef_has_writer(const av_coldef *d, const uint8_t addr[20]);

#endif /* AV_CORE_COLDEF_H */
