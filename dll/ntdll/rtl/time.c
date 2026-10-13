/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Performance counter and unbiased interrupt time queries
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/*
 * @implemented
 */
BOOL
NTAPI
RtlQueryPerformanceCounter(
    _Out_ PLARGE_INTEGER PerformanceCounter)
{
    NTSTATUS Status;

    Status = NtQueryPerformanceCounter(PerformanceCounter, NULL);
    return NT_SUCCESS(Status);
}

/*
 * @implemented
 */
BOOL
NTAPI
RtlQueryPerformanceFrequency(
    _Out_ PLARGE_INTEGER PerformanceFrequency)
{
    LARGE_INTEGER Counter, Frequency;
    NTSTATUS Status;

    /* The frequency is optional for the system call, not here */
    Status = NtQueryPerformanceCounter(&Counter, &Frequency);
    if (!NT_SUCCESS(Status))
        return FALSE;

    *PerformanceFrequency = Frequency;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
NTAPI
RtlQueryUnbiasedInterruptTime(
    _Out_ PULONGLONG UnbiasedTime)
{
    LARGE_INTEGER InterruptTime;
    ULONGLONG Bias = 0;

    if (!UnbiasedTime)
    {
        RtlSetLastWin32ErrorAndNtStatusFromNtStatus(STATUS_INVALID_PARAMETER);
        return FALSE;
    }

#if (NTDDI_VERSION >= NTDDI_VISTA)
    /* The bias is the time spent in sleep and hibernation; it must not change while the time is read */
    do
    {
        Bias = SharedUserData->InterruptTimeBias;
        InterruptTime = KiReadSystemTime(&SharedUserData->InterruptTime);
    } while (Bias != SharedUserData->InterruptTimeBias);
#else
    /* Shared user data of this NT version has no bias: the kernel does not take time spent asleep out */
    InterruptTime = KiReadSystemTime(&SharedUserData->InterruptTime);
#endif

    *UnbiasedTime = InterruptTime.QuadPart - Bias;
    return TRUE;
}
