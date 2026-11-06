/*
 * storage/s_lock.h (pgcompat stub): slock_t spinlock on a C11 atomic_flag-style
 * byte. SpinLockAcquire spins with a pause and yields after a while. In the
 * read-only arena mode (see storage/lwlock.h) spinlocks inside sealed, read-only
 * mmapped graphs are never touched because the HNSW search path takes none.
 * ARKIVECTOR-owned.
 */
#ifndef S_LOCK_H
#define S_LOCK_H

typedef unsigned char slock_t;

extern void av_spin_lock_slow(volatile slock_t *lock);

static inline void av_spin_init(volatile slock_t *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
static inline void av_spin_acquire(volatile slock_t *lock)
{
    if (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0)
        av_spin_lock_slow(lock);
}
static inline void av_spin_release(volatile slock_t *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

#define SpinLockInit(lock)    av_spin_init(lock)
#define SpinLockAcquire(lock) av_spin_acquire(lock)
#define SpinLockRelease(lock) av_spin_release(lock)
#define SpinLockFree(lock)    (__atomic_load_n((lock), __ATOMIC_RELAXED) == 0)
#define S_INIT_LOCK(lock)     av_spin_init(lock)
#define S_LOCK(lock)          av_spin_acquire(lock)
#define S_UNLOCK(lock)        av_spin_release(lock)

#endif /* S_LOCK_H */
