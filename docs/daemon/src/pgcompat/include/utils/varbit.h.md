# daemon/src/pgcompat/include/utils/varbit.h

## Purpose

Subset (mode S) of PostgreSQL's `varbit.h`. Defines `VarBit`, `VARBITLEN`, `VARBITS`, `VARBITBYTES` and related macros for `bit` vectors.

## Ownership and thread safety

Header only.

## Testing

`test/unit/test_kernels.c` and `test/unit/test_vectorbytes.c` build bit values with these macros.
