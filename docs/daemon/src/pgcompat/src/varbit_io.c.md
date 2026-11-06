# daemon/src/pgcompat/src/varbit_io.c

## Purpose

Function-level copy (mode F) of PostgreSQL REL_17_11 `src/backend/utils/adt/varbit.c`. It contains `bit_in`, `varbit_out` and `bit_out`, plus the `HEXDIG` and padding helpers they use.

## Mode

F (PostgreSQL function copy). The functions are byte-identical to upstream. `check_provenance.py` checks them when it is given a PostgreSQL checkout.

## Key functions

| Function | Purpose |
| :--- | :--- |
| `bit_in` | Parse a bit string literal. Used by the text codec for `bit`. |
| `bit_out`, `varbit_out` | Format a bit string. |

## Testing

`test/unit/test_text_io.c` checks bit literals and output.
