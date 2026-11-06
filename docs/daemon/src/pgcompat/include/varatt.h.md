# daemon/src/pgcompat/include/varatt.h

## Purpose

Stub for the varlena header. Provides `VARSIZE`, `SET_VARSIZE`, `VARDATA` and `VARHDRSZ`. Only the 4-byte header is used. Values are never TOASTed.

## Ownership and thread safety

Macros only.
