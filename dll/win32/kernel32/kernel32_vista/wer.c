/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Error Reporting process flags
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

/* There is no error reporting service to consult them, they are only kept for WerGetFlags */
static DWORD BaseWerFlags;

/*
 * @implemented
 */
HRESULT
WINAPI
WerSetFlags(
    _In_ DWORD dwFlags)
{
    InterlockedExchange((PLONG)&BaseWerFlags, (LONG)dwFlags);
    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
WerGetFlags(
    _In_ HANDLE hProcess,
    _Out_ PDWORD pdwFlags)
{
    PROCESS_BASIC_INFORMATION ProcessInfo;
    NTSTATUS Status;

    if (!pdwFlags)
        return E_INVALIDARG;

    if (hProcess != NtCurrentProcess())
    {
        Status = NtQueryInformationProcess(hProcess,
                                           ProcessBasicInformation,
                                           &ProcessInfo,
                                           sizeof(ProcessInfo),
                                           NULL);
        if (!NT_SUCCESS(Status))
            return HRESULT_FROM_NT(Status);

        if (ProcessInfo.UniqueProcessId != (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess)
        {
            DPRINT1("FIXME: WerGetFlags for another process is not supported\n");
            return E_NOTIMPL;
        }
    }

    *pdwFlags = BaseWerFlags;
    return S_OK;
}
