/*
 * av_hnsw_glue.c: ARKIVECTOR-owned definitions that pgvector's hnsw.c (not copied)
 * would normally provide: the hnsw.* GUC variables with pgvector's defaults, the
 * lock tranche, and the per-thread level-sampling PRNG used by RandomDouble()
 * (hnsw.h ARKIVECTOR delta). Compiled with the upstream flags in av_pgvector.
 */
#include "postgres.h"

#include "hnsw.h"
#include "miscadmin.h"

/* Defaults from pgvector v0.8.7 src/hnsw.c HnswInit(). */
int			hnsw_ef_search = HNSW_DEFAULT_EF_SEARCH;
int			hnsw_iterative_scan = HNSW_ITERATIVE_SCAN_OFF;
int			hnsw_max_scan_tuples = 20000;
double		hnsw_scan_mem_multiplier = 1;
int			hnsw_lock_tranche_id = 0;

int			maintenance_work_mem = 65536;	/* kB */
int			work_mem = 4096;	/* kB */

__thread volatile bool *av_pg_interrupt_flag = NULL;

void
av_pg_process_interrupts(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_QUERY_CANCELED),
			 errmsg("canceling statement due to request timeout")));
}

void
HnswInitLockTranche(void)
{
	/* LWLock tranches are informational only in pgcompat. */
}

__thread pg_prng_state *av_hnsw_prng = NULL;
static __thread pg_prng_state av_hnsw_prng_default;
static __thread bool av_hnsw_prng_seeded = false;

pg_prng_state *
av_hnsw_prng_current(void)
{
	if (av_hnsw_prng != NULL)
		return av_hnsw_prng;
	if (!av_hnsw_prng_seeded)
	{
		/*
		 * Distinct per thread (a process-wide counter), so threads do not draw
		 * correlated levels. Segments install their own seeded state for
		 * deterministic builds.
		 */
		static uint64 thread_counter = 0;
		uint64		n = __atomic_fetch_add(&thread_counter, 1, __ATOMIC_RELAXED);

		pg_prng_seed(&av_hnsw_prng_default, (uint64) 0x41524B49564543ULL ^ (n * UINT64CONST(0x9E3779B97F4A7C15)));
		av_hnsw_prng_seeded = true;
	}
	return &av_hnsw_prng_default;
}
