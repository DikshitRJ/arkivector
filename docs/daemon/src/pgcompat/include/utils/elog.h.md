# daemon/src/pgcompat/include/utils/elog.h

## Purpose

Stub. The error-reporting API that copied code uses: `ereport`, `elog`, `errcode`, `errmsg`, `errdetail`, `errhint`, `PG_TRY`, `PG_CATCH`, `PG_FINALLY`, `PG_END_TRY`, and the `ErrorData` struct.

## Key behavior

`PG_TRY` saves `PG_exception_stack` with `sigsetjmp(buf, 0)` semantics. Errors jump back to it. TSan can follow this on glibc 2.44.

## Ownership and thread safety

Macros and declarations. The error state lives in `elog.c`.

## Testing

`test/unit/test_pgcompat.c`.
