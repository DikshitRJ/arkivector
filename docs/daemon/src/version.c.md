# daemon/src/version.c

## Purpose

Implements `av_version_string()` from `version.h`. The function returns a string literal built from the version macros.

## Ownership and thread safety

Returns a pointer to a string literal. Safe from any thread.

## Testing

Not tested directly. The string is printed by the daemon start-up path in later phases.
