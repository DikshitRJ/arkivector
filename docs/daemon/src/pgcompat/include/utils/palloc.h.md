# daemon/src/pgcompat/include/utils/palloc.h

## Purpose

Stub. Declares the `palloc` family, `pfree`, `repalloc`, `pstrdup`, `psprintf`, `add_size` and `mul_size`, and the `MCXT_ALLOC_*` flags. Implemented in `pgcompat/src/palloc.c`.

## Ownership and thread safety

Declarations only. Allocations come from the current memory context of the calling thread.
