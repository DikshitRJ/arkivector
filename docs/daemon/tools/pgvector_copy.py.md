# daemon/tools/pgvector_copy.py

## Purpose

Regenerates the function-level copies (mode F) in `daemon/src/pgvector/` from an upstream pgvector tree. Each function body is byte-identical to upstream.

## Usage

```
python3 -I daemon/tools/pgvector_copy.py [--src pgvector/src] [--dst daemon/src/pgvector] [--check]
```

`--check` exits 1 when a destination file differs from the generated text. CI uses this to prove the copies are unmodified.

## Key parts

- `TAG` and `COMMIT` identify the upstream release (v0.8.7).
- `MANIFEST` maps each destination file to its upstream file, its includes, and its items. An item is a `func` (one function, with its leading comment) or a `block` (a number of lines from a regex match).
- `generate()` builds the file text: a provenance header, the includes, the items.
- `find_func()` locates a function by its definition line. It stops at the closing brace. It exits when the name is missing or not unique.

## Ownership and thread safety

A build-time script. It writes only the files named in `MANIFEST`. It is not thread-related.

## Testing

Run with `--check` in CI. `daemon/tools/check_provenance.py` also calls it.

## Notes

- Do not edit the generated files by hand. Edit the manifest, or update the upstream tag with `pgvector-sync.sh`.
