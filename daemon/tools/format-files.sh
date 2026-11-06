#!/usr/bin/env bash
# List the C files that clang-format applies to (our code only).
# Excludes copied pgvector code, verbatim PostgreSQL files and vendored libraries.
set -euo pipefail
cd "$(dirname "$0")/.."
VERBATIM_PG='src/pgcompat/(src/(list|pairingheap|pg_prng|pg_bitutils|f2s)\.c|include/(nodes/pg_list|lib/(pairingheap|simplehash|stringinfo)|utils/(relptr|varbit)|common/(pg_prng|int|hashfn|shortest_dec|digit_table|ryu_common)|port/pg_bitutils)\.h)$'
git ls-files 'src/*.c' 'src/*.h' 'test/*.c' 'test/*.h' 'bench/*.c' \
  | grep -v '^src/pgvector/' \
  | grep -Ev "$VERBATIM_PG" \
  | while read -r f; do [ -s "$f" ] && echo "$f"; done
