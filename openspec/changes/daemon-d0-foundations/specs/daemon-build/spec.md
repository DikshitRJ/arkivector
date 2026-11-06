## ADDED Requirements

### Requirement: Copied upstream code compiles unmodified
Files copied verbatim from pgvector v0.8.7 and PostgreSQL REL_17_11 SHALL compile without edits. The only exception is copy+shim files, whose deltas MUST be marked `ARKIVECTOR` and listed in PROVENANCE.md or STUBS.md.

#### Scenario: Verbatim pgvector files are byte-identical
- **WHEN** CI compares `daemon/src/pgvector/{halfutils,bitutils}.{c,h}` and the four type headers with `pgvector/src/`
- **THEN** every file is byte-identical

#### Scenario: Strict warnings apply only to Arkivector code
- **WHEN** the dev preset is built with gcc or clang
- **THEN** Arkivector code compiles with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`, and copied code compiles with pgvector's Makefile flags without `-Werror`

### Requirement: PostgreSQL error semantics
`ereport(ERROR)` SHALL transfer control to the innermost PG_TRY frame of the calling thread and release every LWLock the thread holds. Levels below ERROR SHALL NOT transfer control.

#### Scenario: Nested error handling
- **WHEN** code inside two nested PG_TRY blocks raises ERROR and the inner handler re-throws
- **THEN** the outer handler runs, the error data (SQLSTATE, message, detail) is available, and no lock stays held

### Requirement: Memory contexts
palloc'd memory SHALL belong to the current memory context. It SHALL be released all at once when that context is reset or deleted, and resetting a context SHALL delete its child contexts.

#### Scenario: Capped context
- **WHEN** allocations exceed a context's hard cap
- **THEN** an ERROR with SQLSTATE out_of_memory is raised instead of a crash
