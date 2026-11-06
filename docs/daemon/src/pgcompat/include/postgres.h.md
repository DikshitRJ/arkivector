# daemon/src/pgcompat/include/postgres.h

## Purpose

Stub. The primary include of copied PostgreSQL code. It provides `Datum` and the `*GetDatum` and `DatumGet*` macros, and includes the elog, palloc and pgcompat type headers.

## Key symbols

`Datum`, `PointerGetDatum`, `DatumGetPointer`, `Int32GetDatum`, `Float8GetDatum`, `DatumGetFloat8` and the other conversions.

## Ownership and thread safety

Header only. No state.

## Notes

Listed in `src/pgcompat/STUBS.md` as a stub. Other stub headers are summarised there.
