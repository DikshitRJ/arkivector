# daemon/src/codec/vtype.h

## Purpose

Defines the four pgvector value types. The numbers match the VectorBytes type nibble.

## Types and functions

| Name | Meaning |
| :--- | :--- |
| `av_vtype` | `AV_VT_VECTOR` (1), `AV_VT_HALFVEC` (2), `AV_VT_BIT` (3), `AV_VT_SPARSEVEC` (4). |
| `const char *av_vtype_name(av_vtype t)` | `"vector"`, `"halfvec"`, `"bit"`, `"sparsevec"`, or `"?"`. |
| `bool av_vtype_parse(const char *s, av_vtype *out)` | Parse a name. Exact, case-sensitive match. |
| `int av_pgvec_dim(av_vtype t, const void *v)` | Dimension of a pgvector struct. Bit length for bit. Returns -1 for an unknown type. |

## Ownership and thread safety

Pure functions over constants and the struct header. Reentrant.

## Testing

Used by every codec and kernel test. Not tested alone.
