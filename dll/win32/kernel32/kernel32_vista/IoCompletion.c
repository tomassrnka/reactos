/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Vista+ I/O completion port functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "k32_vista.h"

#include <ndk/iofuncs.h>

/* The entries are handed to NtRemoveIoCompletionEx as they are */
C_ASSERT(sizeof(OVERLAPPED_ENTRY) == sizeof(FILE_IO_COMPLETION_INFORMATION));
C_ASSERT(FIELD_OFFSET(OVERLAPPED_ENTRY, lpCompletionKey) == FIELD_OFFSET(FILE_IO_COMPLETION_INFORMATION, KeyContext));
C_ASSERT(FIELD_OFFSET(OVERLAPPED_ENTRY, lpOverlapped) == FIELD_OFFSET(FILE_IO_COMPLETION_INFORMATION, ApcContext));
C_ASSERT(FIELD_OFFSET(OVERLAPPED_ENTRY, Internal) == FIELD_OFFSET(FILE_IO_COMPLETION_INFORMATION, IoStatusBlock.Status));
C_ASSERT(FIELD_OFFSET(OVERLAPPED_ENTRY, dwNumberOfBytesTransferred) == FIELD_OFFSET(FILE_IO_COMPLETION_INFORMATION, IoStatusBlock.Information));

/*
 * @implemented
 */
BOOL
WINAPI
GetQueuedCompletionStatusEx(
    _In_ HANDLE CompletionPort,
    _Out_writes_to_(ulCount, *ulNumEntriesRemoved) LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    _In_ ULONG ulCount,
    _Out_ PULONG ulNumEntriesRemoved,
    _In_ DWORD dwMilliseconds,
    _In_ BOOL fAlertable)
{
    NTSTATUS Status;
    LARGE_INTEGER Time;
    PLARGE_INTEGER TimePtr = NULL;
    RTL_CALLER_ALLOCATED_ACTIVATION_CONTEXT_STACK_FRAME ActCtx;

    /* Convert the timeout, INFINITE means no timeout */
    if (dwMilliseconds != INFINITE)
    {
        Time.QuadPart = dwMilliseconds * -10000LL;
        TimePtr = &Time;
    }

    /* APCs must execute with the default activation context */
    if (fAlertable)
    {
        RtlZeroMemory(&ActCtx, sizeof(ActCtx));
        ActCtx.Size = sizeof(ActCtx);
        ActCtx.Format = RTL_CALLER_ALLOCATED_ACTIVATION_CONTEXT_STACK_FRAME_FORMAT_WHISTLER;
        RtlActivateActivationContextUnsafeFast(&ActCtx, NULL);
    }

    /* An alert does not end an alertable wait, as for the other waits */
    do
    {
        Status = NtRemoveIoCompletionEx(CompletionPort,
                                        (PFILE_IO_COMPLETION_INFORMATION)lpCompletionPortEntries,
                                        ulCount,
                                        ulNumEntriesRemoved,
                                        TimePtr,
                                        (fAlertable != FALSE));
    } while ((Status == STATUS_ALERTED) && (fAlertable));

    if (fAlertable) RtlDeactivateActivationContextUnsafeFast(&ActCtx);

    if (Status == STATUS_SUCCESS)
        return TRUE;

    if (Status == STATUS_TIMEOUT)
        SetLastError(WAIT_TIMEOUT);
    else if (Status == STATUS_USER_APC)
        SetLastError(WAIT_IO_COMPLETION);
    else
        BaseSetLastNTError(Status);

    return FALSE;
}
