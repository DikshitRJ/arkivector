# PROVENANCE.md: code copied from pgvector

Upstream: <https://github.com/pgvector/pgvector>, tag **v0.8.7**, commit `f37c13f68b57d2c3472b2214fbcff699d6d34876`.
Licence: PostgreSQL License, see [`LICENSE.pgvector`](LICENSE.pgvector).
Reuse modes (ARCHITECTURE §5.1): **V** verbatim · **F** function-level copy · **S** copy + shim (`#ifndef ARKIVECTOR` guards and `/* ARKIVECTOR: … */` lines only) · **P** algorithm port.

Files in this directory build with pgvector's own flags (`-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -ffp-contract=fast`), never with our strict warning set. CI checks that **V** files are byte-identical to `pgvector/src/`.

| File | Upstream path | Mode | Functions taken | `ARKIVECTOR:` deltas | Phase |
| :--- | :--- | :---: | :--- | :--- | :---: |
| `LICENSE.pgvector` | `LICENSE` | V | — | none | D0 |
| `halfutils.c` | `src/halfutils.c` | V | whole file | none | D0 |
| `halfutils.h` | `src/halfutils.h` | V | whole file | none | D0 |
| `bitutils.c` | `src/bitutils.c` | V | whole file | none | D0 |
| `bitutils.h` | `src/bitutils.h` | V | whole file | none | D0 |
| `halfvec.h` | `src/halfvec.h` | V | whole file | none | D0 |
| `vector.h` | `src/vector.h` | V | whole file | none | D0 |
| `bitvec.h` | `src/bitvec.h` | V | whole file | none | D0 |
| `sparsevec.h` | `src/sparsevec.h` | V | whole file | none | D0 |

## Known upstream quirks

- `halfutils.h:93` (`HalfToFloat4`, portable path): `(bin & 0x8000) << 16` shifts a promoted `int`, which UBSan reports as `shift-base`. The value is correct on gcc/clang. `cmake/Sanitizers.cmake` disables only that check.
