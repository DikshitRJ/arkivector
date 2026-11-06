# daemon/test/unit/test_pgcompat.c

## Purpose

Tests the PostgreSQL API shims: `palloc` contexts, reset and delete, `List`, `ereport` with `PG_TRY`, nested catch, warnings, and the lightweight lock behavior.

## Suite `pgcompat`

| Test | Checks |
| :--- | :--- |
| `palloc_in_context` | Allocation in a context, zeroing, 16-byte alignment, `repalloc`. |
| `reset_frees_everything` | `MemoryContextReset` frees children and their memory. |
| `list_works` | 1000 `lappend_int` calls, then iteration. |
| `nested_pg_try_catches_error` | Inner and outer `PG_TRY` both catch the error. The error fields are kept. |
| `warning_does_not_jump` | `elog(WARNING)` and `ereport(NOTICE)` do not jump. |
| `error_releases_lwlocks` | An error inside a held lock releases the lock. |
| `lwlock_shared_and_readonly` | Shared holders, and read-only mode leaves the lock word at zero. |
| `oom_in_capped_context` | A capped context raises `ERRCODE_OUT_OF_MEMORY`. |
| `catch_releases_only_inner_locks` | Only the locks taken inside the frame are released. |
| `release_after_flag_change` | A lock taken before read-only mode is released for real. |

## Testing

Run with `av_unit -s pgcompat`, or `ctest -R unit_pgcompat`.
