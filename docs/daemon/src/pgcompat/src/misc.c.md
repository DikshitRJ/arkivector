# daemon/src/pgcompat/src/misc.c

## Purpose

Small helpers from PostgreSQL's `port.h`, `float.c` and `miscadmin.h`.

## Functions

| Function | Behavior |
| :--- | :--- |
| `av_pg_qsort_arg` | `qsort_r` with a context argument. |
| `pg_strong_random` | Fill a buffer from `/dev/urandom`. Returns false on error. |
| `copyObjectImpl`, `equal` | Raise `ERROR`. Node copying and comparison are not supported. |
| `pg_ltoa` | Format an `int32` with `snprintf`. |
| `float_overflow_error`, `float_underflow_error`, `float_zero_divide_error` | Raise the PostgreSQL error with the same text. |

## Ownership and thread safety

No shared state. `pg_strong_random` opens and closes its own file descriptor.

## Testing

Indirect. The unit suites exercise `pg_ltoa` through sparsevec output, and the float errors through the text tests.

## Notes

- The `copyObjectImpl` and `equal` stubs make it an error to use `List` of nodes. pgvector does not do that.
