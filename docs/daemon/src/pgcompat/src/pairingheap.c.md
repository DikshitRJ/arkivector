# daemon/src/pgcompat/src/pairingheap.c

## Purpose

Verbatim copy of PostgreSQL REL_17_11 `src/backend/lib/pairingheap.c`. HNSW code uses it for the candidate heaps.

## Mode

V (verbatim). Upstream flags. Checked byte for byte by `check_provenance.py`.

## Testing

Indirect, through the HNSW spike (`test/unit/test_hnsw.c`).

## Notes

- Upstream file. Do not edit.
