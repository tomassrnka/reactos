/*
 * PROJECT:     ReactOS Service Control Manager
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/system/services/lock.c
 * PURPOSE:     User-side Services Start Serialization Lock functions
 * COPYRIGHT:   Copyright 2012 Hermès Bélusca
 */

/* INCLUDES *****************************************************************/

#include "services.h"

#include <time.h>
#include <winnls.h>
#include <strsafe.h>
#include <sddl.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* The unique user service start lock of the SCM */
static PSTART_LOCK pServiceStartLock = NULL;


/* The owner of a lock the SCM itself holds */
static const WCHAR szSystemOwner[] = L"NT AUTHORITY\\SYSTEM";


/* FUNCTIONS *****************************************************************/

/*
 * The name of the user of the RPC call, as the lock status reports it: DOMAIN\user, or .\user for an
 * account of this computer, as Windows does. Without a name for the account, its SID string.
 * The result is to be freed with HeapFree.
 */
static LPWSTR
ScmGetCallerName(VOID)
{
    HANDLE hToken = NULL;
    PTOKEN_USER pUser = NULL;
    LPWSTR pszName = NULL, pszDomain = NULL, pszResult = NULL;
    WCHAR szComputer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD dwLength = 0, cchName = 0, cchDomain = 0, cchComputer = ARRAYSIZE(szComputer);
    SID_NAME_USE Use;
    BOOL bImpersonated = (RpcImpersonateClient(NULL) == RPC_S_OK);

    if (!bImpersonated)
        return NULL;

    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hToken))
        hToken = NULL;
    RpcRevertToSelf();
    if (hToken == NULL)
        return NULL;

    GetTokenInformation(hToken, TokenUser, NULL, 0, &dwLength);
    if (dwLength == 0)
        goto done;
    pUser = HeapAlloc(GetProcessHeap(), 0, dwLength);
    if (pUser == NULL ||
        !GetTokenInformation(hToken, TokenUser, pUser, dwLength, &dwLength))
        goto done;

    LookupAccountSidW(NULL, pUser->User.Sid, NULL, &cchName, NULL, &cchDomain, &Use);
    if (cchName != 0)
    {
        pszName = HeapAlloc(GetProcessHeap(), 0, cchName * sizeof(WCHAR));
        pszDomain = HeapAlloc(GetProcessHeap(), 0, (cchDomain + 1) * sizeof(WCHAR));
        if (pszName != NULL && pszDomain != NULL &&
            LookupAccountSidW(NULL, pUser->User.Sid, pszName, &cchName, pszDomain, &cchDomain, &Use))
        {
            if (!GetComputerNameW(szComputer, &cchComputer))
                szComputer[0] = UNICODE_NULL;

            if (cchDomain != 0 && _wcsicmp(pszDomain, szComputer) == 0)
                wcscpy(pszDomain, L".");

            dwLength = (cchDomain + 1 + cchName + 1) * sizeof(WCHAR);
            pszResult = HeapAlloc(GetProcessHeap(), 0, dwLength);
            if (pszResult != NULL)
            {
                if (cchDomain != 0)
                    StringCbPrintfW(pszResult, dwLength, L"%s\\%s", pszDomain, pszName);
                else
                    StringCbCopyW(pszResult, dwLength, pszName);
            }
        }
    }
    else
    {
        /* No name: the SID string */
        LPWSTR pszSid = NULL;

        if (ConvertSidToStringSidW(pUser->User.Sid, &pszSid))
        {
            pszResult = HeapAlloc(GetProcessHeap(), 0, (wcslen(pszSid) + 1) * sizeof(WCHAR));
            if (pszResult != NULL)
                wcscpy(pszResult, pszSid);
            LocalFree(pszSid);
        }
    }

done:
    if (pszName != NULL)
        HeapFree(GetProcessHeap(), 0, pszName);
    if (pszDomain != NULL)
        HeapFree(GetProcessHeap(), 0, pszDomain);
    if (pUser != NULL)
        HeapFree(GetProcessHeap(), 0, pUser);
    CloseHandle(hToken);
    return pszResult;
}


/*
 * NOTE: IsServiceController is TRUE if locked by the
 * Service Control Manager, and FALSE otherwise.
 */
DWORD
ScmAcquireServiceStartLock(IN BOOL IsServiceController,
                           OUT LPSC_RPC_LOCK lpLock)
{
    DWORD dwRequiredSize;
    DWORD dwError = ERROR_SUCCESS;
    LPWSTR pszOwner = NULL;

    *lpLock = NULL;

    /* The owner is looked up before the database is locked, it takes calls to the LSA */
    if (!IsServiceController)
        pszOwner = ScmGetCallerName();

    /* Lock the service database exclusively */
    ScmLockDatabaseExclusive();

    if (pServiceStartLock != NULL)
    {
        dwError = ERROR_SERVICE_DATABASE_LOCKED;
        goto done;
    }

    /* Allocate a new lock for the database */
    dwRequiredSize = sizeof(START_LOCK);

    if (!IsServiceController)
    {
        /* FIXME: dwRequiredSize += RtlLengthSid(UserSid <-- to be retrieved); */
    }

    pServiceStartLock = HeapAlloc(GetProcessHeap(),
                                  HEAP_ZERO_MEMORY,
                                  dwRequiredSize);
    if (pServiceStartLock == NULL)
    {
        dwError = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }

    pServiceStartLock->Tag = LOCK_TAG;
    pServiceStartLock->TimeWhenLocked = (DWORD)time(NULL);

    /* The owner SID is not kept, only the name; a lock of the SCM itself has none */
    pServiceStartLock->LockOwnerSid = (PSID)NULL;
    pServiceStartLock->LockOwnerName = pszOwner;
    pszOwner = NULL;

    *lpLock = (LPSC_RPC_LOCK)pServiceStartLock;

done:
    /* Unlock the service database */
    ScmUnlockDatabase();

    if (pszOwner != NULL)
        HeapFree(GetProcessHeap(), 0, pszOwner);

    return dwError;
}


DWORD
ScmReleaseServiceStartLock(IN OUT LPSC_RPC_LOCK lpLock)
{
    PSTART_LOCK pStartLock;
    DWORD dwError = ERROR_SUCCESS;

    if (lpLock == NULL)
        return ERROR_INVALID_SERVICE_LOCK;

    pStartLock = (PSTART_LOCK)*lpLock;

    if (pStartLock->Tag != LOCK_TAG)
        return ERROR_INVALID_SERVICE_LOCK;

    /* Lock the service database exclusively */
    ScmLockDatabaseExclusive();

    /* Release the lock handle */
    if ((pStartLock == pServiceStartLock) &&
        (pServiceStartLock != NULL))
    {
        if (pServiceStartLock->LockOwnerName != NULL)
            HeapFree(GetProcessHeap(), 0, pServiceStartLock->LockOwnerName);
        HeapFree(GetProcessHeap(), 0, pServiceStartLock);
        pServiceStartLock = NULL;
        *lpLock = NULL;

        dwError = ERROR_SUCCESS;
    }
    else
    {
        dwError = ERROR_INVALID_SERVICE_LOCK;
    }

    /* Unlock the service database */
    ScmUnlockDatabase();

    return dwError;
}


/*
 * Helper functions for RQueryServiceLockStatusW() and RQueryServiceLockStatusA().
 * The owner's name follows the structure, and the size needed is that of both. As the structure is
 * filled only when the buffer is big enough, the size can be asked for with a buffer of any size.
 */
DWORD
ScmQueryServiceLockStatusW(OUT LPQUERY_SERVICE_LOCK_STATUSW lpLockStatus,
                           IN DWORD cbBufSize,
                           OUT LPDWORD pcbBytesNeeded)
{
    LPCWSTR pszOwner = L"";
    DWORD dwError = ERROR_SUCCESS;

    /* Lock the service database shared */
    ScmLockDatabaseShared();

    if (pServiceStartLock != NULL)
    {
        pszOwner = (pServiceStartLock->LockOwnerName != NULL) ? pServiceStartLock->LockOwnerName
                                                              : szSystemOwner;
    }

    *pcbBytesNeeded = sizeof(QUERY_SERVICE_LOCK_STATUSW) + (DWORD)((wcslen(pszOwner) + 1) * sizeof(WCHAR));
    if (cbBufSize < *pcbBytesNeeded)
    {
        dwError = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }

    wcscpy((LPWSTR)(lpLockStatus + 1), pszOwner);
    lpLockStatus->lpLockOwner = (LPWSTR)(ULONG_PTR)sizeof(QUERY_SERVICE_LOCK_STATUSW);

    if (pServiceStartLock != NULL)
    {
        lpLockStatus->fIsLocked = TRUE;
        lpLockStatus->dwLockDuration = (DWORD)time(NULL) - pServiceStartLock->TimeWhenLocked;
    }
    else
    {
        lpLockStatus->fIsLocked = FALSE;
        lpLockStatus->dwLockDuration = 0;
    }

done:
    /* Unlock the service database */
    ScmUnlockDatabase();

    return dwError;
}


DWORD
ScmQueryServiceLockStatusA(OUT LPQUERY_SERVICE_LOCK_STATUSA lpLockStatus,
                           IN DWORD cbBufSize,
                           OUT LPDWORD pcbBytesNeeded)
{
    LPCWSTR pszOwner = L"";
    DWORD cbOwner;
    DWORD dwError = ERROR_SUCCESS;

    /* Lock the service database shared */
    ScmLockDatabaseShared();

    if (pServiceStartLock != NULL)
    {
        pszOwner = (pServiceStartLock->LockOwnerName != NULL) ? pServiceStartLock->LockOwnerName
                                                              : szSystemOwner;
    }

    cbOwner = WideCharToMultiByte(CP_ACP, 0, pszOwner, -1, NULL, 0, NULL, NULL);
    *pcbBytesNeeded = sizeof(QUERY_SERVICE_LOCK_STATUSA) + cbOwner;
    if (cbBufSize < *pcbBytesNeeded)
    {
        dwError = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }

    WideCharToMultiByte(CP_ACP, 0, pszOwner, -1, (LPSTR)(lpLockStatus + 1), cbOwner, NULL, NULL);
    lpLockStatus->lpLockOwner = (LPSTR)(ULONG_PTR)sizeof(QUERY_SERVICE_LOCK_STATUSA);

    if (pServiceStartLock != NULL)
    {
        lpLockStatus->fIsLocked = TRUE;
        lpLockStatus->dwLockDuration = (DWORD)time(NULL) - pServiceStartLock->TimeWhenLocked;
    }
    else
    {
        lpLockStatus->fIsLocked = FALSE;
        lpLockStatus->dwLockDuration = 0;
    }

done:
    /* Unlock the service database */
    ScmUnlockDatabase();

    return dwError;
}

/* EOF */
