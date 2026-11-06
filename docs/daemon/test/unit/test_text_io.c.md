# daemon/test/unit/test_text_io.c

## Purpose

Table-driven port of the literal, output and distance cases from pgvector's regression tests (`vector_type`, `halfvec`, `sparsevec`, `bit`).

## Table

Each row is one case. The fields are the value type, the kind (`C_IN` for input and output, `C_DIST` for a distance function), the SQL statement, the function name, the literals, the typmod, the expected error flag, and the expected text. Rows marked "handwritten" were not taken from pgvector.

Skipped on purpose: array casts, arithmetic, aggregates, comparison operators, and typmod errors such as `vector(0)`.

## Matching rules

- Distances match with a relative difference of 1e-12, or exactly for `NaN` and `Infinity`.
- Error messages must match the upstream text.

## Testing

Run with `av_unit -s text_io`, or `ctest -R unit_text_io`. The D1 result is 248 cases with no differences (see `PROVENANCE.md`).

## Notes

- The file is about 530 lines. This document covers the header and the table design. The remaining rows follow the same format.
