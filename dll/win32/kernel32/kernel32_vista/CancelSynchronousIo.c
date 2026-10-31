/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CancelSynchronousIo
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "k32_vista.h"

#include <ndk/iofuncs.h>

/*
 * @implemented
 *
 * Cancels the synchronous I/O the thread is waiting for. The handle needs THREAD_TERMINATE. It fails with
 * ERROR_NOT_FOUND when the thread has no synchronous request in progress.
 */
BOOL
WINAPI
CancelSynchronousIo(
    _In_ HANDLE hThread)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    Status = NtCancelSynchronousIoFile(hThread, NULL, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
