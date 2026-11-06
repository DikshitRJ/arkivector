/*
 * test_pgcompat.c: palloc contexts, reset semantics, PG_TRY/ereport, LWLock basics.
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"

#include "greatest.h"

TEST palloc_in_context(void)
{
    MemoryContext ctx = AllocSetContextCreate(NULL, "t", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    char *a = palloc(100);
    char *b = palloc0(4000);
    ASSERT(a != NULL && b != NULL);
    ASSERT_EQ(GetMemoryChunkContext(a), ctx);
    for (int i = 0; i < 4000; i++)
        ASSERT_EQ(0, b[i]);
    ASSERT_EQ(0, ((uintptr_t) a) % 16);
    memset(a, 'x', 100);
    char *c = repalloc(a, 10000);
    ASSERT_EQ('x', c[99]);
    ASSERT(MemoryContextMemAllocated(ctx, false) > 0);
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    PASS();
}

TEST reset_frees_everything(void)
{
    MemoryContext ctx = AllocSetContextCreate(NULL, "t", ALLOCSET_SMALL_SIZES);
    MemoryContext child = AllocSetContextCreate(ctx, "child", ALLOCSET_SMALL_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    for (int i = 0; i < 1000; i++)
        (void) palloc(1000);
    MemoryContextSwitchTo(child);
    (void) palloc(1 << 20);
    MemoryContextSwitchTo(old);
    ASSERT(MemoryContextMemAllocated(ctx, true) > (1 << 20));
    MemoryContextReset(ctx); /* deletes child too */
    ASSERT(ctx != NULL);
    ASSERT(MemoryContextIsEmpty(ctx));
    ASSERT(MemoryContextMemAllocated(ctx, true) < 64 * 1024);
    MemoryContextDelete(ctx);
    PASS();
}

TEST list_works(void)
{
    MemoryContext ctx = AllocSetContextCreate(NULL, "t", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    List *l = NIL;
    for (int i = 0; i < 1000; i++)
        l = lappend_int(l, i);
    ASSERT_EQ(1000, list_length(l));
    int sum = 0;
    ListCell *lc;
    foreach (lc, l)
        sum += lfirst_int(lc);
    ASSERT_EQ(999 * 1000 / 2, sum);
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    PASS();
}

static void raise_error(int code)
{
    ereport(ERROR, (errcode(code), errmsg("boom %d", 42), errdetail("detail here")));
}

TEST nested_pg_try_catches_error(void)
{
    volatile int outer = 0, inner = 0;
    PG_TRY();
    {
        PG_TRY(2);
        {
            raise_error(ERRCODE_INVALID_TEXT_REPRESENTATION);
            FAILm("not reached");
        }
        PG_CATCH(2);
        {
            inner = 1;
            ASSERT_EQ(ERRCODE_INVALID_TEXT_REPRESENTATION, av_pg_error_data()->sqlerrcode);
            ASSERT_STR_EQ("boom 42", av_pg_error_data()->message);
            ASSERT_STR_EQ("detail here", av_pg_error_data()->detail);
            PG_RE_THROW();
        }
        PG_END_TRY(2);
    }
    PG_CATCH();
    {
        outer = 1;
        FlushErrorState();
    }
    PG_END_TRY();
    ASSERT_EQ(1, inner);
    ASSERT_EQ(1, outer);
    ASSERT_EQ(NULL, PG_exception_stack);
    PASS();
}

TEST warning_does_not_jump(void)
{
    volatile int reached = 0;
    int saved = log_min_messages;
    log_min_messages = ERROR; /* keep test output clean */
    PG_TRY();
    {
        elog(WARNING, "just a warning");
        ereport(NOTICE, (errmsg("notice")));
        reached = 1;
    }
    PG_CATCH();
    {
        reached = -1;
    }
    PG_END_TRY();
    log_min_messages = saved;
    ASSERT_EQ(1, reached);
    PASS();
}

TEST error_releases_lwlocks(void)
{
    static LWLock lock;
    LWLockInitialize(&lock, 0);
    PG_TRY();
    {
        LWLockAcquire(&lock, LW_EXCLUSIVE);
        ASSERT_EQ(1, av_lwlock_held_count());
        elog(ERROR, "fail while holding");
    }
    PG_CATCH();
    {
        FlushErrorState();
    }
    PG_END_TRY();
    ASSERT_EQ(0, av_lwlock_held_count());
    ASSERT(LWLockConditionalAcquire(&lock, LW_EXCLUSIVE));
    LWLockRelease(&lock);
    PASS();
}

TEST lwlock_shared_and_readonly(void)
{
    static LWLock lock;
    LWLockInitialize(&lock, 0);
    LWLockAcquire(&lock, LW_SHARED);
    LWLockAcquire(&lock, LW_SHARED);
    ASSERT(!LWLockConditionalAcquire(&lock, LW_EXCLUSIVE));
    LWLockRelease(&lock);
    LWLockRelease(&lock);
    ASSERT(LWLockConditionalAcquire(&lock, LW_EXCLUSIVE));
    LWLockRelease(&lock);
    /* read-only mode never writes the lock word */
    av_lwlock_readonly_arena = true;
    LWLockAcquire(&lock, LW_EXCLUSIVE);
    ASSERT_EQ(0u, lock.state);
    LWLockRelease(&lock);
    av_lwlock_readonly_arena = false;
    ASSERT_EQ(0, av_lwlock_held_count());
    PASS();
}

TEST oom_in_capped_context(void)
{
    MemoryContext ctx = AvContextCreate(NULL, "capped", 4096, 64 * 1024);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    volatile int caught = 0;
    PG_TRY();
    {
        for (int i = 0; i < 1000; i++)
            (void) palloc(1024);
    }
    PG_CATCH();
    {
        caught = (av_pg_error_data()->sqlerrcode == ERRCODE_OUT_OF_MEMORY);
        FlushErrorState();
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    ASSERT_EQ(1, caught);
    PASS();
}

TEST catch_releases_only_inner_locks(void)
{
    static LWLock outer, inner;
    LWLockInitialize(&outer, 0);
    LWLockInitialize(&inner, 0);
    LWLockAcquire(&outer, LW_EXCLUSIVE);
    PG_TRY();
    {
        LWLockAcquire(&inner, LW_SHARED);
        (void) av_lwlock_readonly_enter();
        elog(ERROR, "inner failure");
    }
    PG_CATCH();
    {
        FlushErrorState();
    }
    PG_END_TRY();
    ASSERT_EQ(false, av_lwlock_readonly_arena); /* restored */
    ASSERT_EQ(1, av_lwlock_held_count());       /* outer still held */
    ASSERT(LWLockHeldByMe(&outer));
    ASSERT(LWLockConditionalAcquire(&inner, LW_EXCLUSIVE));
    LWLockRelease(&inner);
    LWLockRelease(&outer);
    ASSERT_EQ(0, av_lwlock_held_count());
    PASS();
}

TEST release_after_flag_change(void)
{
    static LWLock lock;
    LWLockInitialize(&lock, 0);
    LWLockAcquire(&lock, LW_EXCLUSIVE);
    bool prev = av_lwlock_readonly_enter();
    LWLockRelease(&lock); /* real lock: released for real */
    av_lwlock_readonly_leave(prev);
    ASSERT_EQ(0u, lock.state);
    ASSERT_EQ(0, av_lwlock_held_count());
    PASS();
}

SUITE(pgcompat)
{
    RUN_TEST(catch_releases_only_inner_locks);
    RUN_TEST(release_after_flag_change);
    RUN_TEST(palloc_in_context);
    RUN_TEST(reset_frees_everything);
    RUN_TEST(list_works);
    RUN_TEST(nested_pg_try_catches_error);
    RUN_TEST(warning_does_not_jump);
    RUN_TEST(error_releases_lwlocks);
    RUN_TEST(lwlock_shared_and_readonly);
    RUN_TEST(oom_in_capped_context);
}
