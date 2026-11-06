# daemon/src/pgcompat/src/pg_prng.c

## Purpose

Verbatim copy of PostgreSQL REL_17_11 `src/common/pg_prng.c`. The xoroshiro128** generator. The HNSW code uses it through `RandomDouble()`.

## Mode

V (verbatim). Upstream flags. Checked byte for byte by `check_provenance.py`.

## Notes

- The HNSW delta gives each inserter thread its own state. The process-global state is not used with threads (D0 spike finding).
- Upstream file. Do not edit.
