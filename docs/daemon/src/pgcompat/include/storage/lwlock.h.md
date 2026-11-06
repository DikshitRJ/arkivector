# daemon/src/pgcompat/include/storage/lwlock.h

## Purpose

Stub. Declares the lightweight lock API: `LWLock`, `LWLockAcquire`, `LWLockConditionalAcquire`, `LWLockRelease`, `LWLockReleaseAll`, `LWLockHeldByMe`, `LWLockInitialize`, and the thread-local flag `av_lwlock_readonly_arena`.

## Ownership and thread safety

Implemented in `pgcompat/src/lwlock.c`. The held-lock stack and the read-only flag are per thread.
