/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     WaitOnAddress, exported by kernelbase and not by kernel32, as on Windows
 */

#include <stdarg.h>
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winternl.h>

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

    Status = RtlWaitOnAddress((const void *)Address, CompareAddress, AddressSize, Timeout);
    if (Status != STATUS_SUCCESS)
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }
    return TRUE;
}
