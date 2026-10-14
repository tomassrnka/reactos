/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that the tick count, interrupt time and system time keep pace
 *              with the performance counter, also across clock interrupts that
 *              could not be delivered
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include <reactos/rosclock.h>

#define NDEBUG
#include <debug.h>

#define BLOCKED_MS 100
#define ROUNDS 3
#define RATE_ATTEMPTS 3

typedef struct _CLOCK_SAMPLE
{
    ULONGLONG InterruptTime;
    ULONGLONG SystemTime;
    ULONGLONG TickCount;
    LONGLONG Qpc;
} CLOCK_SAMPLE, *PCLOCK_SAMPLE;

static LONGLONG QpcFrequency;

/* Waits for the next clock interrupt and samples right after it, so that a
   sample is on the interrupt grid. The interrupt must have been seen within
   50 us of the performance counter read and the whole sample taken within
   50 us without another interrupt, or the sample is taken again (50 tries) */
static
BOOLEAN
SampleAfterInterrupt(
    _Out_ PCLOCK_SAMPLE Sample)
{
    ULONGLONG Start, Check;
    LONGLONG Before, After, Limit, Done;
    LARGE_INTEGER Time, Ticks;
    ULONG Try, NoInterrupt = 0, SlowRead = 0, Interrupted = 0;
    KIRQL OldIrql;

    for (Try = 0; Try < 50; Try++)
    {
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        Start = KeQueryInterruptTime();
        Before = KeQueryPerformanceCounter(NULL).QuadPart;
        Limit = Before + QpcFrequency;
        for (;;)
        {
            Sample->InterruptTime = KeQueryInterruptTime();
            After = KeQueryPerformanceCounter(NULL).QuadPart;
            if ((Sample->InterruptTime != Start) || (After > Limit))
                break;
            Before = After;
            YieldProcessor();
        }
        KeQuerySystemTime(&Time);
        KeQueryTickCount(&Ticks);
        Check = KeQueryInterruptTime();
        Done = KeQueryPerformanceCounter(NULL).QuadPart;
        KeLowerIrql(OldIrql);

        Sample->Qpc = After;
        Sample->SystemTime = Time.QuadPart;
        Sample->TickCount = Ticks.QuadPart;
        if (Sample->InterruptTime == Start)
            NoInterrupt++;
        else if (Check != Sample->InterruptTime)
            Interrupted++;
        else if ((After - Before > QpcFrequency / 20000) || (Done - After > QpcFrequency / 20000))
            SlowRead++;
        else
            return TRUE;
    }
    trace("No sample in %lu tries: %lu without an interrupt in a second, %lu with another interrupt, %lu with a read over 50 us\n",
          Try, NoInterrupt, Interrupted, SlowRead);
    return FALSE;
}

/* Whether the HAL counts clock interrupts it could not deliver. A HAL that
   answers the ReactOS clock query says so; for one that does not (before the
   query existed) the APIC HAL with a constant-rate TSC is the case to test */
static
BOOLEAN
LostTicksCounted(
    _Out_ PBOOLEAN FromHal)
{
    ULONG Flags = 0, Length = 0;
    INT CpuInfo[4];
    BOOLEAN Constant;
    NTSTATUS Status;

    Status = HalQuerySystemInformation(HalReactOSClockInformation, sizeof(Flags), &Flags, &Length);
    *FromHal = NT_SUCCESS(Status);
    if (*FromHal)
        return (Flags & HAL_ROS_CLOCK_COUNTS_LOST_INTERRUPTS) != 0;

    if (QpcFrequency == 1193182)
        return FALSE;
    __cpuid(CpuInfo, 1);
    Constant = (CpuInfo[2] & 0x80000000) != 0;
    __cpuid(CpuInfo, 0x80000000);
    if ((ULONG)CpuInfo[0] >= 0x80000007)
    {
        __cpuid(CpuInfo, 0x80000007);
        Constant |= (CpuInfo[3] & 0x100) != 0;
    }
    return Constant;
}

/* Elapsed performance counter time in 100 ns units */
static
LONGLONG
QpcElapsed(
    _In_ PCLOCK_SAMPLE Start,
    _In_ PCLOCK_SAMPLE End)
{
    LONGLONG Counts = End->Qpc - Start->Qpc;

    return (Counts / QpcFrequency) * 10000000 + ((Counts % QpcFrequency) * 10000000) / QpcFrequency;
}

static
VOID
TestBlockedInterrupts(VOID)
{
    CLOCK_SAMPLE Start, First, End;
    ULONG Increment = KeQueryTimeIncrement(), Round;
    LONGLONG Reference, Interrupt, System, Tick, Tolerance, Limit;
    KTIMER Inside, After;
    LARGE_INTEGER DueTime, Timeout;
    NTSTATUS Status;
    KIRQL OldIrql;

    /* One period of the clock and some latency */
    Tolerance = Increment + 20000;

    for (Round = 0; Round < ROUNDS; Round++)
    {
        KeInitializeTimer(&Inside);
        KeInitializeTimer(&After);

        if (!SampleAfterInterrupt(&Start))
        {
            skip(FALSE, "Round %lu: no clock interrupt sample\n", Round);
            continue;
        }

        /* One timer falls due while the clock interrupt is blocked, one shortly after */
        DueTime.QuadPart = -(BLOCKED_MS / 2) * 10000LL;
        KeSetTimer(&Inside, DueTime, NULL);
        DueTime.QuadPart = -(BLOCKED_MS + 20) * 10000LL;
        KeSetTimer(&After, DueTime, NULL);

        /* Keep the clock interrupt from this processor for several periods */
        KeRaiseIrql(HIGH_LEVEL, &OldIrql);
        _disable();
        KeStallExecutionProcessor(BLOCKED_MS * 1000);
        _enable();
        KeLowerIrql(OldIrql);

        /* The first sample after the window. If the clock is up to date by
           then, the timer that fell due inside the window must have expired,
           also when the tick count skipped its table hand */
        if (SampleAfterInterrupt(&First))
        {
            Reference = QpcElapsed(&Start, &First);
            Interrupt = (LONGLONG)(First.InterruptTime - Start.InterruptTime);
            Timeout.QuadPart = 0;
            Status = KeWaitForSingleObject(&Inside, Executive, KernelMode, FALSE, &Timeout);
            if (Interrupt > Reference - Tolerance)
                ok_eq_hex(Status, STATUS_SUCCESS);
            else
                trace("Round %lu: the first interrupt advanced %I64d of %I64d\n", Round, Interrupt, Reference);
        }

        /* Give the clock interrupt four periods to catch up */
        Limit = KeQueryPerformanceCounter(NULL).QuadPart + (QpcFrequency * 4 * Increment) / 10000000;
        while (KeQueryPerformanceCounter(NULL).QuadPart < Limit)
            YieldProcessor();
        if (!SampleAfterInterrupt(&End))
        {
            skip(FALSE, "Round %lu: no clock interrupt sample\n", Round);
            KeCancelTimer(&Inside);
            KeCancelTimer(&After);
            continue;
        }

        Reference = QpcElapsed(&Start, &End);
        Interrupt = (LONGLONG)(End.InterruptTime - Start.InterruptTime);
        System = (LONGLONG)(End.SystemTime - Start.SystemTime);
        Tick = (LONGLONG)(End.TickCount - Start.TickCount) * Increment;

        ok(Reference >= BLOCKED_MS * 10000LL, "Round %lu: elapsed %I64d is shorter than the blocked time\n",
           Round, Reference);
        ok(Interrupt > Reference - Tolerance && Interrupt < Reference + Tolerance,
           "Round %lu: interrupt time advanced %I64d, performance counter %I64d\n", Round, Interrupt, Reference);
        ok(Tick > Reference - Tolerance - Increment && Tick < Reference + Tolerance + Increment,
           "Round %lu: tick count advanced %I64d, performance counter %I64d\n", Round, Tick, Reference);
        ok(System > Reference - Tolerance - Increment && System < Reference + Tolerance + Increment,
           "Round %lu: system time advanced %I64d, performance counter %I64d\n", Round, System, Reference);

        /* Both timers are due by now */
        Timeout.QuadPart = 0;
        Status = KeWaitForSingleObject(&Inside, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = KeWaitForSingleObject(&After, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
        KeCancelTimer(&Inside);
        KeCancelTimer(&After);
    }
}

/* The performance counter and the interrupt time must agree on the rate. This
   is a consistency check: the clock interrupt counts lost periods with the
   same counter, so both can be off together; an outside reference is needed
   for the absolute rate */
static
VOID
TestRate(VOID)
{
    CLOCK_SAMPLE Start, End;
    LARGE_INTEGER Interval;
    LONGLONG Reference, Interrupt, Error, Best = 0;
    ULONG Attempt;
    BOOLEAN Measured = FALSE;

    for (Attempt = 0; Attempt < RATE_ATTEMPTS; Attempt++)
    {
        if (!SampleAfterInterrupt(&Start))
            continue;
        Interval.QuadPart = -40000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        if (!SampleAfterInterrupt(&End))
            continue;

        Reference = QpcElapsed(&Start, &End);
        Interrupt = (LONGLONG)(End.InterruptTime - Start.InterruptTime);
        Error = (Reference - Interrupt) * 100000 / Interrupt;
        trace("Attempt %lu: performance counter %I64d in %I64d of interrupt time (%I64d/100000)\n",
              Attempt, Reference, Interrupt, Error);
        if (!Measured || (Error < 0 ? -Error : Error) < (Best < 0 ? -Best : Best))
            Best = Error;
        Measured = TRUE;
        if (Best >= -100 && Best <= 100)
            break;
    }

    if (!Measured)
    {
        skip(FALSE, "No clock interrupt samples\n");
        return;
    }

    /* 0.1% */
    ok(Best >= -100 && Best <= 100, "Performance counter rate error %I64d/100000, frequency %I64d\n",
       Best, QpcFrequency);
}

START_TEST(KeClockTime)
{
    LARGE_INTEGER Frequency;
    BOOLEAN FromHal;

    KeQueryPerformanceCounter(&Frequency);
    QpcFrequency = Frequency.QuadPart;
    if (QpcFrequency <= 0)
    {
        skip(FALSE, "No performance counter frequency\n");
        return;
    }

    /* The clock interrupt runs on the boot processor */
    KeSetSystemAffinityThread(1);

    TestRate();
    if (LostTicksCounted(&FromHal))
        TestBlockedInterrupts();
    else if (FromHal)
        skip(FALSE, "The HAL does not count lost clock interrupts (no precise TSC calibration or no constant-rate TSC)\n");
    else
        skip(FALSE, "No HAL clock query, and neither an APIC HAL with an invariant TSC nor a hypervisor\n");

    KeRevertToUserAffinityThread();
}
