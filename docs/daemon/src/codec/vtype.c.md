# daemon/src/codec/vtype.c

## Purpose

Implements the type names and the dimension helper from `vtype.h`.

## Behavior

- `av_vtype_name` maps each enum value to its name.
- `av_vtype_parse` compares the input against the four names with `strcmp`.
- `av_pgvec_dim` reads `dim` from `Vector`, `HalfVector` or `SparseVector`, and `bit_len` from `VarBit`.

## Ownership and thread safety

No state. Reentrant. The struct reads assume a valid pointer of the matching type.

## Testing

Used by the VectorBytes golden tests (type names) and by the text limit check (`within_limits` in `text.c`).
