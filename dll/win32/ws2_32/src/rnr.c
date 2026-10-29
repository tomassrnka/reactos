/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS WinSock 2 API
 * FILE:        dll/win32/ws2_32/src/rnr.c
 * PURPOSE:     Registration and Resolution Support
 * PROGRAMMER:  Alex Ionescu (alex@relsoft.net)
 */

/* INCLUDES ******************************************************************/

#include <ws2_32.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
INT
WSAAPI
WSAAddressToStringA(IN LPSOCKADDR lpsaAddress,
                    IN DWORD dwAddressLength,
                    IN LPWSAPROTOCOL_INFOA lpProtocolInfo,
                    OUT LPSTR lpszAddressString,
                    IN OUT  LPDWORD lpdwAddressStringLength)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode, Status;
    DWORD CatalogEntryId;
    PTCATALOG Catalog;
    PTCATALOG_ENTRY CatalogEntry;
    LPWSTR UnicodeString;
    DWORD Length = *lpdwAddressStringLength;

    DPRINT("WSAAddressToStringA: %p\n", lpsaAddress);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        /* Leave now */
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* Allocate the unicode string */
    UnicodeString = HeapAlloc(WsSockHeap, 0, Length * 2);
    if (!UnicodeString)
    {
        /* No memory; fail */
        SetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }

    /* Get the catalog */
    Catalog = WsProcGetTCatalog(Process);

    /* Check if we got custom protocol info */
    if (lpProtocolInfo)
    {
        /* Get the entry ID */
        CatalogEntryId = lpProtocolInfo->dwCatalogEntryId;

        /* Get the entry associated with it */
        ErrorCode = WsTcGetEntryFromCatalogEntryId(Catalog,
                                                   CatalogEntryId,
                                                   &CatalogEntry);
    }
    else
    {
        /* Get it from the address family */
        ErrorCode = WsTcGetEntryFromAf(Catalog,
                                       lpsaAddress->sa_family,
                                       &CatalogEntry);
    }

    /* Check for success */
    if (ErrorCode == ERROR_SUCCESS)
    {
        /* Call the provider */
        Status = CatalogEntry->Provider->Service.lpWSPAddressToString(lpsaAddress,
                                                                      dwAddressLength,
                                                                      &CatalogEntry->
                                                                      ProtocolInfo,
                                                                      UnicodeString,
                                                                      lpdwAddressStringLength,
                                                                      &ErrorCode);
        if (Status == ERROR_SUCCESS)
        {
            /* Convert the string */
            WideCharToMultiByte(CP_ACP,
                                0,
                                UnicodeString,
                                -1,
                                lpszAddressString,
                                Length,
                                NULL,
                                NULL);
        }

        /* Dereference the entry */
        WsTcEntryDereference(CatalogEntry);

        /* Free the unicode string */
        HeapFree(WsSockHeap, 0, UnicodeString);

        /* Check for success and return */
        if (Status == ERROR_SUCCESS) return ERROR_SUCCESS;
    }
    else
    {
        /* Free the unicode string */
        HeapFree(WsSockHeap, 0, UnicodeString);
    }

    /* Set the error and return */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
WSAAddressToStringW(IN LPSOCKADDR lpsaAddress,
                    IN DWORD dwAddressLength,
                    IN LPWSAPROTOCOL_INFOW lpProtocolInfo,
                    OUT LPWSTR lpszAddressString,
                    IN OUT LPDWORD lpdwAddressStringLength)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode, Status;
    DWORD CatalogEntryId;
    PTCATALOG Catalog;
    PTCATALOG_ENTRY CatalogEntry;

    DPRINT("WSAAddressToStringW: %p\n", lpsaAddress);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        /* Leave now */
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* Get the catalog */
    Catalog = WsProcGetTCatalog(Process);

    /* Check if we got custom protocol info */
    if (lpProtocolInfo)
    {
        /* Get the entry ID */
        CatalogEntryId = lpProtocolInfo->dwCatalogEntryId;

        /* Get the entry associated with it */
        ErrorCode = WsTcGetEntryFromCatalogEntryId(Catalog,
                                                   CatalogEntryId,
                                                   &CatalogEntry);
    }
    else
    {
        /* Get it from the address family */
        ErrorCode = WsTcGetEntryFromAf(Catalog,
                                       lpsaAddress->sa_family,
                                       &CatalogEntry);
    }

    /* Check for success */
    if (ErrorCode == ERROR_SUCCESS)
    {
        /* Call the provider */
        Status = CatalogEntry->Provider->Service.lpWSPAddressToString(lpsaAddress,
                                                                      dwAddressLength,
                                                                      &CatalogEntry->
                                                                      ProtocolInfo,
                                                                      lpszAddressString,
                                                                      lpdwAddressStringLength,
                                                                      &ErrorCode);

        /* Dereference the entry */
        WsTcEntryDereference(CatalogEntry);

        /* Check for success and return */
        if (Status == ERROR_SUCCESS) return ERROR_SUCCESS;
    }

    /* Set the error and return */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
WSALookupServiceEnd(IN HANDLE hLookup)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode;
    PNSQUERY Query = hLookup;

    DPRINT("WSALookupServiceEnd: %lx\n", hLookup);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        /* Leave now */
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* Check for a valid handle, then validate and reference it */
    if (IsBadReadPtr(Query, sizeof(*Query)) || !WsNqValidateAndReference(Query))
    {
        /* Fail */
        SetLastError(WSA_INVALID_HANDLE);
        return SOCKET_ERROR;
    }

    /* Do the lookup */
    ErrorCode = WsNqLookupServiceEnd(Query);

    /* Remove the validation reference */
    WsNqDereference(Query);

    /* Remove the keep-alive */
    WsNqDereference(Query);

    /* Return */
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
INT
WSAAPI
WSALookupServiceBeginA(IN LPWSAQUERYSETA lpqsRestrictions,
                       IN DWORD dwControlFlags,
                       OUT LPHANDLE lphLookup)
{
    INT ErrorCode;
    LPWSAQUERYSETW UnicodeQuerySet = NULL;
    SIZE_T UnicodeQuerySetSize = 0;

    DPRINT("WSALookupServiceBeginA: %p\n", lpqsRestrictions);

    /* Verify pointer. lpServiceClassId is optional (NLA/NS_NLA queries pass
     * NULL), so only validate it when the caller supplied one. */
    if (IsBadReadPtr(lpqsRestrictions, sizeof(*lpqsRestrictions)) ||
        ((lpqsRestrictions->lpServiceClassId != NULL) &&
         IsBadReadPtr(lpqsRestrictions->lpServiceClassId, sizeof(*lpqsRestrictions->lpServiceClassId))))
    {
        /* Invalid */
        SetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    /* Same dwSize rule as WSALookupServiceBeginW */
    if (lpqsRestrictions->dwSize < sizeof(*lpqsRestrictions))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return SOCKET_ERROR;
    }

    /* Clear the reserved fields */
    lpqsRestrictions->dwOutputFlags = 0;
    lpqsRestrictions->lpszComment = NULL;
    lpqsRestrictions->dwNumberOfCsAddrs = 0;

    /* Find out the side we'll need */
    ErrorCode = MapAnsiQuerySetToUnicode(lpqsRestrictions,
                                         &UnicodeQuerySetSize,
                                         UnicodeQuerySet);

    /* We should've failed */
    if (ErrorCode == WSAEFAULT)
    {
        /* Allocate the buffer we'll need */
        UnicodeQuerySet = HeapAlloc(WsSockHeap, 0, UnicodeQuerySetSize);
        if (UnicodeQuerySet)
        {
            /* Do the conversion for real */
            ErrorCode = MapAnsiQuerySetToUnicode(lpqsRestrictions,
                                                 &UnicodeQuerySetSize,
                                                 UnicodeQuerySet);
            if (ErrorCode == ERROR_SUCCESS)
            {
                /* Now call the Unicode function */
                ErrorCode = WSALookupServiceBeginW(UnicodeQuerySet,
                                                   dwControlFlags,
                                                   lphLookup);
            }

            /* Free our buffer */
            HeapFree(WsSockHeap, 0, UnicodeQuerySet);
        }
        else
        {
            /* No memory to allocate */
            ErrorCode = WSAEFAULT;
        }
    }

    /* Set the error in case of failure; WSALookupServiceBeginW has set its own */
    if (ErrorCode != ERROR_SUCCESS && ErrorCode != SOCKET_ERROR)
        SetLastError(ErrorCode);

    /* Return to caller */
    return ErrorCode == ERROR_SUCCESS ? ErrorCode : SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WINAPI
WSALookupServiceBeginW(IN LPWSAQUERYSETW lpqsRestrictions,
                       IN DWORD dwControlFlags,
                       OUT LPHANDLE lphLookup)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode;
    PNSQUERY Query;

    DPRINT("WSALookupServiceBeginW: %p\n", lpqsRestrictions);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        /* Leave now */
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* Verify pointers. lpServiceClassId is optional (e.g. NLA/NS_NLA queries
     * pass NULL to enumerate the current networks), so only validate it when
     * the caller actually supplied one. */
    if (IsBadWritePtr(lphLookup, sizeof(*lphLookup)) ||
        IsBadReadPtr(lpqsRestrictions, sizeof(*lpqsRestrictions)) ||
        ((lpqsRestrictions->lpServiceClassId != NULL) &&
         IsBadReadPtr(lpqsRestrictions->lpServiceClassId, sizeof(*lpqsRestrictions->lpServiceClassId))))
    {
        /* They are invalid; fail */
        SetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    /* Windows rejects a query set whose dwSize is smaller than WSAQUERYSET
     * (e.g. a zeroed one) with ERROR_INVALID_PARAMETER. Now that a NULL class
     * id is accepted this must be checked here, or such a query reaches the NLA
     * provider and wrongly succeeds. */
    if (lpqsRestrictions->dwSize < sizeof(*lpqsRestrictions))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return SOCKET_ERROR;
    }

    /* Create a new query object */
    if ((Query = WsNqAllocate()))
    {
        /* Initialize it */
        WsNqInitialize(Query);

        /* Do the lookup */
        ErrorCode = WsNqLookupServiceBegin(Query,
                                           lpqsRestrictions,
                                           dwControlFlags,
                                           WsProcGetNsCatalog(Process));

        /* Check for success */
        if (ErrorCode == ERROR_SUCCESS)
        {
            /* Return the handle */
            *lphLookup = Query;
        }
        else
        {
            /* Fail */
            *lphLookup = NULL;
            WsNqDelete(Query);
        }
    }
    else
    {
        /* No memory */
        ErrorCode = SOCKET_ERROR;
        SetLastError(WSAENOBUFS);
    }

    /* Return */
    return ErrorCode;
}

/*
 * @implemented
 */
INT
WINAPI
WSALookupServiceNextW(IN HANDLE hLookup,
                      IN DWORD dwControlFlags,
                      IN OUT LPDWORD lpdwBufferLength,
                      OUT LPWSAQUERYSETW lpqsResults)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode;
    PNSQUERY Query = hLookup;

    DPRINT("WSALookupServiceNextW: %lx\n", hLookup);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        /* Leave now */
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /*
     * Verify pointers. Note that the size of the buffer
     * pointed by lpqsResults is given by *lpdwBufferLength.
     */
    if (IsBadReadPtr(lpdwBufferLength, sizeof(*lpdwBufferLength)) ||
        IsBadWritePtr(lpqsResults, *lpdwBufferLength))
    {
        /* It is invalid; fail */
        SetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    /* Check for a valid handle, then validate and reference it */
    if (IsBadReadPtr(Query, sizeof(*Query)) || !WsNqValidateAndReference(Query))
    {
        /* Fail */
        SetLastError(WSA_INVALID_HANDLE);
        return SOCKET_ERROR;
    }

    /* Do the lookup */
    ErrorCode = WsNqLookupServiceNext(Query,
                                      dwControlFlags,
                                      lpdwBufferLength,
                                      lpqsResults);

    /* Remove the validation reference */
    WsNqDereference(Query);

    /* Return */
    return ErrorCode;
}

/*
 * @implemented
 */
INT
WSAAPI
WSALookupServiceNextA(IN HANDLE hLookup,
                      IN DWORD dwControlFlags,
                      IN OUT LPDWORD lpdwBufferLength,
                      OUT LPWSAQUERYSETA lpqsResults)
{
    LPWSAQUERYSETW UnicodeQuerySet;
    DWORD UnicodeQuerySetSize;
    INT ErrorCode;

    DPRINT("WSALookupServiceNextA: %lx\n", hLookup);

    /*
     * Verify pointers. Note that the size of the buffer
     * pointed by lpqsResults is given by *lpdwBufferLength.
     */
    if (IsBadReadPtr(lpdwBufferLength, sizeof(*lpdwBufferLength)) ||
        IsBadWritePtr(lpqsResults, *lpdwBufferLength))
    {
        /* It is invalid; fail */
        SetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    UnicodeQuerySetSize = *lpdwBufferLength;

    /* Check how much the user is giving */
    if (UnicodeQuerySetSize >= sizeof(WSAQUERYSETW))
    {
        /* Allocate the buffer we'll use */
        UnicodeQuerySet = HeapAlloc(WsSockHeap, 0, UnicodeQuerySetSize);
        if (!UnicodeQuerySet)
        {
            /*
             * We failed, possibly because the specified size was too large?
             * Retrieve the needed buffer size with the WSALookupServiceNextW
             * call and retry again a second time.
             */
            UnicodeQuerySetSize = 0;
        }
    }
    else
    {
        /*
         * The buffer is too small. Retrieve the needed buffer size with
         * the WSALookupServiceNextW call and return it to the caller.
         */
        UnicodeQuerySet = NULL;
        UnicodeQuerySetSize = 0;
    }

    /* Call the Unicode Function */
    ErrorCode = WSALookupServiceNextW(hLookup,
                                      dwControlFlags,
                                      &UnicodeQuerySetSize,
                                      UnicodeQuerySet);

    /*
     * Check whether we actually just retrieved the needed buffer size
     * because our previous local allocation did fail. If so, allocate
     * a new buffer and retry again.
     */
    if ( (!UnicodeQuerySet) && (*lpdwBufferLength >= sizeof(WSAQUERYSETW)) &&
         (ErrorCode == SOCKET_ERROR) && (GetLastError() == WSAEFAULT) )
    {
        /* Allocate the buffer we'll use */
        UnicodeQuerySet = HeapAlloc(WsSockHeap, 0, UnicodeQuerySetSize);
        if (UnicodeQuerySet)
        {
            /* Call the Unicode Function */
            ErrorCode = WSALookupServiceNextW(hLookup,
                                              dwControlFlags,
                                              &UnicodeQuerySetSize,
                                              UnicodeQuerySet);
        }
        /*
         * Otherwise the allocation failed and we
         * fall back into the error checks below.
         */
    }

    if (ErrorCode == ERROR_SUCCESS)
    {
        SIZE_T SetSize = *lpdwBufferLength;

        /* Now convert back to ANSI */
        ErrorCode = MapUnicodeQuerySetToAnsi(UnicodeQuerySet,
                                             &SetSize,
                                             lpqsResults);
        if (ErrorCode != ERROR_SUCCESS)
        {
            SetLastError(ErrorCode);
        }
        else if (SetSize > MAXDWORD)
        {
            ErrorCode = ERROR_ARITHMETIC_OVERFLOW;
            SetLastError(ErrorCode);
        }
        else
        {
            *lpdwBufferLength = SetSize;
        }
    }
    else
    {
        /* Check if we ran out of space */
        if (GetLastError() == WSAEFAULT)
        {
            /* Return how much space we'll need, including padding */
            *lpdwBufferLength = UnicodeQuerySetSize +
                                ((sizeof(ULONG) * 6) - (6 * 1));
        }
    }

    /* If we had a local buffer, free it */
    if (UnicodeQuerySet)
        HeapFree(WsSockHeap, 0, UnicodeQuerySet);

    /* Return to caller */
    return (ErrorCode == ERROR_SUCCESS) ? ErrorCode : SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSPAPI
WSANSPIoctl(HANDLE hLookup,
            DWORD dwControlCode,
            LPVOID lpvInBuffer,
            DWORD cbInBuffer,
            LPVOID lpvOutBuffer,
            DWORD cbOutBuffer,
            LPDWORD lpcbBytesReturned,
            LPWSACOMPLETION lpCompletion)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode;
    PNSQUERY Query = hLookup;
    PNSQUERY_PROVIDER Provider;
    WSATHREADID ThreadId;

    DPRINT("WSANSPIoctl: %lx\n", hLookup);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* The bytes-returned pointer is mandatory */
    if (IsBadWritePtr(lpcbBytesReturned, sizeof(*lpcbBytesReturned)))
    {
        SetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    /* Check for a valid handle, then validate and reference it */
    if (IsBadReadPtr(Query, sizeof(*Query)) || !WsNqValidateAndReference(Query))
    {
        SetLastError(WSA_INVALID_HANDLE);
        return SOCKET_ERROR;
    }

    /* Pick the active provider (or the first one in the query) */
    Provider = Query->ActiveProvider;
    if (!Provider && !IsListEmpty(&Query->ProviderList))
    {
        Provider = CONTAINING_RECORD(Query->ProviderList.Flink,
                                     NSQUERY_PROVIDER,
                                     QueryLink);
    }

    /* Only new-style providers that expose NSPIoctl can service this */
    if (!Provider || !Provider->Provider || !Provider->Provider->Service.NSPIoctl)
    {
        WsNqDereference(Query);
        SetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }

    /* Describe the calling thread for the provider */
    ThreadId.ThreadHandle = GetCurrentThread();
    ThreadId.Reserved = 0;

    /* Dispatch to the provider */
    ErrorCode = Provider->Provider->Service.NSPIoctl(Provider->LookupHandle,
                                                     dwControlCode,
                                                     lpvInBuffer,
                                                     cbInBuffer,
                                                     lpvOutBuffer,
                                                     cbOutBuffer,
                                                     lpcbBytesReturned,
                                                     lpCompletion,
                                                     &ThreadId);

    /* Remove the validation reference */
    WsNqDereference(Query);

    /* The provider has set its own last error (e.g. WSA_IO_PENDING) */
    return ErrorCode;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSARemoveServiceClass(IN LPGUID lpServiceClassId)
{
    DPRINT("WSARemoveServiceClass: %lx\n", lpServiceClassId);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSASetServiceA(IN LPWSAQUERYSETA lpqsRegInfo,
               IN WSAESETSERVICEOP essOperation,
               IN DWORD dwControlFlags)
{
    DPRINT("WSASetServiceA: %lx\n", lpqsRegInfo);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSASetServiceW(IN LPWSAQUERYSETW lpqsRegInfo,
               IN WSAESETSERVICEOP essOperation,
               IN DWORD dwControlFlags)
{
    DPRINT("WSASetServiceW: %lx\n", lpqsRegInfo);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSAGetServiceClassInfoA(IN LPGUID lpProviderId,
                        IN LPGUID lpServiceClassId,
                        IN OUT LPDWORD lpdwBufferLength,
                        OUT LPWSASERVICECLASSINFOA lpServiceClassInfo)
{
    DPRINT("WSAGetServiceClassInfoA: %lx\n", lpProviderId);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSAGetServiceClassInfoW(IN LPGUID lpProviderId,
                        IN LPGUID lpServiceClassId,
                        IN OUT LPDWORD lpdwBufferLength,
                        OUT LPWSASERVICECLASSINFOW lpServiceClassInfo)
{
    DPRINT("WSAGetServiceClassInfoW: %lx\n", lpProviderId);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSAGetServiceClassNameByClassIdA(IN LPGUID lpServiceClassId,
                                 OUT LPSTR lpszServiceClassName,
                                 IN OUT LPDWORD lpdwBufferLength)
{
    DPRINT("WSAGetServiceClassNameByClassIdA: %lx\n", lpServiceClassId);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSAGetServiceClassNameByClassIdW(IN LPGUID lpServiceClassId,
                                 OUT LPWSTR lpszServiceClassName,
                                 IN OUT LPDWORD lpdwBufferLength)
{
    DPRINT("WSAGetServiceClassNameByClassIdW: %lx\n", lpServiceClassId);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * @unimplemented
 */
INT
WSAAPI
WSAInstallServiceClassA(IN LPWSASERVICECLASSINFOA lpServiceClassInfo)
{
    DPRINT("WSAInstallServiceClassA: %lx\n", lpServiceClassInfo);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
* @unimplemented
*/
INT
WSAAPI
WSAInstallServiceClassW(IN LPWSASERVICECLASSINFOW lpServiceClassInfo)
{
    DPRINT("WSAInstallServiceClassW: %lx\n", lpServiceClassInfo);
    SetLastError(WSAEINVAL);
    return SOCKET_ERROR;
}

/*
 * WSAEnumNameSpaceProviders{A,W} return an ARRAY of WSANAMESPACE_INFO structures
 * (callers index it as info[i]) followed by the identifier strings the
 * lpszIdentifier members point to. The structures must therefore be contiguous:
 * packing each string right behind its own structure (as this code once did)
 * only works while there is a single provider, and corrupts info[1..] as soon as
 * the catalog has two (e.g. after the NLA provider was registered).
 * Windows also reports the required size (WSAEFAULT) before writing anything.
 */
static
DWORD
NSProviderNameSize(IN PNSCATALOG_ENTRY Entry,
                   IN BOOLEAN Unicode)
{
    if (!Entry->ProviderName)
        return 0;

    if (Unicode)
        return (wcslen(Entry->ProviderName) + 1) * sizeof(WCHAR);

    /* Size of the ANSI string including the terminator */
    return WideCharToMultiByte(CP_ACP, 0, Entry->ProviderName, -1, NULL, 0, NULL, NULL);
}

BOOL
WSAAPI
NSProvidersCountProc(PVOID EnumContext,
                     PNSCATALOG_ENTRY Entry)
{
    PNSPROVIDER_ENUM_CONTEXT Context = (PNSPROVIDER_ENUM_CONTEXT)EnumContext;

    /* First pass: only count the entries and the string bytes they need */
    Context->Count++;
    Context->StringBytes += NSProviderNameSize(Entry, Context->Unicode);

    return TRUE;
}

BOOL
WSAAPI
NSProvidersEnumerationProc(PVOID EnumContext,
                           PNSCATALOG_ENTRY Entry)
{
    PNSPROVIDER_ENUM_CONTEXT Context = (PNSPROVIDER_ENUM_CONTEXT)EnumContext;
    DWORD InfoSize = Context->Unicode ? sizeof(WSANAMESPACE_INFOW) : sizeof(WSANAMESPACE_INFOA);
    DWORD NameSize = NSProviderNameSize(Entry, Context->Unicode);
    WSANAMESPACE_INFOW InfoW;
    PBYTE Buffer = Context->ProtocolBuffer;
    PBYTE Name = Buffer + Context->Count * InfoSize + Context->BufferUsed;

    /* The catalog changed between the two passes; do not overrun the buffer */
    if (Context->Index >= Context->Count ||
        Context->BufferUsed + NameSize > Context->StringBytes)
    {
        Context->ErrorCode = WSAEFAULT;
        return FALSE;
    }

    /* Fill NS Provider data */
    RtlZeroMemory(&InfoW, sizeof(InfoW));
    InfoW.dwNameSpace = Entry->NamespaceId;
    InfoW.dwVersion = Entry->Version;
    InfoW.fActive = Entry->Enabled;
    RtlCopyMemory(&InfoW.NSProviderId, &Entry->ProviderId, sizeof(InfoW.NSProviderId));
    InfoW.lpszIdentifier = NameSize ? (LPWSTR)Name : NULL;

    /* WSANAMESPACE_INFOA and W have the same layout and size */
    RtlCopyMemory(Buffer + Context->Index * InfoSize, &InfoW, InfoSize);

    if (NameSize)
    {
        if (Context->Unicode)
        {
            RtlCopyMemory(Name, Entry->ProviderName, NameSize);
        }
        else if (!WideCharToMultiByte(CP_ACP, 0, Entry->ProviderName, -1,
                                      (LPSTR)Name, NameSize, NULL, NULL))
        {
            /* Never leave an unterminated identifier behind */
            Name[0] = '\0';
        }
    }

    Context->Index++;
    Context->BufferUsed += NameSize;

    /* Continue enumeration */
    return TRUE;
}

INT
WSAAPI
WSAEnumNameSpaceProvidersInternal(IN OUT LPDWORD lpdwBufferLength,
    OUT LPWSANAMESPACE_INFOA lpnspBuffer, BOOLEAN Unicode)
{
    PWSPROCESS WsProcess;
    PNSCATALOG Catalog;
    NSPROVIDER_ENUM_CONTEXT Context;
    DWORD Needed;

    DPRINT("WSAEnumNameSpaceProvidersInternal: %lx\n", lpnspBuffer);

    if (!lpdwBufferLength)
    {
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    WsProcess = WsGetProcess();
    /* Create a catalog object from the current one */
    Catalog = WsProcGetNsCatalog(WsProcess);
    if (!Catalog)
    {
        /* Fail if we couldn't */
        WSASetLastError(WSA_NOT_ENOUGH_MEMORY);
        return SOCKET_ERROR;
    }

    RtlZeroMemory(&Context, sizeof(Context));
    Context.Unicode = Unicode;
    Context.ErrorCode = ERROR_SUCCESS;

    /* First pass: how many entries and how many string bytes do we need? */
    WsNcEnumerateCatalogItems(Catalog, NSProvidersCountProc, &Context);
    Needed = Context.Count * (Unicode ? sizeof(WSANAMESPACE_INFOW) : sizeof(WSANAMESPACE_INFOA)) +
             Context.StringBytes;

    /* Check if enough space was available */
    if (Needed && (!lpnspBuffer || *lpdwBufferLength < Needed))
    {
        /* Fail and tell them how much we need */
        *lpdwBufferLength = Needed;
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }

    /* Second pass: fill the array, then the strings behind it */
    Context.ProtocolBuffer = lpnspBuffer;
    Context.BufferLength = *lpdwBufferLength;
    Context.BufferUsed = 0;
    WsNcEnumerateCatalogItems(Catalog, NSProvidersEnumerationProc, &Context);

    if (Context.ErrorCode != ERROR_SUCCESS)
    {
        /* Failure, normalize error */
        WSASetLastError(Context.ErrorCode);
        return SOCKET_ERROR;
    }

    return Context.Index;
}

/*
 * @implemented
 */
INT
WSAAPI
WSAEnumNameSpaceProvidersA(IN OUT LPDWORD lpdwBufferLength,
                           OUT LPWSANAMESPACE_INFOA lpnspBuffer)
{
    DPRINT("WSAEnumNameSpaceProvidersA: %lx\n", lpnspBuffer);
    return WSAEnumNameSpaceProvidersInternal(lpdwBufferLength, (LPWSANAMESPACE_INFOA)lpnspBuffer, FALSE);
}

/*
 * @implemented
 */
INT
WSAAPI
WSAEnumNameSpaceProvidersW(IN OUT LPDWORD lpdwBufferLength,
                           OUT LPWSANAMESPACE_INFOW lpnspBuffer)
{
    DPRINT("WSAEnumNameSpaceProvidersW: %lx\n", lpnspBuffer);
    return WSAEnumNameSpaceProvidersInternal(lpdwBufferLength, (LPWSANAMESPACE_INFOA)lpnspBuffer, TRUE);
}

/*
 * @implemented
 */
INT
WSAAPI
WSAStringToAddressA(IN LPSTR AddressString,
                    IN INT AddressFamily,
                    IN LPWSAPROTOCOL_INFOA lpProtocolInfo,
                    OUT LPSOCKADDR lpAddress,
                    IN OUT  LPINT lpAddressLength)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode, Status;
    DWORD CatalogEntryId;
    PTCATALOG Catalog;
    PTCATALOG_ENTRY CatalogEntry;
    LPWSTR UnicodeString;
    DWORD Length = (DWORD)strlen(AddressString) + 1;

    DPRINT("WSAStringToAddressA: %s\n", AddressString);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        /* Leave now */
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* Allocate the unicode string */
    UnicodeString = HeapAlloc(WsSockHeap, 0, Length * 2);
    if (!UnicodeString)
    {
        /* No memory; fail */
        SetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }

    /* Convert the string */
    MultiByteToWideChar(CP_ACP, 0, AddressString, -1, UnicodeString, Length);

    /* Get the catalog */
    Catalog = WsProcGetTCatalog(Process);

    /* Check if we got custom protocol info */
    if (lpProtocolInfo)
    {
        /* Get the entry ID */
        CatalogEntryId = lpProtocolInfo->dwCatalogEntryId;

        /* Get the entry associated with it */
        ErrorCode = WsTcGetEntryFromCatalogEntryId(Catalog,
                                                   CatalogEntryId,
                                                   &CatalogEntry);
    }
    else
    {
        /* Get it from the address family */
        ErrorCode = WsTcGetEntryFromAf(Catalog, AddressFamily, &CatalogEntry);
    }

    /* Check for success */
    if (ErrorCode == ERROR_SUCCESS)
    {
        /* Call the provider */
        Status = CatalogEntry->Provider->Service.lpWSPStringToAddress(UnicodeString,
                                                              AddressFamily,
                                                              &CatalogEntry->
                                                              ProtocolInfo,
                                                              lpAddress,
                                                              lpAddressLength,
                                                              &ErrorCode);

        /* Dereference the entry */
        WsTcEntryDereference(CatalogEntry);

        /* Free the unicode string */
        HeapFree(WsSockHeap, 0, UnicodeString);

        /* Check for success and return */
        if (Status == ERROR_SUCCESS) return ERROR_SUCCESS;
    }
    else
    {
        /* Free the unicode string */
        HeapFree(WsSockHeap, 0, UnicodeString);
    }

    /* Set the error and return */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
WSAStringToAddressW(IN LPWSTR AddressString,
                    IN INT AddressFamily,
                    IN LPWSAPROTOCOL_INFOW lpProtocolInfo,
                    OUT LPSOCKADDR lpAddress,
                    IN OUT LPINT lpAddressLength)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    INT ErrorCode, Status;
    DWORD CatalogEntryId;
    PTCATALOG Catalog;
    PTCATALOG_ENTRY CatalogEntry;

    DPRINT("WSAStringToAddressW: %S\n", AddressString);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) != ERROR_SUCCESS)
    {
        /* Leave now */
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    /* Get the catalog */
    Catalog = WsProcGetTCatalog(Process);

    /* Check if we got custom protocol info */
    if (lpProtocolInfo)
    {
        /* Get the entry ID */
        CatalogEntryId = lpProtocolInfo->dwCatalogEntryId;

        /* Get the entry associated with it */
        ErrorCode = WsTcGetEntryFromCatalogEntryId(Catalog,
                                                   CatalogEntryId,
                                                   &CatalogEntry);
    }
    else
    {
        /* Get it from the address family */
        ErrorCode = WsTcGetEntryFromAf(Catalog, AddressFamily, &CatalogEntry);
    }

    /* Check for success */
    if (ErrorCode == ERROR_SUCCESS)
    {
        /* Call the provider */
        Status = CatalogEntry->Provider->Service.lpWSPStringToAddress(AddressString,
                                                                      AddressFamily,
                                                                      &CatalogEntry->
                                                                      ProtocolInfo,
                                                                      lpAddress,
                                                                      lpAddressLength,
                                                                      &ErrorCode);

        /* Dereference the entry */
        WsTcEntryDereference(CatalogEntry);

        /* Check for success and return */
        if (Status == ERROR_SUCCESS) return ERROR_SUCCESS;
    }

    /* Set the error and return */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}
