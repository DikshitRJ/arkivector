# daemon/src/pgcompat/src/list.c

## Purpose

Verbatim copy of PostgreSQL REL_17_11 `src/backend/nodes/list.c`. It provides `List` (`lappend`, `lfirst`, `foreach`) that HNSW code uses.

## Mode

V (verbatim). Built with upstream flags. It must match the upstream file byte for byte. `daemon/tools/check_provenance.py` checks this.

## Testing

`test/unit/test_pgcompat.c`, test `list_works`.

## Notes

- Upstream file. Do not edit. Changes go through the sync tool, not by hand.
