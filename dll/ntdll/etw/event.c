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
    ULONGLONG Serial;
    BOOLEAN TraitsSet;
} ETW_REGISTRATION, *PETW_REGISTRATION;

static LIST_ENTRY EtwpRegistrationList = { &EtwpRegistrationList, &EtwpRegistrationList };
static RTL_SRWLOCK EtwpRegistrationLock = RTL_SRWLOCK_INIT;
/* Handles are heap addresses, which a later registration can reuse: the serial tells them apart */
static ULONGLONG EtwpRegistrationSerial;

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
    Registration->TraitsSet = FALSE;

    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    Registration->Serial = ++EtwpRegistrationSerial;
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
 * The provider traits of a registration are the size of the block, the provider name as a zero-terminated
 * string, and then any number of extensions, each of them a size, a type and data. Windows accepts a
 * block only when the sizes add up exactly (measured on Windows 10: any name, any extension type).
 */
static
BOOLEAN
EtwpTraitsAreValid(
    _In_reads_bytes_(Length) const UCHAR *Traits,
    _In_ ULONG Length)
{
    ULONG Offset, ExtensionSize;

    if (((ULONG)Traits[0] | ((ULONG)Traits[1] << 8)) != Length)
        return FALSE;

    for (Offset = 2; Offset < Length && Traits[Offset] != 0; Offset++)
        ;
    if (Offset >= Length)
        return FALSE;
    Offset++;

    while (Offset < Length)
    {
        if (Length - Offset < 3)
            return FALSE;
        ExtensionSize = (ULONG)Traits[Offset] | ((ULONG)Traits[Offset + 1] << 8);
        if (ExtensionSize < 3 || ExtensionSize > Length - Offset)
            return FALSE;
        Offset += ExtensionSize;
    }
    return TRUE;
}

/*
 * ReactOS keeps no provider state beyond the registration, so the traits are checked and then dropped. The
 * answers, including their order (class, length, handle, contents), were measured on Windows 10; Windows
 * 7 has no such function. The caller's buffer is read with the registration lock released, so that a
 * vectored exception handler of the caller can use the provider functions.
 */
ULONG
NTAPI
EtwEventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    PETW_REGISTRATION Registration;
    ULONGLONG Serial = 0;
    ULONG Status = ERROR_SUCCESS;

    switch (InformationClass)
    {
        case 0: /* EventProviderBinaryTrackInfo: takes no data */
            if (InformationLength != 0)
                return ERROR_INVALID_PARAMETER;
            break;

        case 2: /* EventProviderSetTraits: the size of a block is a 16-bit number */
            if (InformationLength < 3 || InformationLength > 0xffff || !EventInformation)
                return ERROR_INVALID_PARAMETER;
            break;

        case 3: /* EventProviderUseDescriptorType: one byte, 0 or 1 */
            if (InformationLength != 1 || !EventInformation)
                return ERROR_INVALID_PARAMETER;
            break;

        default:
            return ERROR_NOT_SUPPORTED;
    }

    RtlAcquireSRWLockShared(&EtwpRegistrationLock);
    Registration = EtwpFindRegistration(RegHandle);
    if (!Registration)
        Status = ERROR_INVALID_HANDLE;
    else if (InformationClass == 2 && Registration->TraitsSet)
        Status = ERROR_GEN_FAILURE;
    else
        Serial = Registration->Serial;
    RtlReleaseSRWLockShared(&EtwpRegistrationLock);
    if (Status != ERROR_SUCCESS || InformationClass == 0)
        return Status;

    /* An unreadable byte is the caller's exception, as on Windows 10 (measured): the buffer is not guarded here */
    if (InformationClass == 3)
        return *(PUCHAR)EventInformation > 1 ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;

    _SEH2_TRY
    {
        if (!EtwpTraitsAreValid(EventInformation, InformationLength))
            Status = ERROR_FILE_CORRUPT;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = ERROR_NOACCESS;
    }
    _SEH2_END;
    if (Status != ERROR_SUCCESS)
        return Status;

    /* The registration may have gone, or taken traits, while the buffer was read */
    RtlAcquireSRWLockExclusive(&EtwpRegistrationLock);
    Registration = EtwpFindRegistration(RegHandle);
    if (!Registration || Registration->Serial != Serial)
        Status = ERROR_INVALID_HANDLE;
    else if (Registration->TraitsSet)
        Status = ERROR_GEN_FAILURE;
    else
        Registration->TraitsSet = TRUE;
    RtlReleaseSRWLockExclusive(&EtwpRegistrationLock);

    return Status;
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
