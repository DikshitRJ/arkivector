/*
 * miscadmin.h (pgcompat stub): CHECK_FOR_INTERRUPTS(). D0: polls nothing; D4 wires
 * it to the per-request deadline (av_pg_interrupt_pending). ARKIVECTOR-owned.
 */
#ifndef MISCADMIN_H
#define MISCADMIN_H
#include "postgres.h"
extern __thread volatile bool av_pg_interrupt_pending;
extern void av_pg_process_interrupts(void);
#define CHECK_FOR_INTERRUPTS() \
    do { \
        if (unlikely(av_pg_interrupt_pending)) \
            av_pg_process_interrupts(); \
    } while (0)
extern int maintenance_work_mem;
extern int work_mem;
#endif
