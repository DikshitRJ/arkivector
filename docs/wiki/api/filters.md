# Filters

Search, scroll, count and the filter-based delete and extend endpoints accept **at most one** of:

- `filter`: a JSON tree (easiest to build in code; the SDKs have builders);
- `where`: a string in the **Arkiv query language** (the predicate part of `arkiv_query`).

Both compile to the same tree and behave identically. Filters are evaluated by the daemon on its local copy of the attributes, including attributes that are not indexed on chain. Liveness (not deleted, not expired at `as_of_block`) is always applied on top.

## Fields

| Field | Meaning | Type |
| :--- | :--- | :--- |
| `<name>` | Your attribute | per value |
| `$owner`, `$creator` | Owner and creator address | `addr` |
| `$expiresAt` | Expiry block | `u64` |
| `$key` | Entity key | `key` |

## JSON tree

Every node has exactly one operator key:

| Node | Shape | Example |
| :--- | :--- | :--- |
| and / or | `{"and": [Filter, …]}` | `{"and": [{"eq": {"topic": "pets"}}, {"gte": {"stars": 4}}]}` |
| not | `{"not": Filter}` | `{"not": {"exists": "archived"}}` |
| eq, ne, lt, lte, gt, gte | `{"<op>": {"<field>": Value}}` | `{"lt": {"stars": 3}}` |
| between | `{"between": {"<field>": [low, high]}}` (inclusive) | `{"between": {"stars": [2, 4]}}` |
| startswith | `{"startswith": {"<field>": "prefix"}}` (`str` only) | `{"startswith": {"region": "eu-"}}` |
| exists | `{"exists": "<field>"}` | `{"exists": "archived"}` |
| typeof | `{"typeof": {"<field>": "<type>"}}` | `{"typeof": {"score": "dec"}}` |

A `Value` is a plain JSON value, typed from the collection's declared `attributes` (else: string → `str`, boolean → `bool`, integer → `i32`), or an explicit typed value `{"type": "dec", "value": "4.5"}`. Large integers (`u64`, `u256`) and decimals are strings.

## `where` language

```
expr      := expr OR expr | expr AND expr | NOT expr | ( expr ) | predicate
predicate := field op literal
           | field BETWEEN literal AND literal
           | field STARTSWITH str('prefix')
           | EXISTS(field)
           | TYPEOF(field) = type
op        := =  !=  <  <=  >  >=
literal   := true | false | integer (i32) | type(raw)    e.g. str('pets') dec(4.5) u64(1) addr(0x…)
```

Keywords are case-insensitive, `AND` binds tighter than `OR`, `--` starts a comment. **Strings must use the typed form `str('…')`**; a bare `'pets'` is an error. Escape a quote by doubling it: `str('it''s')`.

## Equivalents

| `where` | `filter` |
| :--- | :--- |
| `topic = str('pets')` | `{"eq": {"topic": "pets"}}` |
| `stars >= 4 AND published = true` | `{"and": [{"gte": {"stars": 4}}, {"eq": {"published": true}}]}` |
| `stars BETWEEN 2 AND 4` | `{"between": {"stars": [2, 4]}}` |
| `NOT EXISTS(archived) OR region STARTSWITH str('eu-')` | `{"or": [{"not": {"exists": "archived"}}, {"startswith": {"region": "eu-"}}]}` |
| `score >= dec(4.0)` | `{"gte": {"score": {"type": "dec", "value": "4.0"}}}` |
| `$creator = addr(0x1111111111111111111111111111111111111111)` | `{"eq": {"$creator": {"type": "addr", "value": "0x1111111111111111111111111111111111111111"}}}` |

## Comparison rules (same as Arkiv)

- A predicate on a field only matches entities where the field exists **with that type**. An entity that stores `score` as `str` never matches a `dec` comparison.
- `ne` matches entities that have the field with a different value; entities without the field don't match (use `not` + `eq` for that).
- Numbers compare by value, strings by UTF-8 bytes, `false < true`, addresses and keys as unsigned big-endian bytes.
- `bytes` attributes cannot be filtered.

Limits: `where` ≤ 8192 bytes, depth ≤ 16, ≤ 256 leaf predicates. Errors are `422 invalid_filter` with `position` (the byte offset in `where`).
