/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that KeDelayExecutionThread does not return before the interval
 *              has passed, in kernel mode as well as in user mode
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define ITERATIONS 30
/* The performance counter is read before and after the wait, not at the
   moments the wait is set up and ends: allow for that */
#define TOLERANCE_US 100

static
VOID
TestDelays(
    _In_ KPROCESSOR_MODE WaitMode,
    _In_ PCSTR ModeName)
{
    static const ULONG IntervalsUs[] = { 1000, 10000, 15000, 16000, 31000, 50000 };
    LARGE_INTEGER Frequency, Start, End, Interval;
    ULONG Index, Iteration, Early, Spin;
    LONGLONG Elapsed, Shortest;
    NTSTATUS Status;

    for (Index = 0; Index < RTL_NUMBER_OF(IntervalsUs); Index++)
    {
        Early = 0;
        Shortest = MAXLONGLONG;
        Interval.QuadPart = -(LONGLONG)IntervalsUs[Index] * 10;
        for (Iteration = 0; Iteration < ITERATIONS; Iteration++)
        {
            /* A delay that starts right after a clock interrupt (as one does
               after the previous delay) cannot be early; start at every phase
               of the clock interval */
            for (Spin = 0; Spin < (Iteration * 530) % 16000; Spin += 100)
                KeStallExecutionProcessor(100);

            Start = KeQueryPerformanceCounter(&Frequency);
            Status = KeDelayExecutionThread(WaitMode, FALSE, &Interval);
            End = KeQueryPerformanceCounter(NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);

            Elapsed = (End.QuadPart - Start.QuadPart) * 1000000 / Frequency.QuadPart;
            if (Elapsed < Shortest)
                Shortest = Elapsed;
            if (Elapsed + TOLERANCE_US < (LONGLONG)IntervalsUs[Index])
                Early++;
        }
        trace("%s mode, %lu us: shortest %I64d us, %lu of %d early\n",
              ModeName, IntervalsUs[Index], Shortest, Early, ITERATIONS);
        ok(Early == 0, "%s mode: %lu of %d delays of %lu us ended early (shortest %I64d us)\n",
           ModeName, Early, ITERATIONS, IntervalsUs[Index], Shortest);
    }
}

START_TEST(KeDelayExecution)
{
    ok_irql(PASSIVE_LEVEL);
    TestDelays(UserMode, "User");
    TestDelays(KernelMode, "Kernel");
    ok_irql(PASSIVE_LEVEL);
}
