# daemon/src/pgcompat/src/fmgr.c

## Purpose

Implements the function-manager calls that copied code uses to call SQL functions from C.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `DirectFunctionCall1Coll`, `DirectFunctionCall2Coll`, `DirectFunctionCall3Coll` | Build a `fcinfo` with 1 to 3 arguments, call the function, return the result. |
| `FunctionCall0Coll` to `FunctionCall3Coll` | Same, using the function in an `FmgrInfo`. |

If the function sets `isnull`, the call raises `ERROR`.

## Ownership and thread safety

Each call builds its own `fcinfo` on the stack. Reentrant.

## Testing

Covered by the kernel and text tests, which call pgvector functions through `DirectFunctionCall2` and `DirectFunctionCall3`.

## Notes

- Only up to three arguments are supported.
