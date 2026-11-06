/*
 * filter/value_json.h: render a stored attribute as an API TypedValue JSON object.
 *
 * Encoding (spec/openapi.yaml TypedValue): bool -> JSON boolean, i32 -> JSON integer,
 * u64/u256/dec -> decimal string, str -> string, addr/key -> 0x hex, bytes -> base64.
 * Undecodable stored bytes render a zero value of the type (never fail the response).
 *
 * Ownership: the returned value belongs to `doc`; scratch memory (base64) comes from `arena`.
 * Thread-safety: none needed (operates on caller-owned objects).
 */
#ifndef AV_FILTER_VALUE_JSON_H
#define AV_FILTER_VALUE_JSON_H

#include "arkiv/entity.h"
#include "core/arena.h"
#include "yyjson.h"

yyjson_mut_val *av_attr_to_typed_json(yyjson_mut_doc *doc, const av_attr *at, av_arena *arena);

#endif
