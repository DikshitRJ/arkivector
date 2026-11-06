# daemon/test/fuzz/fuzz_vector_in.c

## Purpose

libFuzzer target for the pgvector text parsers, through `av_text_to_pgvec`.

## Input format

Byte 0 selects the type (`1 + byte % 4`). The rest is the literal. The code copies it and adds a NUL. Inputs over 256 KiB are skipped.

## Property

- An accepted literal formats with `av_pgvec_to_text` without error.
- The formatted text re-parses without error.
- The two structs are byte-identical.

## Testing

Build with the `fuzz` preset.
