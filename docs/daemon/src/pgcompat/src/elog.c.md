# daemon/src/pgcompat/src/elog.c

## Purpose

Implements `ereport`, `elog` and the `PG_TRY` unwinding that copied pgvector code uses. An error becomes a `longjmp` to the nearest `PG_TRY`, which the caller catches.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `errstart` | Start a report. Below `ERROR`, return false when the level is under `log_min_messages`. Abort on a nested report. |
| `errcode`, `errmsg`, `errdetail`, `errhint`, `errposition` | Fill the current report. |
| `errfinish` | Log a non-error report through the log hook, or stderr. `FATAL` and above call `abort()`. `ERROR` stores the report and calls `pg_re_throw`. |
| `pg_re_throw` | `_longjmp` to `PG_exception_stack`. Abort when no `PG_TRY` is active. |
| `av_pg_error_data` | Return the last thrown error. |
| `FlushErrorState` | Clear the last error and the nesting depth. |
| `av_pg_set_log_hook` | Install a hook for messages below `ERROR`. |

## Ownership and thread safety

- Error state is per thread (`__thread`). One report is active per thread.
- `pg_re_throw` does not release locks. The `PG_CATCH` macro restores the saved lock state through `av_pg_unwind_restore`.

## Testing

`test/unit/test_pgcompat.c`: `nested_pg_try_catches_error`, `warning_does_not_jump`, `error_releases_lwlocks`, `catch_releases_only_inner_locks`.

## Notes

- `ereport` is not reentrant. A second report started inside a report aborts the process.
