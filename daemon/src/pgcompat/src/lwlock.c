/*
 * pgcompat/src/lwlock.c: 4-byte atomic reader-writer LWLock. See storage/lwlock.h.
 * ARKIVECTOR-owned.
 */
#include "postgres.h"

#include <sched.h>

#include "storage/lwlock.h"
#include "storage/s_lock.h"

#define LW_VAL_EXCLUSIVE ((uint32) 1 << 31)
#define LW_SHARED_MASK (LW_VAL_EXCLUSIVE - 1)

typedef struct {
    LWLock *lock;
    LWLockMode mode;
} HeldLock;

__thread bool av_lwlock_readonly_arena = false;

static __thread HeldLock held[MAX_SIMUL_LWLOCKS];
static __thread int num_held = 0;

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

void av_spin_lock_slow(volatile slock_t *lock)
{
    int spins = 0;
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0) {
            if (++spins < 1000) {
                cpu_relax();
            } else {
                sched_yield();
                spins = 0;
            }
        }
        if (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0) {
            return;
        }
    }
}

void LWLockInitialize(LWLock *lock, int tranche_id)
{
    (void) tranche_id;
    __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
}

static bool try_acquire(LWLock *lock, LWLockMode mode)
{
    uint32 old = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
    if (mode == LW_EXCLUSIVE) {
        if (old != 0) {
            return false;
        }
        return __atomic_compare_exchange_n(&lock->state, &old, LW_VAL_EXCLUSIVE, false, __ATOMIC_ACQUIRE,
                                           __ATOMIC_RELAXED);
    }
    if (old & LW_VAL_EXCLUSIVE) {
        return false;
    }
    return __atomic_compare_exchange_n(&lock->state, &old, old + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void remember(LWLock *lock, LWLockMode mode)
{
    if (num_held >= MAX_SIMUL_LWLOCKS) {
        elog(ERROR, "too many LWLocks taken");
    }
    held[num_held].lock = lock;
    held[num_held].mode = mode;
    num_held++;
}

bool LWLockAcquire(LWLock *lock, LWLockMode mode)
{
    if (av_lwlock_readonly_arena) {
        return true;
    }
    int spins = 0;
    while (!try_acquire(lock, mode)) {
        if (++spins < 200) {
            cpu_relax();
        } else {
            sched_yield();
            spins = 0;
        }
    }
    remember(lock, mode);
    return true;
}

bool LWLockConditionalAcquire(LWLock *lock, LWLockMode mode)
{
    if (av_lwlock_readonly_arena) {
        return true;
    }
    if (!try_acquire(lock, mode)) {
        return false;
    }
    remember(lock, mode);
    return true;
}

static void release_word(LWLock *lock, LWLockMode mode)
{
    if (mode == LW_EXCLUSIVE) {
        __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
    } else {
        __atomic_fetch_sub(&lock->state, 1, __ATOMIC_RELEASE);
    }
}

void LWLockRelease(LWLock *lock)
{
    if (av_lwlock_readonly_arena) {
        return;
    }
    /* Most recently acquired locks are released first, so search from the top. */
    for (int i = num_held - 1; i >= 0; i--) {
        if (held[i].lock == lock) {
            LWLockMode mode = held[i].mode;
            for (int j = i; j < num_held - 1; j++) {
                held[j] = held[j + 1];
            }
            num_held--;
            release_word(lock, mode);
            return;
        }
    }
    elog(ERROR, "lock is not held");
}

void LWLockReleaseAll(void)
{
    while (num_held > 0) {
        num_held--;
        release_word(held[num_held].lock, held[num_held].mode);
    }
}

bool LWLockHeldByMe(LWLock *lock)
{
    for (int i = 0; i < num_held; i++) {
        if (held[i].lock == lock) {
            return true;
        }
    }
    return false;
}

bool LWLockHeldByMeInMode(LWLock *lock, LWLockMode mode)
{
    for (int i = 0; i < num_held; i++) {
        if (held[i].lock == lock && held[i].mode == mode) {
            return true;
        }
    }
    return false;
}

int LWLockNewTrancheId(void)
{
    static int next = 100;
    return __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
}

void LWLockRegisterTranche(int tranche_id, const char *tranche_name)
{
    (void) tranche_id;
    (void) tranche_name;
}

int av_lwlock_held_count(void)
{
    return num_held;
}
