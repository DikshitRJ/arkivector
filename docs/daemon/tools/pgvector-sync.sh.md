# daemon/tools/pgvector-sync.sh

## Purpose

Brings a new pgvector release tag into `daemon/src/pgvector/`. It does not commit anything.

## Usage

```
daemon/tools/pgvector-sync.sh <new_tag>   # for example v0.8.8
```

## Steps

1. Clone upstream pgvector into a scratch directory. Add worktrees for the old tag (`v0.8.7`) and the new tag.
2. For each verbatim or copy-plus-shim file, run a three-way merge with `git merge-file`. The inputs are our file, the old upstream file and the new upstream file. Conflicts stay in the file, and the script lists them.
3. Regenerate the function-level copies with `pgvector_copy.py --src <new tag src>`. The script fails when a listed function is missing.
4. Print a diff of `hnswbuild.c` for a manual check.
5. Print the next steps: update the tag in the tools and in `PROVENANCE.md`, build, run the unit tests, run `check_provenance.py`, run the parity suite.

## Ownership and thread safety

Developer tool. It needs network access to GitHub. It writes only under `daemon/src/pgvector/`. Nothing is committed.

## Testing

Run it on a branch. The check steps are `check_provenance.py` and the parity suite.

## Notes

- The script uses `TMPDIR` for the scratch directory. It prints the scratch path on exit.
