#!/usr/bin/env bash
# pgvector-sync.sh <new_tag>: carry upstream pgvector changes into daemon/src/pgvector.
#
# 1. Clones pgvector (old tag v0.8.7 and <new_tag>) into a scratch dir.
# 2. Verbatim (V) and copy+shim (S) files: 3-way merge (ours, old upstream, new
#    upstream) with `git merge-file`; conflicts are left in place and listed.
# 3. Function-level (F) files: regenerated from the new tag with pgvector_copy.py
#    (fails loudly if a listed function disappeared or changed shape).
# 4. Prints the next steps: rebuild, unit tests, check_provenance.py, parity suite.
# Nothing is committed; review `git diff` afterwards.
set -euo pipefail
NEW_TAG="${1:?usage: pgvector-sync.sh <new_tag>  (e.g. v0.8.8)}"
OLD_TAG="v0.8.7"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DST="$ROOT/daemon/src/pgvector"
SCRATCH="${TMPDIR:-/tmp}/pgvector-sync.$$"
mkdir -p "$SCRATCH"
trap 'echo "scratch kept at $SCRATCH"' EXIT

git clone --quiet https://github.com/pgvector/pgvector "$SCRATCH/pgvector"
git -C "$SCRATCH/pgvector" worktree add --quiet "$SCRATCH/old" "$OLD_TAG"
git -C "$SCRATCH/pgvector" worktree add --quiet "$SCRATCH/new" "$NEW_TAG"

conflicts=()
merge_one() { # local upstream
    local ours="$DST/$1" base="$SCRATCH/old/src/$2" theirs="$SCRATCH/new/src/$2"
    if [ ! -f "$theirs" ]; then
        echo "MISSING upstream file in $NEW_TAG: src/$2"; conflicts+=("$1"); return
    fi
    if ! git merge-file -L ours -L "$OLD_TAG" -L "$NEW_TAG" "$ours" "$base" "$theirs"; then
        conflicts+=("$1")
    fi
}
for f in halfutils.c halfutils.h bitutils.c bitutils.h halfvec.h vector.h bitvec.h sparsevec.h hnsw.h hnswutils.c; do
    merge_one "$f" "$f"
done
merge_one bitvec_kernels.c bitvec.c

echo "== regenerating function-level copies from $NEW_TAG"
python3 -I "$ROOT/daemon/tools/pgvector_copy.py" --src "$SCRATCH/new/src"

echo "== hnswbuild_mem.c: compare copied functions by hand against $NEW_TAG src/hnswbuild.c"
diff <(git -C "$SCRATCH/pgvector" show "$OLD_TAG:src/hnswbuild.c") "$SCRATCH/new/src/hnswbuild.c" || true

if [ ${#conflicts[@]} -gt 0 ]; then
    echo "CONFLICTS (resolve markers): ${conflicts[*]}"
fi
cat <<MSG
Next:
  1. Update TAG/COMMIT in tools/pgvector_copy.py and PROVENANCE.md, VERBATIM refs in check_provenance.py
  2. cmake --build --preset dev && ctest --preset dev
  3. python3 -I daemon/tools/check_provenance.py --pgvector $SCRATCH/new/src
  4. docker compose -f daemon/test/parity/docker-compose.yml up -d && pytest -q daemon/test/parity
MSG
