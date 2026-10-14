/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/tsc.c
 * PURPOSE:         HAL Routines for TSC handling
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include "tsc.h"
#include "apicp.h"
#define NDEBUG
#include <debug.h>

LARGE_INTEGER HalpCpuClockFrequency = {{INITIAL_STALL_COUNT * 1000000}};

UCHAR TscCalibrationPhase;
ULONG64 TscCalibrationArray[NUM_SAMPLES];

/* TRUE when the frequency was measured against the ACPI power management timer
   (five windows agreeing to 1/10000 and to the RTC calibration); the clock
   interrupt then counts lost periods with it */
BOOLEAN HalpTscCalibrated;

#define PM_TIMER_FREQUENCY 3579545
#define PM_TIMER_WINDOW (PM_TIMER_FREQUENCY / 20) /* 50 ms */
#define PM_TIMER_WINDOWS 8
#define PM_TIMER_AGREEING 5
#define PM_TIMER_LONGEST 4 /* seconds, below the 4.687 s wrap of a 24-bit timer */
#define PM_TIMER_STEPS 16  /* fewer steps than this can each take over 1/16 of a window */

#define RTC_MODE 6 /* Mode 6 is 1024 Hz */
#define SAMPLE_FREQUENCY ((32768 << 1) >> RTC_MODE)

/* PRIVATE FUNCTIONS *********************************************************/

static
ULONG64
DoLinearRegression(
    ULONG XMax,
    ULONG64 *ArrayY)
{
    ULONG X, SumXX;
    ULONG64 SumXY;

    /* Calculate the sum of the squares of X */
    SumXX = (XMax * (XMax + 1) * (2*XMax + 1)) / 6;

    /* Calculate the sum of the differences to the first value
       weighted by x */
    for (SumXY = 0, X = 1; X <= XMax; X++)
    {
         SumXY += X * (ArrayY[X] - ArrayY[0]);
    }

    /* Account for sample frequency */
    SumXY *= SAMPLE_FREQUENCY;

    /* Return the quotient of the sums */
    return (SumXY + (SumXX/2)) / SumXX;
}

/* Reads the TSC and the power management timer together; returns the TSC in the
   middle of the closest of several reads and the TSC ticks the read took */
static
ULONG64
HalpReadTscAndPmTimer(
    _In_ USHORT Port,
    _In_ ULONG Mask,
    _Out_ PULONG PmTimer,
    _Out_ PULONG64 Bracket)
{
    ULONG64 Before, After, Tsc = 0;
    ULONG Value, i;

    *Bracket = ~0ULL;
    *PmTimer = 0;
    for (i = 0; i < 8; i++)
    {
        Before = __rdtsc();
        Value = __indword(Port) & Mask;
        After = __rdtsc();
        if (After - Before < *Bracket)
        {
            *Bracket = After - Before;
            *PmTimer = Value;
            Tsc = Before + *Bracket / 2;
        }
    }
    return Tsc;
}

/* Sorts a few values in place */
static
VOID
HalpSortValues(
    _Inout_updates_(Count) PULONG64 Values,
    _In_ ULONG Count)
{
    ULONG64 Value;
    ULONG i, j;

    for (i = 1; i < Count; i++)
    {
        Value = Values[i];
        for (j = i; (j > 0) && (Values[j - 1] > Value); j--)
            Values[j] = Values[j - 1];
        Values[j] = Value;
    }
}

/* Keeps the longest steps between polls of the timer (TSC ticks and counts) */
static
VOID
HalpKeepLongStep(
    _In_ ULONG64 Ticks,
    _In_ ULONG64 Count,
    _Inout_updates_(PM_TIMER_STEPS) PULONG64 StepTicks,
    _Inout_updates_(PM_TIMER_STEPS) PULONG64 StepCounts,
    _Inout_ PULONG Shortest)
{
    ULONG i;

    if (Ticks <= StepTicks[*Shortest])
        return;
    StepTicks[*Shortest] = Ticks;
    StepCounts[*Shortest] = Count;
    for (*Shortest = 0, i = 1; i < PM_TIMER_STEPS; i++)
    {
        if (StepTicks[i] < StepTicks[*Shortest])
            *Shortest = i;
    }
}

/*!
    \brief Measures the TSC frequency against the ACPI power management timer.

    Eight consecutive windows of 50 ms of the timer, each ended by a close
    read of the TSC and the timer (the closest of several reads), each give a
    frequency. The timer is polled, so late or lost interrupts do not matter.
    A pause between two polls longer than a wrap of the timer (4.7 s for 24
    bits) hides the wrap. Such a step between polls takes more than 1/16 of
    its window by the TSC (unless 16 steps of a window each hide a wrap),
    while the timer counts far less than its share. So a window is dropped
    when the TSC ticks per count of a step that takes more than 1/16 of it
    differ from those of the rest of the window by more than 1/8 (a pause the
    timer counted through agrees), or when that rest is under 1/4 of the
    window's count. This needs no other clock.
    A window that took more than twice or less than half the median TSC
    duration of the windows is dropped: a pause there could hide a wrap of the
    timer (4.7 s for 24 bits, nearly 100 windows long), and the TSC sees the
    pause whatever its rate. The result is the median of the remaining
    frequencies, and at least five of them must agree with it to 1/10000.
    As a second guard, a window longer than 4 s by the RTC calibration is
    dropped and the result must agree with that calibration to 1/8. Otherwise
    the measurement fails and the RTC calibration is used, without counting
    lost clock periods.
*/
static
BOOLEAN
HalpCalibrateTscWithPmTimer(
    _In_ ULONG64 RtcFrequency)
{
    ULONG64 Ticks[PM_TIMER_WINDOWS], Counts[PM_TIMER_WINDOWS], Frequency[PM_TIMER_WINDOWS];
    ULONG64 Sorted[PM_TIMER_WINDOWS], Last, End, Bracket, Count, Median, Difference;
    ULONG64 StepTicks[PM_TIMER_STEPS], StepCounts[PM_TIMER_STEPS];
    ULONG64 Tsc, Previous, Step, RestTicks, RestCount, Expected, Actual;
    BOOLEAN Suspect[PM_TIMER_WINDOWS];
    ULONG Port, Mask, Timer, Now, Window, Valid, Agreeing, Unchanged, Shortest, i;
    ULONG_PTR Flags;

    Port = HalpGetAcpiTimerPort(&Mask);
    if (!Port)
        return FALSE;

    Flags = __readeflags();
    _disable();
    Last = HalpReadTscAndPmTimer((USHORT)Port, Mask, &Timer, &Bracket);
    for (Window = 0; Window < PM_TIMER_WINDOWS; Window++)
    {
        Previous = Last;
        RtlZeroMemory(StepTicks, sizeof(StepTicks));
        RtlZeroMemory(StepCounts, sizeof(StepCounts));
        Shortest = 0;
        for (Count = 0, Unchanged = 0; Count < PM_TIMER_WINDOW; )
        {
            Now = __indword((USHORT)Port) & Mask;
            Tsc = __rdtsc();

            /* Give up on a timer that does not count */
            if ((Now == Timer) && (++Unchanged > (1UL << 24)))
            {
                __writeeflags(Flags);
                return FALSE;
            }
            if (Now != Timer)
                Unchanged = 0;
            Step = (Now - Timer) & Mask;
            HalpKeepLongStep(Tsc - Previous, Step, StepTicks, StepCounts, &Shortest);
            Count += Step;
            Timer = Now;
            Previous = Tsc;
        }
        End = HalpReadTscAndPmTimer((USHORT)Port, Mask, &Now, &Bracket);
        Step = (Now - Timer) & Mask;
        HalpKeepLongStep(End - Previous, Step, StepTicks, StepCounts, &Shortest);
        Count += Step;
        Timer = Now;
        Ticks[Window] = End - Last;
        Counts[Window] = Count;
        Last = End;

        /* Steps that could hide a wrap, against the rest of the window; the
           bounds keep the products below 2^63 */
        RestTicks = Ticks[Window];
        RestCount = Count;
        for (i = 0; i < PM_TIMER_STEPS; i++)
        {
            if (16 * StepTicks[i] > Ticks[Window])
            {
                RestTicks -= StepTicks[i];
                RestCount -= StepCounts[i];
            }
        }
        Suspect[Window] = (Count > 2 * PM_TIMER_WINDOW) || (Ticks[Window] >= (1ULL << 44)) ||
                          (4 * RestCount < PM_TIMER_WINDOW);
        for (i = 0; !Suspect[Window] && (i < PM_TIMER_STEPS); i++)
        {
            if (16 * StepTicks[i] <= Ticks[Window])
                continue;
            Expected = StepTicks[i] * RestCount;
            Actual = StepCounts[i] * RestTicks;
            Difference = (Expected > Actual) ? Expected - Actual : Actual - Expected;
            Suspect[Window] = (Difference > Expected / 8);
        }
    }
    __writeeflags(Flags);

    /* Median TSC duration of a window */
    for (Window = 0; Window < PM_TIMER_WINDOWS; Window++)
        Sorted[Window] = Ticks[Window];
    HalpSortValues(Sorted, PM_TIMER_WINDOWS);
    Median = Sorted[PM_TIMER_WINDOWS / 2];

    /* Frequencies of the windows of about that duration */
    for (Valid = 0, Window = 0; Window < PM_TIMER_WINDOWS; Window++)
    {
        if (Suspect[Window] || !Ticks[Window] || (Ticks[Window] > 2 * Median) ||
            (2 * Ticks[Window] < Median) || (Ticks[Window] / PM_TIMER_LONGEST > RtcFrequency))
            continue;
        Frequency[Valid++] = (Ticks[Window] / Counts[Window]) * PM_TIMER_FREQUENCY +
            ((Ticks[Window] % Counts[Window]) * PM_TIMER_FREQUENCY + Counts[Window] / 2) / Counts[Window];
    }
    if (Valid < PM_TIMER_AGREEING)
        return FALSE;
    HalpSortValues(Frequency, Valid);
    Median = Frequency[Valid / 2];

    for (Agreeing = 0, Window = 0; Window < Valid; Window++)
    {
        Difference = (Frequency[Window] > Median) ? Frequency[Window] - Median : Median - Frequency[Window];
        if (Difference <= Median / 10000)
            Agreeing++;
    }
    if (Agreeing < PM_TIMER_AGREEING)
        return FALSE;

    Difference = (Median > RtcFrequency) ? Median - RtcFrequency : RtcFrequency - Median;
    if (Difference > RtcFrequency / 8)
        return FALSE;

    HalpCpuClockFrequency.QuadPart = Median;
    HalpTscCalibrated = TRUE;
    return TRUE;
}

/* Measures the TSC frequency over four RTC interrupts */
static
VOID
HalpCalibrateTscWithRtc(VOID)
{
    ULONG_PTR Flags;
    PVOID PreviousHandler;
    UCHAR RegisterA, RegisterB;

     /* Save flags and disable interrupts */
    Flags = __readeflags();
    _disable();

    /* Enable the periodic interrupt in the CMOS */
    RegisterB = HalpReadCmos(RTC_REGISTER_B);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB | RTC_REG_B_PI);

    /* Modify register A to RTC_MODE to get SAMPLE_FREQUENCY */
    RegisterA = HalpReadCmos(RTC_REGISTER_A);
    RegisterA = (RegisterA & 0xF0) | RTC_MODE;
    HalpWriteCmos(RTC_REGISTER_A, RegisterA);

    /* Save old IDT entry */
    PreviousHandler = KeQueryInterruptHandler(APIC_CLOCK_VECTOR);

    /* Set the calibration ISR */
    KeRegisterInterruptHandler(APIC_CLOCK_VECTOR, TscCalibrationISR);

    /* Enable the timer interrupt */
    HalEnableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL, Latched);

    /* Read register C, so that the next interrupt can happen */
    HalpReadCmos(RTC_REGISTER_C);

    /* Wait for completion */
    _enable();
    while (TscCalibrationPhase < NUM_SAMPLES) _ReadWriteBarrier();
    _disable();

    /* Disable the periodic interrupt in the CMOS */
    HalpWriteCmos(RTC_REGISTER_B, RegisterB & ~RTC_REG_B_PI);

    /* Disable the timer interrupt */
    HalDisableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL);

    /* Restore the previous handler */
    KeRegisterInterruptHandler(APIC_CLOCK_VECTOR, PreviousHandler);

    /* Calculate an average, using simplified linear regression */
    HalpCpuClockFrequency.QuadPart = DoLinearRegression(NUM_SAMPLES - 1,
                                                        TscCalibrationArray);

    /* Restore flags */
    __writeeflags(Flags);

}

VOID
NTAPI
HalpInitializeTsc(VOID)
{
    /* Check if the CPU supports RDTSC */
    if (!(KeGetCurrentPrcb()->FeatureBits & KF_RDTSC))
    {
        KeBugCheck(HAL_INITIALIZATION_FAILED);
    }

    /* Count RTC interrupts, then refine with the power management timer;
       the TSC is not reset (on this processor alone it would leave the TSCs
       of the processors out of step) */
    HalpCalibrateTscWithRtc();
    HalpCalibrateTscWithPmTimer((ULONG64)HalpCpuClockFrequency.QuadPart);
}

CODE_SEG("INIT")
VOID
NTAPI
HalpCalibrateStallExecution(VOID)
{
    // Timer interrupt is now active

    HalpInitializeTsc();

    KeGetPcr()->StallScaleFactor = (ULONG)(HalpCpuClockFrequency.QuadPart / 1000000);
}

/* PUBLIC FUNCTIONS ***********************************************************/

LARGE_INTEGER
NTAPI
KeQueryPerformanceCounter(
    OUT PLARGE_INTEGER PerformanceFrequency OPTIONAL)
{
    LARGE_INTEGER Result;

    /* Make sure it's calibrated */
    ASSERT(HalpCpuClockFrequency.QuadPart != 0);

    /* Does the caller want the frequency? */
    if (PerformanceFrequency)
    {
        /* Return tsc frequency */
        *PerformanceFrequency = HalpCpuClockFrequency;
    }

    /* Return the current value */
    Result.QuadPart = __rdtsc();
    return Result;
}

VOID
NTAPI
KeStallExecutionProcessor(ULONG MicroSeconds)
{
    ULONG64 StartTime, EndTime;

    /* Get the initial time */
    StartTime = __rdtsc();

    /* Calculate the ending time */
    EndTime = StartTime + KeGetPcr()->StallScaleFactor * MicroSeconds;

    /* Loop until time is elapsed */
    while (__rdtsc() < EndTime);
}

VOID
NTAPI
HalCalibratePerformanceCounter(
    IN volatile PLONG Count,
    IN ULONGLONG NewCount)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
}

