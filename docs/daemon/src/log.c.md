# daemon/src/log.c

## Purpose

Implements the `av_err` helpers and a minimal stderr logger. See `log.h`.

## Behavior

- `av_err_clear` zeroes the object and sets `position` to -1.
- `av_err_set` clears the object, copies the code (truncated to 39 characters), and formats the message (truncated to 511 characters).
- `av_err_set_vb` always sets the code to `invalid_vector`.
- `av_vlog` drops the line when the level is below the minimum. It formats into a 2048-byte buffer and writes one line to stderr.
- The default minimum level is `AV_LOG_INFO`.

## Ownership and thread safety

- The global level is not protected by a lock. Set it before worker threads start.
- `av_vlog` uses a stack buffer. It is safe to call from any thread.

## Testing

Indirect. The codec tests check the error codes that these helpers set.

## Notes

- Log lines are not structured JSON yet. D2 replaces this module.
