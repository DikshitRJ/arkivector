# daemon/src/version.h

## Purpose

Holds the version strings of the daemon, the API contract, and the copied upstreams.

## Macros

| Macro | Value | Source of truth |
| :--- | :--- | :--- |
| `AV_DAEMON_VERSION` | `"0.1.0-dev"` | This file. |
| `AV_API_VERSION` | `"1.0.0"` | `spec/VERSION`. |
| `AV_PGVECTOR_TAG` | `"v0.8.7"` | `src/pgvector/PROVENANCE.md`. |
| `AV_POSTGRES_TAG` | `"REL_17_11"` | `src/pgcompat/STUBS.md`. |

## Public API

`const char *av_version_string(void)` returns `arkivectord <version> (api <v>, pgvector <tag>)`. The string has static storage.

## Ownership and thread safety

Static strings. Safe from any thread.

## Testing

Not tested directly.
