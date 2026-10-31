/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that NtDelayExecution does not return before a relative interval has passed
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* A delay ends at a clock interrupt, so it can be longer than requested. It must
   not be shorter: callers rely on "slept at least N ms". The slack covers what a
   virtual machine adds to the delivery of single clock interrupts; an unfixed
   system is early by up to a whole clock interval, 15.6 ms, on half the calls. */
#define SLACK_US 1000

static LARGE_INTEGER Frequency;

static LONGLONG
NowUs(VOID)
{
    LARGE_INTEGER Counter;

    NtQueryPerformanceCounter(&Counter, NULL);
    /* The counter can be a CPU time stamp counter: its product with 1000000 overflows */
    return (Counter.QuadPart / Frequency.QuadPart) * 1000000 +
           (Counter.QuadPart % Frequency.QuadPart) * 1000000 / Frequency.QuadPart;
}

/* Start the delay at a different point within the clock interval every time */
static void
SpinRandomPhase(void)
{
    LONGLONG End = NowUs() + (rand() % 17000);

    while (NowUs() < End)
        ;
}

static void
CheckDelay(LONGLONG Microseconds, int Iterations)
{
    LARGE_INTEGER Interval;
    LONGLONG Shortest = MAXLONGLONG, Start, Elapsed;
    NTSTATUS Status;
    int Early = 0, i;

    for (i = 0; i < Iterations; i++)
    {
        SpinRandomPhase();
        Interval.QuadPart = -Microseconds * 10;
        Start = NowUs();
        Status = NtDelayExecution(FALSE, &Interval);
        Elapsed = NowUs() - Start;

        ok_ntstatus(Status, STATUS_SUCCESS);
        if (Elapsed < Shortest)
            Shortest = Elapsed;
        if (Elapsed + SLACK_US < Microseconds)
            Early++;
    }

    ok(Early == 0, "NtDelayExecution(%I64d us) returned early %d of %d times, shortest %I64d us\n",
       Microseconds, Early, Iterations, Shortest);
}

START_TEST(NtDelayExecution)
{
    ULONG Coarsest, Finest, Current, Actual;
    LARGE_INTEGER Counter;
    NTSTATUS Status;

    NtQueryPerformanceCounter(&Counter, &Frequency);
    srand(NtGetTickCount());

    CheckDelay(1000, 200);
    CheckDelay(10000, 100);
    CheckDelay(15600, 100);
    CheckDelay(100000, 40);
    CheckDelay(1000000, 8);

    /* The same with the finest clock interval the system offers */
    Status = NtQueryTimerResolution(&Coarsest, &Finest, &Current);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtSetTimerResolution(Finest, TRUE, &Actual);
        ok_ntstatus(Status, STATUS_SUCCESS);
        CheckDelay(1000, 200);
        CheckDelay(10000, 100);
        CheckDelay(100000, 40);
        Status = NtSetTimerResolution(Finest, FALSE, &Actual);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }
}
