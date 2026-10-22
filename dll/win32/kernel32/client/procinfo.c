/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetProcessInformation and SetProcessInformation of Windows 8 and later
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/*
 * The kernel has no page priority and no leap second setting for processes, so what a process asks for is kept
 * here, in the process that asks: the memory priority (a hint for the page replacement) and the leap second flag
 * (a hint for the time functions). Nothing acts on them. The power throttling state is a hint for the scheduler
 * that nothing here could act on either; a request is checked and accepted, and Windows gives no way to read it
 * back. A handle of another process is checked and accepted, and the value is not kept, as the state of that
 * process is not in this one; reading it gives the default.
 *
 * The classes, the sizes, the values and the errors are the ones that Windows 10 22H2 (build 19045) showed to a
 * probe on the reference machine: Get takes the memory priority, the leap second information, the app memory
 * information and the protection level, Set takes the memory priority, the power throttling state and the leap
 * second information; every other class is ERROR_INVALID_PARAMETER.
 */

/* GLOBALS ********************************************************************/

static ULONG BasepProcessMemoryPriority = MEMORY_PRIORITY_NORMAL;
static ULONG BasepProcessLeapSecondFlags;

/* FUNCTIONS ******************************************************************/

/* Whether a handle of a process (a handle that was already checked) is the calling process */
static
BOOLEAN
BasepIsCurrentProcess(
    _In_ HANDLE ProcessHandle)
{
    PROCESS_BASIC_INFORMATION BasicInfo;
    NTSTATUS Status;

    if (ProcessHandle == NtCurrentProcess())
        return TRUE;

    Status = NtQueryInformationProcess(ProcessHandle, ProcessBasicInformation, &BasicInfo, sizeof(BasicInfo), NULL);
    return NT_SUCCESS(Status) && BasicInfo.UniqueProcessId == (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess;
}

/* The pseudo handle of the calling process is a process handle that all access is given to */
static
NTSTATUS
BasepCheckProcessHandle(
    _In_ HANDLE ProcessHandle,
    _In_ ACCESS_MASK DesiredAccess)
{
    if (ProcessHandle == NtCurrentProcess())
        return STATUS_SUCCESS;

    return BasepCheckHandleType(ProcessHandle, L"Process", DesiredAccess);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetProcessInformation(
    _In_ HANDLE hProcess,
    _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
    _Out_writes_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
    _In_ DWORD ProcessInformationSize)
{
    ULONG Size;
    BOOLEAN Current;
    PROCESS_LEAP_SECOND_INFO Data;
    NTSTATUS Status;

    switch (ProcessInformationClass)
    {
        case ProcessMemoryPriority:
            Size = sizeof(MEMORY_PRIORITY_INFORMATION);
            break;

        case ProcessProtectionLevelInfo:
            Size = sizeof(ULONG);
            break;

        case ProcessLeapSecondInfo:
            Size = sizeof(PROCESS_LEAP_SECOND_INFO);
            break;

        case ProcessAppMemoryInfo:
            /* The commit of an app is not accounted for here */
            BaseSetLastNTError(STATUS_NOT_SUPPORTED);
            return FALSE;

        default:
            /* The power throttling state, among others, cannot be read */
            BaseSetLastNTError(STATUS_INVALID_PARAMETER);
            return FALSE;
    }

    if (ProcessInformationSize != Size)
    {
        BaseSetLastNTError(STATUS_INFO_LENGTH_MISMATCH);
        return FALSE;
    }

    if (!ProcessInformation)
    {
        BaseSetLastNTError(STATUS_ACCESS_VIOLATION);
        return FALSE;
    }

    Status = BasepCheckProcessHandle(hProcess, PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    Current = BasepIsCurrentProcess(hProcess);

    RtlZeroMemory(&Data, sizeof(Data));
    RtlEnterCriticalSection(&BaseDllDirectoryLock);
    switch (ProcessInformationClass)
    {
        case ProcessMemoryPriority:
            Data.Flags = Current ? BasepProcessMemoryPriority : MEMORY_PRIORITY_NORMAL;
            break;

        case ProcessProtectionLevelInfo:
            /* No process is protected here */
            Data.Flags = PROTECTION_LEVEL_NONE;
            break;

        default:
            if (Current)
                Data.Flags = BasepProcessLeapSecondFlags;
            break;
    }
    RtlLeaveCriticalSection(&BaseDllDirectoryLock);

    _SEH2_TRY
    {
        RtlCopyMemory(ProcessInformation, &Data, Size);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = STATUS_ACCESS_VIOLATION;
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessInformation(
    _In_ HANDLE hProcess,
    _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
    _In_reads_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
    _In_ DWORD ProcessInformationSize)
{
    ULONG Size;
    union
    {
        ULONG Value;
        PROCESS_POWER_THROTTLING_STATE Throttling;
        PROCESS_LEAP_SECOND_INFO LeapSecond;
    } Data;
    NTSTATUS Status = STATUS_SUCCESS;

    switch (ProcessInformationClass)
    {
        case ProcessMemoryPriority:
            Size = sizeof(MEMORY_PRIORITY_INFORMATION);
            break;

        case ProcessPowerThrottling:
            Size = sizeof(PROCESS_POWER_THROTTLING_STATE);
            break;

        case ProcessLeapSecondInfo:
            Size = sizeof(PROCESS_LEAP_SECOND_INFO);
            break;

        default:
            /* Only these three can be set */
            BaseSetLastNTError(STATUS_INVALID_PARAMETER);
            return FALSE;
    }

    if (ProcessInformationSize != Size)
    {
        BaseSetLastNTError(STATUS_INFO_LENGTH_MISMATCH);
        return FALSE;
    }

    if (!ProcessInformation)
    {
        BaseSetLastNTError(STATUS_ACCESS_VIOLATION);
        return FALSE;
    }

    RtlZeroMemory(&Data, sizeof(Data));
    _SEH2_TRY
    {
        RtlCopyMemory(&Data, ProcessInformation, Size);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = STATUS_ACCESS_VIOLATION;
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* The value is checked before the handle */
    switch (ProcessInformationClass)
    {
        case ProcessMemoryPriority:
            /* 1 (very low) to 5 (normal) */
            if (Data.Value < MEMORY_PRIORITY_VERY_LOW || Data.Value > MEMORY_PRIORITY_NORMAL)
                Status = STATUS_INVALID_PARAMETER;
            break;

        case ProcessPowerThrottling:
            /* The version is 1, and a state needs its control bit */
            if (Data.Throttling.Version != PROCESS_POWER_THROTTLING_CURRENT_VERSION ||
                (Data.Throttling.ControlMask & ~PROCESS_POWER_THROTTLING_VALID_FLAGS) ||
                (Data.Throttling.StateMask & ~Data.Throttling.ControlMask))
            {
                Status = STATUS_INVALID_PARAMETER;
            }
            break;

        default:
            /* The reserved field is not looked at */
            if (Data.LeapSecond.Flags & ~PROCESS_LEAP_SECOND_INFO_VALID_FLAGS)
                Status = STATUS_INVALID_PARAMETER;
            break;
    }
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    Status = BasepCheckProcessHandle(hProcess, PROCESS_SET_INFORMATION);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (ProcessInformationClass != ProcessPowerThrottling && BasepIsCurrentProcess(hProcess))
    {
        RtlEnterCriticalSection(&BaseDllDirectoryLock);
        if (ProcessInformationClass == ProcessMemoryPriority)
            BasepProcessMemoryPriority = Data.Value;
        else
            BasepProcessLeapSecondFlags = Data.LeapSecond.Flags;
        RtlLeaveCriticalSection(&BaseDllDirectoryLock);
    }

    return TRUE;
}

/* EOF */
