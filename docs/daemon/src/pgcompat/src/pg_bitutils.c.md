# daemon/src/pgcompat/src/pg_bitutils.c

## Purpose

Verbatim copy of PostgreSQL REL_17_11 `src/port/pg_bitutils.c`. It gives the popcount and next-power-of-two helpers.

## Mode

V (verbatim). Built with upstream flags. The portable popcount path is used, because `HAVE_X86_64_POPCNTQ` is not defined.

## Notes

- Upstream file. Do not edit.
