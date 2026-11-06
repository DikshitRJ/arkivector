# daemon/src/pgcompat/include/utils/memutils.h

## Purpose

Stub. Declares the context API: `AllocSetContextCreate`, `AvContextCreate`, `MemoryContextReset`, `MemoryContextDelete`, `MemoryContextSwitchTo`, `GetMemoryChunkContext`, and the `ALLOCSET_*` size presets.

## Notes

`AvContextCreate` is our extension. It takes a hard cap. Implemented in `pgcompat/src/palloc.c`.

## Testing

`test/unit/test_pgcompat.c`.
