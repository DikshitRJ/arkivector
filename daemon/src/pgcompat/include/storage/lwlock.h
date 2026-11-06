/*
 * storage/lwlock.h (pgcompat stub): PostgreSQL LWLock API as a 4-byte atomic
 * reader-writer lock (ARCHITECTURE §5.2). Keeps HnswElementData compact.
 *
 * Lock word: bit 31 = held exclusively, bits 0..30 = number of shared holders.
 * Acquire spins briefly, then yields. Writers do not starve readers or vice versa
 * in any guaranteed way; pgvector holds these locks only for short critical sections.
 *
 * Per-thread held-lock stack (like PostgreSQL's held_lwlocks[]): LWLockRelease
 * looks up the mode there. On ERROR, PG_CATCH releases the locks taken since the
 * matching PG_TRY (av_pg_unwind_restore in elog.h).
 *
 * Read-only arena mode: when the thread-local av_lwlock_readonly_arena is true,
 * LWLockAcquire/LWLockRelease are no-ops and never write to the lock word. The
 * executor sets it while searching a sealed (immutable, PROT_READ mmapped) segment,
 * through av_lwlock_readonly_enter/leave; PG_CATCH restores it on error. A lock
 * acquired for real is always released for real, even if the flag changed.
 *
 * ARKIVECTOR-owned. See STUBS.md.
 */
#ifndef LWLOCK_H
#define LWLOCK_H

typedef struct LWLock {
    uint32 state; /* accessed only with __atomic builtins */
} LWLock;

typedef enum LWLockMode { LW_EXCLUSIVE, LW_SHARED, LW_WAIT_UNTIL_FREE } LWLockMode;

#define MAX_SIMUL_LWLOCKS 200

extern __thread bool av_lwlock_readonly_arena;

extern void LWLockInitialize(LWLock *lock, int tranche_id);
extern bool LWLockAcquire(LWLock *lock, LWLockMode mode);
extern bool LWLockConditionalAcquire(LWLock *lock, LWLockMode mode);
extern void LWLockRelease(LWLock *lock);
extern void LWLockReleaseAll(void);
extern bool LWLockHeldByMe(LWLock *lock);
extern bool LWLockHeldByMeInMode(LWLock *lock, LWLockMode mode);
extern int LWLockNewTrancheId(void);
extern void LWLockRegisterTranche(int tranche_id, const char *tranche_name);
extern int av_lwlock_held_count(void);
/* Enter read-only arena mode; returns the previous value for _leave(). */
extern bool av_lwlock_readonly_enter(void);
extern void av_lwlock_readonly_leave(bool prev);

#endif /* LWLOCK_H */
