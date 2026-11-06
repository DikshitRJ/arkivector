# daemon/testkit/mock-arkiv/query_lang.py

## Purpose

Implements the Arkiv query language for `arkiv_query`: the lexer, the parser, the semantic checks, the evaluator, and the result shaping.

## Key functions

| Function | Purpose |
| :--- | :--- |
| `lex(q)` | Split the query into tokens. Supports comments starting with `--`. |
| `Parser` | Build the AST. Enforces nesting depth and the number of conditions. |
| `parse_query(q, limits)` | Check the byte length, then parse. |
| `evaluate(node, entity)` | Test one entity. Only indexed attributes are visible to predicates. |
| `run_query(chain, params, limits)` | Run `arkiv_query`. Sort, page and project the result. |
| `get_entity(chain, params)` | Run `arkiv_getEntity`. Returns a live entity or null. |

Error codes follow the Arkiv error-handling rules: `-32001` parse, `-32002` type, `-32003` literal, `-32004` limits, `-32602` invalid params.

## Ownership and thread safety

Pure over the chain snapshot. Single-threaded.

## Testing

`selftest.py`. Full coverage of the query language is a D3 task.

## Notes

- Default order is creation order. Ties under `ORDER BY` use `$key` ascending.
- `dec` values are scaled by 1e18, and compared as integers.
