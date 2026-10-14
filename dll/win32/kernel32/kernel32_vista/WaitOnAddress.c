/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     WaitOnAddress
 *
 * Kept apart from sync.c: kernel32 exports this at every export level, and sync.c also
 * holds functions that need the NT6 ntdll exports, which an NT5 ntdll does not have.
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

BOOL
WINAPI
WaitOnAddress(
    _In_ volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_ DWORD Milliseconds)
{
    NTSTATUS Status;
    LARGE_INTEGER Time;
    PLARGE_INTEGER Timeout = NULL;

    if (Milliseconds != INFINITE)
    {
        Time.QuadPart = (ULONGLONG)Milliseconds * -10000;
        Timeout = &Time;
    }

    Status = RtlWaitOnAddress(Address, CompareAddress, AddressSize, Timeout);
    if (Status != STATUS_SUCCESS)
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }
    return TRUE;
}

