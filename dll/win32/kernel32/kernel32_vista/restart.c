/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Application restart and recovery registration
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "k32_vista.h"
#include <ndk/mmfuncs.h>

#define NDEBUG
#include <debug.h>

/*
 * There is no error reporting service here: nothing restarts or recovers a
 * process, so a registration is only kept for the Get functions.
 * kernel32_vista.dll exports these functions only below 0x600, where
 * kernel32 does not, so that a process has one registration.
 * NT 6 keeps it in the process environment block (WerRegistrationData),
 * where other processes read it; that slot is the start of the process heap
 * list in ReactOS's PEB, so only the calling process's own registration can
 * be read.
 */
static BOOL BaseRestartRegistered;
static DWORD BaseRestartFlags;
static WCHAR BaseRestartCommandLine[RESTART_MAX_CMD_LINE];

static APPLICATION_RECOVERY_CALLBACK BaseRecoveryCallback;
static PVOID BaseRecoveryParameter;
static DWORD BaseRecoveryPingInterval;
static DWORD BaseRecoveryFlags;

static
HRESULT
BasepCheckRegistrationProcess(
    _In_ HANDLE hProcess)
{
    PROCESS_BASIC_INFORMATION ProcessInfo;
    UCHAR Byte;
    NTSTATUS Status;

    if (!hProcess)
        return E_INVALIDARG;
    if (hProcess == NtCurrentProcess())
        return S_OK;

    /* As on NT 6, the handle must allow reading the process's memory */
    Status = NtQueryInformationProcess(hProcess,
                                       ProcessBasicInformation,
                                       &ProcessInfo,
                                       sizeof(ProcessInfo),
                                       NULL);
    if (NT_SUCCESS(Status))
    {
        Status = NtReadVirtualMemory(hProcess,
                                     ProcessInfo.PebBaseAddress,
                                     &Byte,
                                     sizeof(Byte),
                                     NULL);
    }
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));

    if (ProcessInfo.UniqueProcessId != (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess)
    {
        DPRINT1("FIXME: the registration of another process cannot be read\n");
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
RegisterApplicationRestart(
    _In_opt_ PCWSTR pwzCommandline,
    _In_ DWORD dwFlags)
{
    WCHAR CommandLine[RESTART_MAX_CMD_LINE];
    SIZE_T Length = 0;

    /* Captured before the lock is taken: the caller's buffer may fault */
    if (pwzCommandline)
    {
        _SEH2_TRY
        {
            while (Length < RESTART_MAX_CMD_LINE && pwzCommandline[Length])
            {
                CommandLine[Length] = pwzCommandline[Length];
                Length++;
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return E_INVALIDARG);
        }
        _SEH2_END;

        if (Length >= RESTART_MAX_CMD_LINE)
            return E_INVALIDARG;
    }
    CommandLine[Length] = UNICODE_NULL;

    RtlAcquirePebLock();
    RtlCopyMemory(BaseRestartCommandLine, CommandLine, (Length + 1) * sizeof(WCHAR));
    BaseRestartFlags = dwFlags;
    BaseRestartRegistered = TRUE;
    RtlReleasePebLock();

    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
UnregisterApplicationRestart(VOID)
{
    RtlAcquirePebLock();
    BaseRestartRegistered = FALSE;
    BaseRestartFlags = 0;
    BaseRestartCommandLine[0] = UNICODE_NULL;
    RtlReleasePebLock();

    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
GetApplicationRestartSettings(
    _In_ HANDLE hProcess,
    _Out_writes_opt_(*pcchSize) PWSTR pwzCommandline,
    _Inout_ PDWORD pcchSize,
    _Out_opt_ PDWORD pdwFlags)
{
    WCHAR CommandLine[RESTART_MAX_CMD_LINE];
    DWORD Needed, Flags;
    HRESULT hr;

    /* Without a buffer only the size can be asked for */
    if (!pcchSize || (!pwzCommandline && *pcchSize))
        return E_INVALIDARG;

    hr = BasepCheckRegistrationProcess(hProcess);
    if (FAILED(hr))
        return hr;

    RtlAcquirePebLock();
    Needed = BaseRestartRegistered ? (DWORD)wcslen(BaseRestartCommandLine) + 1 : 0;
    RtlCopyMemory(CommandLine, BaseRestartCommandLine, Needed * sizeof(WCHAR));
    Flags = BaseRestartFlags;
    RtlReleasePebLock();

    if (!Needed)
    {
        *pcchSize = 0;
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    if (*pcchSize < Needed)
    {
        if (pwzCommandline && *pcchSize)
            pwzCommandline[0] = UNICODE_NULL;
        *pcchSize = Needed;
        return pwzCommandline ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : S_OK;
    }

    RtlCopyMemory(pwzCommandline, CommandLine, Needed * sizeof(WCHAR));
    *pcchSize = Needed;
    if (pdwFlags)
        *pdwFlags = Flags;

    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
RegisterApplicationRecoveryCallback(
    _In_ APPLICATION_RECOVERY_CALLBACK pRecoveryCallback,
    _In_opt_ PVOID pvParameter,
    _In_ DWORD dwPingInterval,
    _In_ DWORD dwFlags)
{
    if (!pRecoveryCallback || dwPingInterval > RECOVERY_MAX_PING_INTERVAL)
        return E_INVALIDARG;

    RtlAcquirePebLock();
    BaseRecoveryCallback = pRecoveryCallback;
    BaseRecoveryParameter = pvParameter;
    BaseRecoveryPingInterval = dwPingInterval ? dwPingInterval : RECOVERY_DEFAULT_PING_INTERVAL;
    BaseRecoveryFlags = dwFlags;
    RtlReleasePebLock();

    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
UnregisterApplicationRecoveryCallback(VOID)
{
    RtlAcquirePebLock();
    BaseRecoveryCallback = NULL;
    BaseRecoveryParameter = NULL;
    BaseRecoveryPingInterval = 0;
    BaseRecoveryFlags = 0;
    RtlReleasePebLock();

    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
GetApplicationRecoveryCallback(
    _In_ HANDLE hProcess,
    _Out_ APPLICATION_RECOVERY_CALLBACK *pRecoveryCallback,
    _Outptr_opt_result_maybenull_ PVOID *ppvParameter,
    _Out_opt_ PDWORD pdwPingInterval,
    _Out_opt_ PDWORD pdwFlags)
{
    APPLICATION_RECOVERY_CALLBACK Callback;
    PVOID Parameter;
    DWORD PingInterval, Flags;
    HRESULT hr;

    if (!pRecoveryCallback)
        return E_INVALIDARG;

    hr = BasepCheckRegistrationProcess(hProcess);
    if (FAILED(hr))
        return hr;

    RtlAcquirePebLock();
    Callback = BaseRecoveryCallback;
    Parameter = BaseRecoveryParameter;
    PingInterval = BaseRecoveryPingInterval;
    Flags = BaseRecoveryFlags;
    RtlReleasePebLock();

    /* Nothing registered: no callback and zeros, as on NT 6 */
    *pRecoveryCallback = Callback;
    if (ppvParameter)
        *ppvParameter = Parameter;
    if (pdwPingInterval)
        *pdwPingInterval = PingInterval;
    if (pdwFlags)
        *pdwFlags = Flags;

    return S_OK;
}

/*
 * @implemented
 */
HRESULT
WINAPI
ApplicationRecoveryInProgress(
    _Out_ PBOOL pbCancelled)
{
    if (!pbCancelled)
        return E_INVALIDARG;

    /* Only a recovery callback that the error reporting service called may report progress */
    *pbCancelled = FALSE;
    return E_FAIL;
}

/*
 * @implemented
 */
VOID
WINAPI
ApplicationRecoveryFinished(
    _In_ BOOL bSuccess)
{
    /* No recovery is ever in progress */
}
