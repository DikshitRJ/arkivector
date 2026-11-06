# daemon/test/fuzz/fuzz_vectorbytes.c

## Purpose

libFuzzer target for the VectorBytes parser. It checks the parser on random bytes.

## Property

- Validate and decode must agree. Both succeed, or both fail.
- An accepted blob must match its decoded type, dimension, `nnz` and flags.
- An accepted blob must re-encode to the same bytes.
- No crash and no out-of-bounds read.

## Testing

Build with the `fuzz` preset. Run the binary with a corpus directory.
