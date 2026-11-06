/*
 * core/fileio.h: bounded whole-file reads for small local files (config, API keys, keystores).
 *
 * Thread-safety: stateless; safe from any thread (does blocking I/O, so never call it on the loop thread).
 */
#ifndef AV_CORE_FILEIO_H
#define AV_CORE_FILEIO_H

#include <stddef.h>

#include "log.h"

/*
 * Read the whole file at `path` into a NUL-terminated heap buffer.
 * `what` labels error messages (e.g. "config"). Files larger than `max_bytes` are rejected.
 * On success returns the buffer (caller frees with free()) and stores the byte count (without the
 * terminating NUL) in *out_len. On failure returns NULL and fills `e` (code invalid_argument or internal).
 */
char *av_read_file(const char *path, const char *what, size_t max_bytes, size_t *out_len, av_err *e);

#endif
