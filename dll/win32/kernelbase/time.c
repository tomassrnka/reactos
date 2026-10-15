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

/* Layout of the shared user data that has not changed since NT 4: InterruptTime at 0x08, SystemTime at 0x14 */
typedef struct _BASEP_KSYSTEM_TIME
{
    ULONG LowPart;
    LONG High1Time;
    LONG High2Time;
} BASEP_KSYSTEM_TIME;

#define BASEP_INTERRUPT_TIME ((volatile BASEP_KSYSTEM_TIME *)0x7ffe0008)
#define BASEP_SYSTEM_TIME    ((volatile BASEP_KSYSTEM_TIME *)0x7ffe0014)
#define BASEP_MAX_ADVANCE    1000000ULL  /* 100 ms in 100 ns units: far above a tick of 15.6 ms */

static
ULONGLONG
BasepReadKSystemTime(
    _In_ volatile BASEP_KSYSTEM_TIME *Time)
{
    LONG High;
    ULONG Low;

    do
    {
        High = Time->High1Time;
        Low = Time->LowPart;
    } while (High != Time->High2Time);

    return ((ULONGLONG)(ULONG)High << 32) | Low;
}

VOID
WINAPI
QueryInterruptTime(
    _Out_ PULONGLONG InterruptTime)
{
    *InterruptTime = BasepReadKSystemTime(BASEP_INTERRUPT_TIME);
}

/*
 * The interrupt time and the system time advance together at every clock tick. The precise system time of
 * kernel32 lies between the system time at the start of the call and one tick after it, and the amount it is
 * ahead of the tick is the same for the interrupt time.
 */
VOID
WINAPI
QueryInterruptTimePrecise(
    _Out_ PULONGLONG InterruptTime)
{
    ULONGLONG Interrupt, System, Precise;
    ULONGLONG Interrupt2, System2;
    FILETIME FileTime;

    do
    {
        Interrupt = BasepReadKSystemTime(BASEP_INTERRUPT_TIME);
        System = BasepReadKSystemTime(BASEP_SYSTEM_TIME);
        Interrupt2 = BasepReadKSystemTime(BASEP_INTERRUPT_TIME);
        System2 = BasepReadKSystemTime(BASEP_SYSTEM_TIME);
    } while ((Interrupt != Interrupt2) || (System != System2));

    GetSystemTimePreciseAsFileTime(&FileTime);
    Precise = ((ULONGLONG)FileTime.dwHighDateTime << 32) | FileTime.dwLowDateTime;

    /* The system time was set between the two reads (backward, or forward by more than a tick could be): no advance */
    if ((Precise < System) || (Precise - System > BASEP_MAX_ADVANCE))
        Precise = System;

    *InterruptTime = Interrupt + (Precise - System);
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
    QueryInterruptTimePrecise(UnbiasedInterruptTime);
}
