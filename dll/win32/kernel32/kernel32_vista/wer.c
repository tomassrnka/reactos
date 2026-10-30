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

/*
 * @implemented
 *
 * A runtime exception module is a DLL the error reporting service loads, out of process, when this
 * process crashes, so that it can claim the crash (the CLR registers mscordacwks.dll). Windows keeps
 * up to WER_MAX_REGISTERED_RUNTIME_EXCEPTION_MODULES registrations per process, keyed by the module
 * path (compared case-sensitively) and the context; registering the same pair again succeeds
 * without taking another slot. The crash reporter here (drwtsn32) does not load such modules yet, but the
 * registration contract is kept: the CLR refuses to start when it cannot register.
 */
#define BASE_WER_MAX_RUNTIME_MODULES 16

typedef struct _BASE_WER_RUNTIME_MODULE
{
    PWSTR Path;
    PVOID Context;
} BASE_WER_RUNTIME_MODULE;

static BASE_WER_RUNTIME_MODULE BaseWerRuntimeModules[BASE_WER_MAX_RUNTIME_MODULES];
static RTL_SRWLOCK BaseWerRuntimeModulesLock = RTL_SRWLOCK_INIT;

HRESULT
WINAPI
WerRegisterRuntimeExceptionModule(
    _In_ PCWSTR pwszOutOfProcessCallbackDll,
    _In_opt_ PVOID pContext)
{
    SIZE_T Size;
    PWSTR Copy;
    ULONG i, Free = BASE_WER_MAX_RUNTIME_MODULES;

    if (!pwszOutOfProcessCallbackDll || !*pwszOutOfProcessCallbackDll)
        return E_INVALIDARG;

    Size = (wcslen(pwszOutOfProcessCallbackDll) + 1) * sizeof(WCHAR);
    Copy = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
    if (!Copy)
        return E_OUTOFMEMORY;
    RtlCopyMemory(Copy, pwszOutOfProcessCallbackDll, Size);

    RtlAcquireSRWLockExclusive(&BaseWerRuntimeModulesLock);
    for (i = 0; i < BASE_WER_MAX_RUNTIME_MODULES; i++)
    {
        if (!BaseWerRuntimeModules[i].Path)
        {
            if (Free == BASE_WER_MAX_RUNTIME_MODULES)
                Free = i;
        }
        else if (BaseWerRuntimeModules[i].Context == pContext &&
                 !wcscmp(BaseWerRuntimeModules[i].Path, pwszOutOfProcessCallbackDll))
        {
            /* Already registered */
            RtlReleaseSRWLockExclusive(&BaseWerRuntimeModulesLock);
            RtlFreeHeap(RtlGetProcessHeap(), 0, Copy);
            return S_OK;
        }
    }
    if (Free < BASE_WER_MAX_RUNTIME_MODULES)
    {
        BaseWerRuntimeModules[Free].Path = Copy;
        BaseWerRuntimeModules[Free].Context = pContext;
        Copy = NULL;
    }
    RtlReleaseSRWLockExclusive(&BaseWerRuntimeModulesLock);

    if (Copy)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Copy);
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    DPRINT("WerRegisterRuntimeExceptionModule(%S, %p): registered in slot %lu\n",
           pwszOutOfProcessCallbackDll, pContext, Free);
    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
WerUnregisterRuntimeExceptionModule(
    _In_ PCWSTR pwszOutOfProcessCallbackDll,
    _In_opt_ PVOID pContext)
{
    PWSTR Path = NULL;
    ULONG i;

    if (!pwszOutOfProcessCallbackDll || !*pwszOutOfProcessCallbackDll)
        return E_INVALIDARG;

    RtlAcquireSRWLockExclusive(&BaseWerRuntimeModulesLock);
    for (i = 0; i < BASE_WER_MAX_RUNTIME_MODULES; i++)
    {
        if (BaseWerRuntimeModules[i].Path &&
            BaseWerRuntimeModules[i].Context == pContext &&
            !wcscmp(BaseWerRuntimeModules[i].Path, pwszOutOfProcessCallbackDll))
        {
            Path = BaseWerRuntimeModules[i].Path;
            BaseWerRuntimeModules[i].Path = NULL;
            BaseWerRuntimeModules[i].Context = NULL;
            break;
        }
    }
    RtlReleaseSRWLockExclusive(&BaseWerRuntimeModulesLock);

    if (!Path)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Path);
    return S_OK;
}
