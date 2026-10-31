/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that Sleep and SleepEx never return before the interval has passed
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

    QueryPerformanceCounter(&Counter);
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
CheckDelay(const char *Name, DWORD Milliseconds, int Iterations, BOOL Ex)
{
    LONGLONG Shortest = MAXLONGLONG, Start, Elapsed;
    int Early = 0, i;

    for (i = 0; i < Iterations; i++)
    {
        SpinRandomPhase();
        Start = NowUs();
        if (Ex)
            SleepEx(Milliseconds, FALSE);
        else
            Sleep(Milliseconds);
        Elapsed = NowUs() - Start;

        if (Elapsed < Shortest)
            Shortest = Elapsed;
        if (Elapsed + SLACK_US < (LONGLONG)Milliseconds * 1000)
            Early++;
    }

    ok(Early == 0, "%s(%lu) returned early %d of %d times, shortest %I64d us\n",
       Name, Milliseconds, Early, Iterations, Shortest);
}

START_TEST(SleepNeverEarly)
{
    QueryPerformanceFrequency(&Frequency);
    srand(GetTickCount());

    CheckDelay("Sleep", 1, 200, FALSE);
    CheckDelay("Sleep", 10, 100, FALSE);
    CheckDelay("Sleep", 16, 100, FALSE);
    CheckDelay("Sleep", 100, 40, FALSE);
    CheckDelay("Sleep", 1000, 12, FALSE);
    CheckDelay("SleepEx", 1, 200, TRUE);
    CheckDelay("SleepEx", 10, 100, TRUE);
    CheckDelay("SleepEx", 100, 40, TRUE);
    CheckDelay("SleepEx", 1000, 6, TRUE);
}
