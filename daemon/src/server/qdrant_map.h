/*
 * server/qdrant_map.h: Qdrant REST shapes <-> Arkivector entities, filters and payloads
 * (docs/api/qdrant.md, GitHub issue #15). Pure functions, no I/O: the /qdrant handlers in
 * qdrant.c call them, and fuzz_qdrant feeds them untrusted JSON.
 *
 * Point -> entity (upsert):
 *   id       u64 -> av.id decimal string; UUID (hyphenated or 32 hex) -> lower-case hyphenated
 *   vector   number array -> "vector" (named / sparse vectors -> not_implemented)
 *   payload  "page_content" (else "text") string -> "document"; scalars -> attributes:
 *            string -> str, bool -> bool, integer -> i32 if it fits else u64 (negative: dec),
 *            float -> dec; null -> skipped. Everything else (nested JSON, strings over 128
 *            bytes, keys that are not Arkiv attribute names) -> "document" as a JSON object
 *            when no text key exists, else dropped (counted in *n_dropped for a Warning header).
 * Entity -> payload (search): attributes as plain JSON values (i32/u64 numbers, dec numbers,
 *   str/bool as is, other types as their TypedValue string), plus the document: a JSON object
 *   document is merged back key by key, any other document is returned as "page_content".
 * Qdrant filter -> JSON filter AST (filters.md): must -> and, should -> or, must_not ->
 *   not(or(...)), match.value -> eq, match.any -> or of eq, match.except -> not(or of eq),
 *   range -> comparisons (i32/u64/dec branches, so ints and floats both match), is_empty /
 *   is_null -> not exists, has_id -> av.id in. Dotted keys, match.text, geo, values_count,
 *   nested and min_should -> not_implemented.
 *
 * Ownership: every produced value belongs to the given yyjson_mut_doc.
 * Errors: av_err codes invalid_argument / invalid_filter / not_implemented / bad_request.
 */
#ifndef AV_SERVER_QDRANT_MAP_H
#define AV_SERVER_QDRANT_MAP_H

#include <stdbool.h>
#include <stddef.h>

#include "log.h"
#include "yyjson.h"

#define AV_QDRANT_ID_MAX 64

/* Qdrant point id (u64 or UUID) -> canonical av.id string. */
av_status av_qdrant_point_id(yyjson_val *id, char out[AV_QDRANT_ID_MAX], av_err *e);

/* av.id (or entity key hex when the entity has none) -> Qdrant id JSON: an integer when the id
 * is a decimal u64 without leading zeros, else the string. */
yyjson_mut_val *av_qdrant_id_json(yyjson_mut_doc *doc, const char *id);

/* PUT .../points body ({"points":[...]} or {"batch":{ids,vectors,payloads}}) -> array of REST
 * EntityInput objects. *n_dropped counts payload fields that could not be kept. */
av_status av_qdrant_points_to_entities(yyjson_val *body, yyjson_mut_doc *doc, yyjson_mut_val **out_arr,
                                       size_t *n_dropped, av_err *e);

/* Qdrant Filter object -> JSON filter AST (NULL *out for an empty filter). */
av_status av_qdrant_filter_to_json(yyjson_val *qf, yyjson_mut_doc *doc, yyjson_mut_val **out, av_err *e);

/* with_payload selector: NULL/false -> no payload; true -> all; ["a","b"] -> those keys;
 * {"include":[...]} / {"exclude":[...]}. Returns false when no payload is wanted. */
bool av_qdrant_wants_payload(yyjson_val *with_payload);

/* Build the payload object of one hit from its TypedValue attribute map (may be NULL) and the
 * document (may be NULL), filtered by the with_payload selector. */
yyjson_mut_val *av_qdrant_payload(yyjson_mut_doc *doc, yyjson_mut_val *attrs_obj, const char *document,
                                  size_t doc_len, yyjson_val *with_payload);

#endif /* AV_SERVER_QDRANT_MAP_H */
