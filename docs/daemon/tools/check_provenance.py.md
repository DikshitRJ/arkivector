# daemon/tools/check_provenance.py

## Purpose

Checks that copied pgvector and PostgreSQL code is unchanged, except for the `ARKIVECTOR` deltas recorded in `PROVENANCE.md` and `STUBS.md`.

## Usage

```
python3 -I daemon/tools/check_provenance.py [--pgvector pgvector/src] [--postgres <pg checkout>]
```

Exit status 1 when any check fails.

## Checks

| Mode | Check |
| :---: | :--- |
| V | Verbatim files are byte-identical to upstream. The table `VERBATIM` lists the pairs. |
| F | `pgvector_copy.py --check` passes. |
| S | `strip_arkivector()` removes the `ARKIVECTOR` regions. The rest equals upstream. Applies to `hnsw.h` and `hnswutils.c`. |
| S | Each function in `hnswbuild_mem.c` matches the same function in upstream `hnswbuild.c`. |
| PG | With `--postgres`, checks `varbit_io.c` functions and the verbatim PostgreSQL files in `pgcompat`. |

## Key functions

- `strip_arkivector` handles `#ifndef ARKIVECTOR`, `#ifdef ARKIVECTOR`, `#else` and `#endif`. It keeps the upstream side.
- `find_func` returns one function's lines, or None.

## Ownership and thread safety

Build-time check. Single-threaded.

## Testing

Run in CI. Its own output is the test result.
