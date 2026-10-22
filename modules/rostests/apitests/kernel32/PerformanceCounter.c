/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that QueryPerformanceCounter, GetTickCount and the system time
 *              advance at the rate of the interrupt time
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define ATTEMPTS 3

typedef ULONGLONG (WINAPI *PGETTICKCOUNT64)(VOID);

typedef struct _CLOCK_SAMPLE
{
    ULONGLONG InterruptTime;
    ULONGLONG SystemTime;
    ULONGLONG TickCount;
    LONGLONG Qpc;
} CLOCK_SAMPLE;

static PGETTICKCOUNT64 pGetTickCount64;
static LONGLONG Frequency;

static ULONGLONG ReadInterruptTime(VOID)
{
    volatile KSYSTEM_TIME *Time = &SharedUserData->InterruptTime;
    LONG High;
    ULONG Low;

    do
    {
        High = Time->High1Time;
        Low = Time->LowPart;
    } while (High != Time->High2Time);

    return ((ULONGLONG)(ULONG)High << 32) | Low;
}

static LONGLONG ReadQpc(VOID)
{
    LARGE_INTEGER Qpc;

    QueryPerformanceCounter(&Qpc);
    return Qpc.QuadPart;
}

/* Samples right after the interrupt time changed, so that both ends of an
   interval lie on the clock interrupt grid. The change must have been seen
   within 100 us of the performance counter read and the whole sample taken
   within 100 us without another interrupt, or the sample is taken again */
static BOOL SampleAfterInterrupt(CLOCK_SAMPLE *Sample)
{
    ULONGLONG Start, Check;
    LONGLONG Before, After, Limit, Done;
    FILETIME Ft;
    ULONG Try;

    for (Try = 0; Try < 20; Try++)
    {
        Start = ReadInterruptTime();
        Before = ReadQpc();
        Limit = Before + Frequency;
        for (;;)
        {
            Sample->InterruptTime = ReadInterruptTime();
            After = ReadQpc();
            if ((Sample->InterruptTime != Start) || (After > Limit))
                break;
            Before = After;
        }
        GetSystemTimeAsFileTime(&Ft);
        Sample->TickCount = pGetTickCount64 ? pGetTickCount64() : GetTickCount();
        Check = ReadInterruptTime();
        Done = ReadQpc();
        Sample->SystemTime = ((ULONGLONG)Ft.dwHighDateTime << 32) | Ft.dwLowDateTime;
        Sample->Qpc = After;
        if ((Sample->InterruptTime != Start) && (Check == Sample->InterruptTime) &&
            (After - Before <= Frequency / 10000) && (Done - After <= Frequency / 10000))
        {
            return TRUE;
        }
    }
    return FALSE;
}

START_TEST(PerformanceCounter)
{
    CLOCK_SAMPLE Start, End;
    LARGE_INTEGER Freq;
    LONGLONG Interrupt = 0, Reference = 0, System = 0, Tick = 0, Expected = 0, Error = 0;
    DWORD Adjustment, Increment;
    BOOL AdjustmentDisabled, Measured = FALSE, Good;
    int OldPriority, Attempt;
    DWORD_PTR OldAffinity;

    pGetTickCount64 = (PGETTICKCOUNT64)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64");

    ok(QueryPerformanceFrequency(&Freq), "QueryPerformanceFrequency failed\n");
    Frequency = Freq.QuadPart;
    if (Frequency <= 0)
    {
        skip("No performance counter\n");
        return;
    }

    /* The clock interrupt runs on the boot processor; stay there */
    OldAffinity = SetThreadAffinityMask(GetCurrentThread(), 1);
    OldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    /* A time service can set the clock during an attempt: try three times */
    for (Attempt = 0; Attempt < ATTEMPTS; Attempt++)
    {
        if (!SampleAfterInterrupt(&Start))
            continue;
        Sleep(4000);
        if (!SampleAfterInterrupt(&End))
            continue;

        Interrupt = (LONGLONG)(End.InterruptTime - Start.InterruptTime);
        Reference = End.Qpc - Start.Qpc;
        Reference = (Reference / Frequency) * 10000000 + ((Reference % Frequency) * 10000000) / Frequency;
        System = (LONGLONG)(End.SystemTime - Start.SystemTime);
        if (pGetTickCount64)
            Tick = (LONGLONG)(End.TickCount - Start.TickCount) * 10000;
        else
            Tick = (LONGLONG)(ULONG)((ULONG)End.TickCount - (ULONG)Start.TickCount) * 10000;
        Error = (Reference - Interrupt) * 100000 / Interrupt;

        /* With a time adjustment the system time advances by the adjustment per clock increment */
        Expected = Interrupt;
        if (GetSystemTimeAdjustment(&Adjustment, &Increment, &AdjustmentDisabled) && !AdjustmentDisabled && Increment)
            Expected = Interrupt * Adjustment / Increment;

        Measured = TRUE;
        trace("Attempt %d: frequency %I64d, rate error %I64d/100000 over %I64d\n", Attempt, Frequency, Error, Interrupt);
        Good = (Error >= -100 && Error <= 100) &&
               (System > Expected - 160000 && System < Expected + 160000) &&
               (Tick > Interrupt - 170000 && Tick < Interrupt + 170000);
        if (Good)
            break;
    }

    SetThreadPriority(GetCurrentThread(), OldPriority);
    if (OldAffinity)
        SetThreadAffinityMask(GetCurrentThread(), OldAffinity);

    if (!Measured)
    {
        skip("No clock interrupt samples\n");
        return;
    }

    /* The performance counter frequency agrees with the interrupt time to 0.1%;
       a consistency check, not an absolute one */
    ok(Error >= -100 && Error <= 100,
       "QueryPerformanceCounter advanced %I64d in %I64d of interrupt time (error %I64d/100000, frequency %I64d)\n",
       Reference, Interrupt, Error, Frequency);

    /* The system time and the tick count follow the interrupt time to a clock period */
    ok(System > Expected - 160000 && System < Expected + 160000,
       "System time advanced %I64d, expected %I64d in %I64d of interrupt time\n", System, Expected, Interrupt);
    ok(Tick > Interrupt - 170000 && Tick < Interrupt + 170000,
       "Tick count advanced %I64d in %I64d of interrupt time\n", Tick, Interrupt);
}
