/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetThreadIdealProcessorEx and SetThreadIdealProcessorEx (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The ideal processor is a hint to the scheduler. The kernel keeps one processor of group 0 per thread.
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

BOOL
WINAPI
GetThreadIdealProcessorEx(
    _In_ HANDLE hThread,
    _Out_ PPROCESSOR_NUMBER lpIdealProcessor)
{
    NTSTATUS Status;

    Status = NtQueryInformationThread(hThread,
                                      ThreadIdealProcessorEx,
                                      lpIdealProcessor,
                                      sizeof(*lpIdealProcessor),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    return TRUE;
}

/* The previous processor is returned only when the new one was accepted */
BOOL
WINAPI
SetThreadIdealProcessorEx(
    _In_ HANDLE hThread,
    _In_ PPROCESSOR_NUMBER lpIdealProcessor,
    _Out_opt_ PPROCESSOR_NUMBER lpPreviousIdealProcessor)
{
    PROCESSOR_NUMBER Previous = {0}, New;
    NTSTATUS Status;

    if (!lpIdealProcessor)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    New = *lpIdealProcessor;

    if (lpPreviousIdealProcessor && !GetThreadIdealProcessorEx(hThread, &Previous))
        return FALSE;

    Status = NtSetInformationThread(hThread, ThreadIdealProcessorEx, &New, sizeof(New));
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (lpPreviousIdealProcessor)
        *lpPreviousIdealProcessor = Previous;
    return TRUE;
}
