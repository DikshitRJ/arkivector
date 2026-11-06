# daemon/src/pgcompat/src/f2s.c

## Purpose

Verbatim copy of PostgreSQL REL_17_11 `src/common/f2s.c`. It prints floats with the shortest decimal form that reads back exactly (Ryu). The vector `*_out` functions use it.

## Mode

V (verbatim). Upstream flags. The headers `digit_table.h` and `ryu_common.h` are also verbatim.

## Testing

`test/unit/test_text_io.c` checks the output text of vectors and halfvecs.

## Notes

- Upstream file. Do not edit.
