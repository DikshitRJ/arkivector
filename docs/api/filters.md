# Filters

Search, scroll, count, and the filter-based delete/extend endpoints take **at most one** of:
- `filter`: a JSON AST (recommended for SDKs, since builders produce it);
- `where`: a string in the **Arkiv query language** (the same grammar as `arkiv_query`, predicate part only, no `ORDER BY`).

Both compile to the same internal AST and have identical semantics. Filters are evaluated **locally** by the daemon on mirrored attributes, including attributes that are **not indexed on chain**.

## 1. Fields

| Field | Meaning | Type |
| :--- | :--- | :--- |
| `<name>` | User attribute | Per value |
| `$owner`, `$creator` | System fields | `addr` |
| `$expiresAt` | System field | `u64` |
| `$key` | System field | `key` |

Liveness (`$expiresAt > as_of_block`, not deleted) is **always** applied implicitly.

## 2. JSON AST

Each node is an object with **exactly one** operator key:

| Node | Shape | Example |
| :--- | :--- | :--- |
| and | `{"and": [Filter, …]}` (≥ 1) | `{"and": [{"eq": {"category": "finance"}}, {"gte": {"score": {"type": "dec", "value": "4.0"}}}]}` |
| or | `{"or": [Filter, …]}` (≥ 1) | |
| not | `{"not": Filter}` | |
| eq / ne / lt / lte / gt / gte | `{"<op>": {"<field>": Value}}` | `{"lt": {"rank": 100}}` |
| between | `{"between": {"<field>": [low, high]}}` (inclusive) | `{"between": {"rank": [-100, 100]}}` |
| startswith | `{"startswith": {"<field>": "prefix"}}` (str only) | `{"startswith": {"region": "eu-"}}` |
| exists | `{"exists": "<field>"}` | `{"exists": "archived"}` |
| typeof | `{"typeof": {"<field>": "<AttrType>"}}` | `{"typeof": {"score": "dec"}}` |

**Value typing.** A `Value` is a `TypedValue` (`{"type": "dec", "value": "4.0"}`) or a shorthand scalar. Shorthand is typed by:
1. the collection `attributes` schema entry for that field, else
2. the system-field type, else
3. JSON type: string → `str`, boolean → `bool`, integer → `i32`.

A shorthand that can't be converted to the schema type (e.g. `"abc"` for a `dec` field) → `422 invalid_filter`. **SDK builders SHOULD always emit TypedValues** when they know the type, which avoids ambiguity.

Limits: depth ≤ 16, ≤ 256 leaf predicates. Exceeding them → `invalid_filter`.

## 3. Comparison semantics (identical to Arkiv)

- A predicate on `(field, type)` only matches entities where the field **exists with that type**. An entity storing `score` as `str` never matches `{"gte": {"score": {"type": "dec", "value": "4"}}}`.
- `ne` matches entities where the field exists with the same type **and** differs. Entities without the field do **not** match `ne` (use `not` + `eq` for that).
- Numbers compare by magnitude (`dec` exactly, as fixed-point 10⁻¹⁸). `str` compares lexicographically by UTF-8 bytes. `false < true`. `addr`/`key` compare as unsigned big-endian bytes.
- `startswith` is valid only for `str`, and only with a string literal.
- `bytes` attributes cannot be filtered (`invalid_filter`).

## 4. `where` grammar (summary)

```
expr      := or_expr
or_expr   := and_expr ( "OR" and_expr )*
and_expr  := unary ( "AND" unary )*
unary     := "NOT" unary | "(" expr ")" | predicate
predicate := field op literal
           | field "BETWEEN" literal "AND" literal
           | field "STARTSWITH" str_literal
           | "EXISTS" "(" field ")"
           | "TYPEOF" "(" field ")" "=" type_name
op        := "=" | "!=" | "<" | "<=" | ">" | ">="
field     := attr_name | "$owner" | "$creator" | "$expiresAt" | "$key"
literal   := "true" | "false" | integer              (bare integer = i32)
           | type_name "(" raw ")"                   e.g. dec(4.5) str('it''s') addr(0x…) key(0x…) u64(1) u256(1) i32(-5)
comment   := "--" … end of line
```

Keywords are case-insensitive. `AND` binds tighter than `OR`. String literals use single quotes, with `''` as the escape. Errors return `invalid_filter` with `position` (byte offset).

## 5. Equivalence examples

| `where` | `filter` |
| :--- | :--- |
| `category = str('finance')` | `{"eq": {"category": {"type": "str", "value": "finance"}}}` |
| `score >= dec(4.0) AND published = true` | `{"and": [{"gte": {"score": {"type": "dec", "value": "4.0"}}}, {"eq": {"published": {"type": "bool", "value": true}}}]}` |
| `rank BETWEEN -100 AND 100` | `{"between": {"rank": [{"type": "i32", "value": -100}, {"type": "i32", "value": 100}]}}` |
| `NOT EXISTS(archived) OR region STARTSWITH str('eu-')` | `{"or": [{"not": {"exists": "archived"}}, {"startswith": {"region": "eu-"}}]}` |
| `$creator = addr(0x1111111111111111111111111111111111111111)` | `{"eq": {"$creator": {"type": "addr", "value": "0x1111111111111111111111111111111111111111"}}}` |

The demo-dataset queries in [`spec/fixtures/demo-dataset.json`](../../spec/fixtures/demo-dataset.json) use both forms and list `expected_count_matching_filter`.
