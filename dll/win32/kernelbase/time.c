/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Interrupt time functions that Windows exports from kernelbase and not from kernel32
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The MSYS2 and Cygwin runtimes load these three at start (clock_gettime) and abort with a fatal error when
 * KernelBase.dll does not provide them.
 */

#include <stdarg.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>

/* Layout of the shared user data that has not changed since NT 4: the interrupt time is at 0x08 */
typedef struct _BASEP_KSYSTEM_TIME
{
    ULONG LowPart;
    LONG High1Time;
    LONG High2Time;
} BASEP_KSYSTEM_TIME;

#define BASEP_INTERRUPT_TIME ((volatile BASEP_KSYSTEM_TIME *)0x7ffe0008)
#define BASEP_TICK           156250LL    /* 15.6 ms in 100 ns units */
#define BASEP_UNSET          MINLONGLONG

static
LONGLONG
BasepReadInterruptTime(VOID)
{
    LONG High;
    ULONG Low;

    do
    {
        High = BASEP_INTERRUPT_TIME->High1Time;
        Low = BASEP_INTERRUPT_TIME->LowPart;
    } while (High != BASEP_INTERRUPT_TIME->High2Time);

    return ((LONGLONG)High << 32) | Low;
}

VOID
WINAPI
QueryInterruptTime(
    _Out_ PULONGLONG InterruptTime)
{
    *InterruptTime = BasepReadInterruptTime();
}

/*
 * The precise interrupt time is the interrupt time of the clock tick plus the performance counter elapsed since. It
 * never reads the system time, so that a change of the wall clock cannot move it.
 *
 * The process keeps one offset: the interrupt time minus the performance counter in 100 ns units. The first call
 * sets it from the tick time. Every call predicts the time with the offset and keeps the prediction in the band
 * that the tick time allows (not before the tick time read first, not later than the one read last plus a tick);
 * a prediction outside the band moves the offset into it, so a counter that runs at another rate than the tick
 * clock is followed. The value never goes back: a call that was preempted after it sampled returns the newest
 * value that was handed out.
 */
static volatile LONGLONG BasepPreciseOffset = BASEP_UNSET;
static volatile LONGLONG BasepPreciseLast;

static
LONGLONG
BasepCounterTo100ns(
    _In_ LONGLONG Counter,
    _In_ LONGLONG Frequency)
{
    /* Split, so that a large counter does not overflow the multiplication */
    return (Counter / Frequency) * 10000000LL + ((Counter % Frequency) * 10000000LL) / Frequency;
}

static
LONGLONG
BasepQueryPrecise(VOID)
{
    LARGE_INTEGER Counter, Frequency;
    LONGLONG First, Last, Offset, Time, Seen, Old;

    if (!QueryPerformanceFrequency(&Frequency) || Frequency.QuadPart <= 0)
        return BasepReadInterruptTime();

    First = BasepReadInterruptTime();
    if (!QueryPerformanceCounter(&Counter))
        return First;
    Last = BasepReadInterruptTime();

    Offset = InterlockedCompareExchange64(&BasepPreciseOffset, 0, 0);
    if (Offset == BASEP_UNSET)
    {
        /* The first call of the process; if another thread was first, its offset stands */
        Offset = First - BasepCounterTo100ns(Counter.QuadPart, Frequency.QuadPart);
        Old = InterlockedCompareExchange64(&BasepPreciseOffset, Offset, BASEP_UNSET);
        if (Old != BASEP_UNSET)
            Offset = Old;
    }

    Time = BasepCounterTo100ns(Counter.QuadPart, Frequency.QuadPart) + Offset;
    if (Time < First)
    {
        /* The counter runs slower than the tick clock */
        Old = Offset;
        Offset += First - Time;
        Time = First;
        InterlockedCompareExchange64(&BasepPreciseOffset, Offset, Old);
    }
    else if (Time > Last + BASEP_TICK)
    {
        /* The counter runs faster than the tick clock */
        Old = Offset;
        Offset -= Time - (Last + BASEP_TICK);
        Time = Last + BASEP_TICK;
        InterlockedCompareExchange64(&BasepPreciseOffset, Offset, Old);
    }

    Seen = InterlockedCompareExchange64(&BasepPreciseLast, 0, 0);
    for (;;)
    {
        if (Time <= Seen)
            return Seen;

        Old = InterlockedCompareExchange64(&BasepPreciseLast, Time, Seen);
        if (Old == Seen)
            return Time;

        Seen = Old;
    }
}

VOID
WINAPI
QueryInterruptTimePrecise(
    _Out_ PULONGLONG InterruptTime)
{
    *InterruptTime = BasepQueryPrecise();
}

/*
 * The shared user data of this system has no bias for time spent in sleep and hibernation (kernel32 reads it
 * as zero as well), so the unbiased time is the interrupt time.
 */
VOID
WINAPI
QueryUnbiasedInterruptTimePrecise(
    _Out_ PULONGLONG UnbiasedInterruptTime)
{
    *UnbiasedInterruptTime = BasepQueryPrecise();
}
