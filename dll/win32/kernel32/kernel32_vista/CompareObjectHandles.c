/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     CompareObjectHandles
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "k32_vista.h"

#include <ndk/obfuncs.h>

/*
 * @implemented
 *
 * TRUE when both handles refer to the same kernel object. FALSE with ERROR_NOT_SAME_OBJECT when they do not,
 * with ERROR_INVALID_HANDLE when either is not a valid handle.
 */
BOOL
WINAPI
CompareObjectHandles(
    _In_ HANDLE hFirstObjectHandle,
    _In_ HANDLE hSecondObjectHandle)
{
    NTSTATUS Status;

    Status = NtCompareObjects(hFirstObjectHandle, hSecondObjectHandle);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
