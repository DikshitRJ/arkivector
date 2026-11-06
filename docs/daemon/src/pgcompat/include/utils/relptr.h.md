# daemon/src/pgcompat/include/utils/relptr.h

## Purpose

Verbatim PostgreSQL REL_17_11 `src/include/utils/relptr.h`. Position-independent pointers (`relptr_*`). HNSW arenas store their links as offsets, so that a mapped arena works at any address.

## Mode

V (verbatim). Checked byte for byte by `check_provenance.py`.

## Testing

The D0 spike maps an arena at a second address and checks identical results (`test/unit/test_hnsw.c`).
