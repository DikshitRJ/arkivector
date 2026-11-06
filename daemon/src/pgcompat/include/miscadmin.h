/*
 * miscadmin.h (pgcompat stub): CHECK_FOR_INTERRUPTS().
 *
 * av_pg_interrupt_flag points at the cancellation flag of the request the thread is
 * serving (set by the query executor; NULL = not cancellable). Any thread may set
 * *flag (e.g. the loop thread's deadline timer); the worker polls it here. Inserters
 * never install a flag, so an insert cannot be interrupted half-way.
 * ARKIVECTOR-owned.
 */
#ifndef MISCADMIN_H
#define MISCADMIN_H
#include "postgres.h"
extern __thread volatile bool *av_pg_interrupt_flag;
extern void av_pg_process_interrupts(void);
#define CHECK_FOR_INTERRUPTS()                                                          \
    do {                                                                                \
        volatile bool *av_flag_ = av_pg_interrupt_flag;                                 \
        if (unlikely(av_flag_ != NULL && __atomic_load_n(av_flag_, __ATOMIC_RELAXED))) \
            av_pg_process_interrupts();                                                 \
    } while (0)
extern int maintenance_work_mem;
extern int work_mem;
#endif
