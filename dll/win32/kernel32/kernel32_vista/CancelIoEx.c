/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CancelIoEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "k32_vista.h"

#include <ndk/iofuncs.h>

/*
 * @implemented
 */
BOOL
WINAPI
CancelIoEx(
    _In_ HANDLE hFile,
    _In_opt_ LPOVERLAPPED lpOverlapped)
{
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    /* An overlapped request is identified by its OVERLAPPED, which is also its I/O status block */
    Status = NtCancelIoFileEx(hFile, (PIO_STATUS_BLOCK)lpOverlapped, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
