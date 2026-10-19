/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetSystemTimePreciseAsFileTime
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef VOID (WINAPI *FN_GetSystemTimePreciseAsFileTime)(LPFILETIME);

static FN_GetSystemTimePreciseAsFileTime pGetSystemTimePreciseAsFileTime;

static ULONGLONG
FileTimeValue(const FILETIME *Time)
{
    return ((ULONGLONG)Time->dwHighDateTime << 32) | Time->dwLowDateTime;
}

static ULONGLONG
PreciseTime(VOID)
{
    FILETIME Time;

    pGetSystemTimePreciseAsFileTime(&Time);
    return FileTimeValue(&Time);
}

static void
test_CoarseBracket(void)
{
    FILETIME Before, After, Precise;
    LONGLONG Low = MAXLONGLONG, High = -MAXLONGLONG;
    int Below = 0, Above = 0, i;

    for (i = 0; i < 20000; i++)
    {
        GetSystemTimeAsFileTime(&Before);
        pGetSystemTimePreciseAsFileTime(&Precise);
        GetSystemTimeAsFileTime(&After);

        /* Windows 10: never before the coarse time read first (0 of 200000), up to one tick (15.6 ms) ahead */
        if (FileTimeValue(&Precise) < FileTimeValue(&Before))
            Below++;
        if (FileTimeValue(&Precise) > FileTimeValue(&After) + 250000)
            Above++;
        if ((LONGLONG)(FileTimeValue(&Precise) - FileTimeValue(&Before)) < Low)
            Low = FileTimeValue(&Precise) - FileTimeValue(&Before);
        if ((LONGLONG)(FileTimeValue(&Precise) - FileTimeValue(&After)) > High)
            High = FileTimeValue(&Precise) - FileTimeValue(&After);
    }

    ok(Below == 0, "%d of 20000 values were before the coarse time (lowest difference %I64d)\n", Below, Low);
    ok(Above == 0, "%d of 20000 values were more than 25 ms ahead of the coarse time (highest difference %I64d)\n",
       Above, High);
}

static void
test_Monotonic(void)
{
    ULONGLONG Previous, Current;
    int i, Backwards = 0;

    Previous = PreciseTime();
    for (i = 0; i < 200000; i++)
    {
        Current = PreciseTime();
        if (Current < Previous)
            Backwards++;
        Previous = Current;
    }

    ok(Backwards == 0, "The time went backwards %d times in 200000 calls\n", Backwards);
}

static void
test_Resolution(void)
{
    FILETIME Coarse;
    ULONGLONG Previous, Current, CoarsePrevious, CoarseCurrent;
    int PreciseSteps = 0, CoarseSteps = 0, i;

    /* The coarse time moves at the clock tick, a few times in a few thousand calls; the precise time with most calls */
    GetSystemTimeAsFileTime(&Coarse);
    CoarsePrevious = FileTimeValue(&Coarse);
    Previous = PreciseTime();
    for (i = 0; i < 5000; i++)
    {
        Current = PreciseTime();
        if (Current != Previous)
            PreciseSteps++;
        Previous = Current;

        GetSystemTimeAsFileTime(&Coarse);
        CoarseCurrent = FileTimeValue(&Coarse);
        if (CoarseCurrent != CoarsePrevious)
            CoarseSteps++;
        CoarsePrevious = CoarseCurrent;
    }

    /* Windows 10: 24 to 44 percent of the calls give a new value (a call takes less than the 100 ns of the counter),
       the coarse time a new value in 1 of 100,000 */
    ok(PreciseSteps >= 250, "The time changed only %d times in 5000 calls (the coarse time %d times)\n", PreciseSteps, CoarseSteps);
    ok(PreciseSteps > CoarseSteps * 10, "The time changed %d times, the coarse time %d times\n", PreciseSteps, CoarseSteps);
}

static ULONGLONG
CoarseTime(VOID)
{
    FILETIME Time;

    GetSystemTimeAsFileTime(&Time);
    return FileTimeValue(&Time);
}

static void
test_TracksClocks(void)
{
    LARGE_INTEGER Frequency, Before1, After1, Before2, After2;
    ULONGLONG Time1, Time2, CoarseBefore1, CoarseAfter1, CoarseBefore2, CoarseAfter2;
    LONGLONG DeltaTime, CounterLow, CounterHigh, CoarseLow, CoarseHigh;
    int i;

    QueryPerformanceFrequency(&Frequency);
    for (i = 0; i < 3; i++)
    {
        CoarseBefore1 = CoarseTime();
        QueryPerformanceCounter(&Before1);
        Time1 = PreciseTime();
        QueryPerformanceCounter(&After1);
        CoarseAfter1 = CoarseTime();
        Sleep(60 + 17 * i);
        CoarseBefore2 = CoarseTime();
        QueryPerformanceCounter(&Before2);
        Time2 = PreciseTime();
        QueryPerformanceCounter(&After2);
        CoarseAfter2 = CoarseTime();

        /* The time of each call is between the clocks read around it, so the difference is between these differences */
        DeltaTime = Time2 - Time1;
        CounterLow = (Before2.QuadPart - After1.QuadPart) * 10000000 / Frequency.QuadPart;
        CounterHigh = (After2.QuadPart - Before1.QuadPart) * 10000000 / Frequency.QuadPart;
        CoarseLow = CoarseBefore2 - CoarseAfter1;
        CoarseHigh = CoarseAfter2 - CoarseBefore1;

        /* The precise time never gets away from the system time by more than a tick (15.6 ms) */
        ok(DeltaTime >= CoarseLow - 250000 && DeltaTime <= CoarseHigh + 250000,
           "The time advanced by %I64d, the system time by %I64d to %I64d (in 100 ns)\n", DeltaTime, CoarseLow, CoarseHigh);

        /* Windows 10 agrees with the performance counter within 15 microseconds. The tick clock of a virtual machine
           that is not given the processor loses time against the counter; then the precise time follows the tick clock */
        if (CoarseHigh + 250000 >= CounterLow && CoarseLow <= CounterHigh + 250000)
        {
            ok(DeltaTime >= CounterLow - 250000 && DeltaTime <= CounterHigh + 250000,
               "The time advanced by %I64d, the performance counter by %I64d to %I64d (in 100 ns)\n", DeltaTime, CounterLow, CounterHigh);
        }
        else
        {
            skip("The system time moved %I64d to %I64d, the performance counter %I64d to %I64d\n", CoarseLow, CoarseHigh, CounterLow, CounterHigh);
        }
    }
}

static void
test_FirstStep(void)
{
    LARGE_INTEGER Frequency, Start, End;
    ULONGLONG Previous, Current;
    int i, Judged = 0, Large = 0, Backwards = 0;

    /* From the Wine test: the first change of the value is smaller than 1 ms; a change over a gap in which the thread
       was not run says nothing, and is not judged */
    QueryPerformanceFrequency(&Frequency);
    for (i = 0; i < 100 && Judged < 20; i++)
    {
        QueryPerformanceCounter(&Start);
        Previous = PreciseTime();
        do
        {
            Current = PreciseTime();
        } while (Current == Previous);
        QueryPerformanceCounter(&End);

        if ((End.QuadPart - Start.QuadPart) * 10000 > Frequency.QuadPart)
            continue;

        Judged++;
        if (Current < Previous)
            Backwards++;
        else if (Current - Previous >= 10000)
            Large++;
    }
    ok(Judged >= 10, "Only %d of 100 observations were not interrupted\n", Judged);
    ok(Backwards == 0, "The time went backwards %d times\n", Backwards);
    ok(Large == 0, "The time changed by 1 ms or more %d times in %d observations\n", Large, Judged);
}

static volatile ULONGLONG SharedMaximum;
static volatile LONG SharedFailures;

static DWORD WINAPI
OrderThread(PVOID Parameter)
{
    ULONGLONG Seen, Time, Old;
    int i;

    for (i = 0; i < 100000; i++)
    {
        Seen = InterlockedCompareExchange64((volatile LONGLONG *)&SharedMaximum, 0, 0);
        Time = PreciseTime();
        if (Time < Seen)
            InterlockedIncrement(&SharedFailures);

        /* Publish the largest value seen so far */
        do
        {
            Old = SharedMaximum;
            if (Time <= Old)
                break;
        } while ((ULONGLONG)InterlockedCompareExchange64((volatile LONGLONG *)&SharedMaximum, Time, Old) != Old);
    }
    return 0;
}

static void
test_Threads(void)
{
    HANDLE Threads[4];
    int i;

    SharedMaximum = 0;
    SharedFailures = 0;
    for (i = 0; i < 4; i++)
    {
        Threads[i] = CreateThread(NULL, 0, OrderThread, NULL, 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed with %lu\n", GetLastError());
    }
    for (i = 0; i < 4; i++)
    {
        if (Threads[i])
        {
            WaitForSingleObject(Threads[i], INFINITE);
            CloseHandle(Threads[i]);
        }
    }

    /* A call that starts after another call returned never returns a smaller time */
    ok(SharedFailures == 0, "%ld calls returned less than a call that had returned before\n", SharedFailures);
}

START_TEST(GetSystemTimePrecise)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

    pGetSystemTimePreciseAsFileTime = (FN_GetSystemTimePreciseAsFileTime)GetProcAddress(hKernel32, "GetSystemTimePreciseAsFileTime");
    if (!pGetSystemTimePreciseAsFileTime && !is_reactos())
    {
        skip("GetSystemTimePreciseAsFileTime is not exported\n");
        return;
    }
    ok(pGetSystemTimePreciseAsFileTime != NULL, "GetSystemTimePreciseAsFileTime is not exported\n");
    if (!pGetSystemTimePreciseAsFileTime)
        return;

    test_CoarseBracket();
    test_Monotonic();
    test_Resolution();
    test_TracksClocks();
    test_FirstStep();
    test_Threads();
}
