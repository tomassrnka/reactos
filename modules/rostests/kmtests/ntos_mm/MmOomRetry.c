/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Bounded out-of-memory page-fault retry (T16 kernel robustness)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 *
 * A thread that faults out of memory while holding a lock the balancer needs
 * can only spin: it must not wait for the balancer (CORE-20761). That spin used
 * to be unbounded, so under sustained exhaustion the thread held its contended
 * lock forever and every thread behind it froze. The fix bounds that spin
 * (MiMaxOomSpinRetries) and then fails the fault with STATUS_NO_MEMORY, which
 * unwinds and releases the lock. MmDbgTestOomRetryBound drives that exact bound
 * while holding the process AddressCreationLock; SimulatedMax overrides the live
 * cap and Ceiling caps the test's own loop so the unbounded case cannot hang.
 *
 * Counts are kept small: each no-progress spin performs a real
 * KeDelayExecutionThread (~one tick), so a large count would run for minutes.
 */

#include <kmt_test.h>

typedef ULONG (NTAPI *PMM_DBG_TEST_OOM)(ULONG SimulatedMax, ULONG Ceiling);

START_TEST(MmOomRetry)
{
    PMM_DBG_TEST_OOM pMmDbgTestOomRetryBound;
    ULONG Unbounded, Bounded, Again;
    const ULONG Cap = 64;
    const ULONG Ceiling = 200;

    pMmDbgTestOomRetryBound = KmtGetSystemRoutineAddress(L"MmDbgTestOomRetryBound");
    if (skip(pMmDbgTestOomRetryBound != NULL, "MmDbgTestOomRetryBound unavailable\n"))
        return;

    /* Fail-before: with the cap disabled the lock-held OOM spin never stops on
     * its own; only the test's ceiling ends it. This is the pre-fix behaviour. */
    Unbounded = pMmDbgTestOomRetryBound(0xFFFFFFFF, Ceiling);
    ok(Unbounded == Ceiling,
       "Without a cap the lock-held spin should reach the ceiling %lu, got %lu\n",
       Ceiling, Unbounded);

    /* Pass-after: with the cap active the spin stops at the cap, below the
     * ceiling - bounded. That it returned at all means the guarded mutex was
     * released (otherwise the next call would deadlock on a non-recursive lock). */
    Bounded = pMmDbgTestOomRetryBound(Cap, Ceiling);
    ok(Bounded == Cap, "With the cap the spin should stop at %lu, got %lu\n", Cap, Bounded);
    ok(Bounded < Ceiling, "Bounded %lu must be below ceiling %lu\n", Bounded, Ceiling);

    /* Lock-release proof: a guarded mutex is not recursive, so this second
     * bounded call can only succeed if every prior call released it. */
    Again = pMmDbgTestOomRetryBound(Cap, Ceiling);
    ok(Again == Cap, "Repeat call proves the lock was released each time, got %lu\n", Again);
}
