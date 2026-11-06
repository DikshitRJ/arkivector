# daemon/src/log.h

## Purpose

Defines the status codes, the error object `av_err`, and the leveled logger. Every module returns `av_status` and fills an `av_err` when it fails.

## Types

| Type | Meaning |
| :--- | :--- |
| `av_status` | `AV_OK` (0) or `AV_ERR` (1). |
| `av_err` | `code` (from `spec/error-codes.json`), `vb_error` (a `VB_*` sub-code), `field` (JSON pointer), `position` (byte offset, -1 when unset), `as_of_block`, `msg`. |
| `av_log_level` | `AV_LOG_DEBUG`, `AV_LOG_INFO`, `AV_LOG_WARN`, `AV_LOG_ERROR`. |

## Public API

| Function | Purpose |
| :--- | :--- |
| `void av_err_clear(av_err *e)` | Reset to "no error". NULL is allowed. |
| `void av_err_set(av_err *e, const char *code, const char *fmt, ...)` | Set the code and a printf-style message. NULL is allowed. |
| `void av_err_set_vb(av_err *e, const char *vb_code, const char *fmt, ...)` | Set code `invalid_vector` and a `VB_*` sub-code. |
| `void av_err_field(av_err *e, const char *field)` | Set the JSON pointer of the offending field. |
| `void av_log(av_log_level level, const char *module, const char *fmt, ...)` | Write one log line. |
| `void av_vlog(av_log_level level, const char *module, const char *fmt, va_list ap)` | Same, with a `va_list`. |
| `void av_log_set_level(av_log_level level)` | Set the minimum level. |
| `bool av_log_enabled(av_log_level level)` | True when the level is at or above the minimum. |

## Ownership and thread safety

- The caller owns each `av_err`. The functions write into it.
- The minimum log level is a plain global. `av_log_set_level` must run before threads start.

## Testing

Indirect. The codec and kernel tests check `e.code` and `e.vb_error` for every rejected input.

## Notes

- Logging is minimal until D2. Output goes to stderr as `level=... module=... msg="..."`.
- Secret redaction is not implemented yet. D2 adds it.
