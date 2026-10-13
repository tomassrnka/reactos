/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Manifest-based event provider registration and event writing
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES ******************************************************************/

#include <ntdll.h>
#include <evntprov.h>

#define NDEBUG
#include <debug.h>

/*
 * ReactOS has no event tracing sessions, so no provider is ever enabled: the enable callback is never
 * called, EtwEventEnabled and EtwEventProviderEnabled return FALSE and a written event goes nowhere,
 * which is what Windows does for a provider that no session enables. The registrations are real, so
 * handles are checked as Windows checks them.
 */

typedef struct _ETW_REGISTRATION
{
    LIST_ENTRY ListEntry;
    GUID ProviderId;
    PENABLECALLBACK EnableCallback;
    PVOID CallbackContext;
} ETW_REGISTRATION, *PETW_REGISTRATION;

static LIST_ENTRY EtwpRegistrationList = { &EtwpRegistrationList, &EtwpRegistrationList };
static RTL_SRWLOCK EtwpRegistrationLock = RTL_SRWLOCK_INIT;

/* PRIVATE FUNCTIONS *********************************************************/

/* Called with EtwpRegistrationLock held */
static
PETW_REGISTRATION
EtwpFindRegistration(
    _In_ REGHANDLE RegHandle)
{
    PLIST_ENTRY Entry;

    if (!RegHandle)
        return NULL;

    for (Entry = EtwpRegistrationList.Flink; Entry != &EtwpRegistrationList; Entry = Entry->Flink)
    {
        if ((REGHANDLE)(ULONG_PTR)Entry == RegHandle)
            return CONTAINING_RECORD(Entry, ETW_REGISTRATION, ListEntry);
    }
    return NULL;
}

static
ULONG
EtwpCheckWrite(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG UserDataCount,
    _In_opt_ PEVENT_DATA_DESCRIPTOR UserData)
{
    PETW_REGISTRATION Registration;

    if (UserDataCount > MAX_EVENT_DATA_DESCRIPTORS || (UserDataCount && !UserData))
        return ERROR_INVALID_PARAMETER;

    RtlAcquireSRWLockShared(&EtwpRegistrationLock);
    Registration = EtwpFindRegistration(RegHandle);
    RtlReleaseSRWLockShared(&EtwpRegistrationLock);

    return Registration ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
ULONG
NTAPI
EtwEventRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    PETW_REGISTRATION Registration;

    if (!ProviderId || !RegHandle)
        return ERROR_INVALID_PARAMETER;

    Registration = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(*Registration));
    if (!Registration)
        return ERROR_NOT_ENOUGH_MEMORY;

    Registration->ProviderId = *ProviderId;
    Registration->EnableCallback = EnableCallback;
    Registration->CallbackContext = CallbackContext;

    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    InsertTailList(&EtwpRegistrationList, &Registration->ListEntry);
    RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

    *RegHandle = (REGHANDLE)(ULONG_PTR)&Registration->ListEntry;
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
ULONG
NTAPI
EtwEventUnregister(
    _In_ REGHANDLE RegHandle)
{
    PETW_REGISTRATION Registration;

    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    Registration = EtwpFindRegistration(RegHandle);
    if (Registration)
        RemoveEntryList(&Registration->ListEntry);
    RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

    if (!Registration)
        return ERROR_INVALID_HANDLE;

    RtlFreeHeap(RtlGetProcessHeap(), 0, Registration);
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
EtwEventEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    /* No session enables any provider */
    return FALSE;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
EtwEventProviderEnabled(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    /* No session enables any provider */
    return FALSE;
}

/*
 * @implemented
 */
ULONG
NTAPI
EtwEventWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    if (!EventDescriptor)
        return ERROR_INVALID_PARAMETER;

    /* Nothing listens: the event is dropped once the arguments are checked */
    return EtwpCheckWrite(RegHandle, UserDataCount, UserData);
}

/*
 * @implemented
 */
ULONG
NTAPI
EtwEventWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    if (!EventDescriptor)
        return ERROR_INVALID_PARAMETER;

    return EtwpCheckWrite(RegHandle, UserDataCount, UserData);
}

/*
 * @implemented
 */
ULONG
NTAPI
EtwEventWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ PCWSTR String)
{
    if (!String)
        return ERROR_INVALID_PARAMETER;

    return EtwpCheckWrite(RegHandle, 0, NULL);
}
