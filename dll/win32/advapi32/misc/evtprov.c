/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     advapi32.dll event provider functions (Event Tracing for Windows): no sessions, no consumers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *              Behaviour of the stubs follows Wine's ntdll (dlls/ntdll/misc.c)
 */

#include <advapi32.h>
#include <evntprov.h>
#include <rpc.h>

WINE_DEFAULT_DEBUG_CHANNEL(advapi);

/* The other Event* provider exports forward to the Etw* functions of ntdll in advapi32.spec */
ULONG
WINAPI
EventWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    TRACE("(%I64x, %p, %I64x, %lx, %p, %p, %lu, %p) stub\n", RegHandle, EventDescriptor, Filter, Flags,
          ActivityId, RelatedActivityId, UserDataCount, UserData);
    return ERROR_SUCCESS;
}

#if (DLL_EXPORT_VERSION >= _WIN32_WINNT_WIN7)
ULONG
NTAPI
EtwEventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength);

/*
 * Windows 10 leaves the last error alone for an unsupported class and for a length that does not fit
 * (measured), and sets it for the failures that come after the checks of the arguments: an unusable handle,
 * a bad buffer, traits that cannot be set or are corrupt, and a descriptor type of class 3 that is not valid.
 */
ULONG
WINAPI
EventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    ULONG Status;

    Status = EtwEventSetInformation(RegHandle, InformationClass, EventInformation, InformationLength);
    if (Status == ERROR_INVALID_HANDLE || Status == ERROR_GEN_FAILURE || Status == ERROR_FILE_CORRUPT ||
        Status == ERROR_NOACCESS ||
        (Status == ERROR_INVALID_PARAMETER && InformationClass == 3 && InformationLength == 1 && EventInformation))
    {
        SetLastError(Status);
    }
    return Status;
}
#endif

/*
 * The activity id of a thread is kept in its TEB, where Windows Vista and later keep it (measured on
 * Windows Server 2008 R2 and Windows 10), and where the Instrumentation array of the older TEB layout
 * has room for it. A new thread has the zero id.
 */
#ifdef _WIN64
#define TEB_ACTIVITY_ID_OFFSET 0x1710
#else
#define TEB_ACTIVITY_ID_OFFSET 0xF50
#endif

static inline GUID *
GetThreadActivityId(VOID)
{
    return (GUID *)((PUCHAR)NtCurrentTeb() + TEB_ACTIVITY_ID_OFFSET);
}

static BOOL
CreateActivityId(OUT GUID *Id)
{
    RPC_STATUS Status = UuidCreate(Id);

    return Status == RPC_S_OK || Status == RPC_S_UUID_LOCAL_ONLY;
}

ULONG
WINAPI
EventActivityIdControl(
    _In_ ULONG ControlCode,
    _Inout_ LPGUID ActivityId)
{
    GUID Current, Created;

    TRACE("(%lu, %p)\n", ControlCode, ActivityId);

    if (ControlCode < EVENT_ACTIVITY_CTRL_GET_ID || ControlCode > EVENT_ACTIVITY_CTRL_CREATE_SET_ID ||
        ActivityId == NULL)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Current = *GetThreadActivityId();

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
            *ActivityId = Current;
            break;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            *GetThreadActivityId() = *ActivityId;
            break;

        case EVENT_ACTIVITY_CTRL_CREATE_ID:
            if (!CreateActivityId(&Created))
                return ERROR_FUNCTION_FAILED;
            *ActivityId = Created;
            break;

        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            *GetThreadActivityId() = *ActivityId;
            *ActivityId = Current;
            break;

        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            if (!CreateActivityId(&Created))
                return ERROR_FUNCTION_FAILED;
            *GetThreadActivityId() = Created;
            *ActivityId = Current;
            break;
    }

    return ERROR_SUCCESS;
}
