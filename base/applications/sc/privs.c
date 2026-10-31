/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/applications/sc/privs.c
 * PURPOSE:     Query/Set the privileges a service requires
 */

#include "sc.h"

/* Prints "[SC] <Api> FAILED <Error>:", then the system's text for the error, each followed by an empty line */
VOID
ReportApiFailure(LPCTSTR pszApi,
                 DWORD dwError)
{
    LPTSTR pszMessage = NULL;
    DWORD dwLength;

    _tprintf(_T("[SC] %s FAILED %lu:\n\n"), pszApi, dwError);

    dwLength = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                             FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL,
                             dwError,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPTSTR)&pszMessage,
                             0,
                             NULL);
    if (dwLength != 0 && pszMessage != NULL)
    {
        /* The text comes with its own line end */
        while (dwLength > 0 && (pszMessage[dwLength - 1] == _T('\r') || pszMessage[dwLength - 1] == _T('\n')))
            pszMessage[--dwLength] = _T('\0');

        _tprintf(_T("%s\n\n"), pszMessage);
        LocalFree(pszMessage);
    }
}


/* Opens the service manager and a service; on failure reports and returns the error */
DWORD
OpenServiceForConfig(LPCTSTR ServiceName,
                     DWORD dwAccess,
                     SC_HANDLE *phManager,
                     SC_HANDLE *phService)
{
    DWORD dwError;

    *phService = NULL;
    *phManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (*phManager == NULL)
    {
        dwError = GetLastError();
        ReportApiFailure(_T("OpenSCManager"), dwError);
        return dwError;
    }

    *phService = OpenService(*phManager, ServiceName, dwAccess);
    if (*phService == NULL)
    {
        dwError = GetLastError();
        ReportApiFailure(_T("OpenService"), dwError);
        CloseServiceHandle(*phManager);
        *phManager = NULL;
        return dwError;
    }

    return ERROR_SUCCESS;
}


/*
 * Queries a configuration level into a buffer of the size on the command line, or of the size it needs
 * when there is none (or it is not a number). The result is to be freed with HeapFree.
 */
DWORD
QueryConfigLevel(SC_HANDLE hService,
                 DWORD dwInfoLevel,
                 LPCTSTR BufferSize,
                 LPBYTE *ppBuffer)
{
    DWORD dwSize = (BufferSize != NULL) ? (DWORD)_ttoi(BufferSize) : 0;
    DWORD dwNeeded = 0, dwError;
    LPBYTE pBuffer;

    *ppBuffer = NULL;

    if (dwSize == 0)
    {
        if (QueryServiceConfig2(hService, dwInfoLevel, NULL, 0, &dwNeeded) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            dwError = GetLastError();
            ReportApiFailure(_T("QueryServiceConfig2"), dwError);
            return dwError;
        }

        dwSize = dwNeeded;
    }

    pBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize + sizeof(DWORD));
    if (pBuffer == NULL)
    {
        ReportApiFailure(_T("HeapAlloc"), ERROR_OUTOFMEMORY);
        return ERROR_OUTOFMEMORY;
    }

    if (!QueryServiceConfig2(hService, dwInfoLevel, pBuffer, dwSize, &dwNeeded))
    {
        dwError = GetLastError();
        ReportApiFailure(_T("QueryServiceConfig2"), dwError);
        if (dwError == ERROR_INSUFFICIENT_BUFFER)
            _tprintf(_T("[SC] GetConfigInfo needs %lu bytes\n"), dwNeeded);
        HeapFree(GetProcessHeap(), 0, pBuffer);
        return dwError;
    }

    *ppBuffer = pBuffer;
    return ERROR_SUCCESS;
}


/* sc privs <service> <priv1/priv2/...> */
DWORD
SetPrivileges(LPCTSTR ServiceName,
              LPCTSTR Privileges)
{
    SC_HANDLE hManager, hService;
    SERVICE_REQUIRED_PRIVILEGES_INFO Info;
    LPTSTR pszList;
    SIZE_T cchLength, cchOut = 0;
    DWORD dwError;

    /* The names, each ended by a null, and an empty string ends the list; the list ends with the first empty name */
    cchLength = _tcslen(Privileges);
    pszList = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (cchLength + 2) * sizeof(TCHAR));
    if (pszList == NULL)
    {
        ReportApiFailure(_T("HeapAlloc"), ERROR_OUTOFMEMORY);
        return ERROR_OUTOFMEMORY;
    }

    while (*Privileges != _T('\0') && *Privileges != _T('/'))
    {
        while (*Privileges != _T('\0') && *Privileges != _T('/'))
            pszList[cchOut++] = *Privileges++;
        pszList[cchOut++] = _T('\0');

        if (*Privileges == _T('/'))
            Privileges++;
    }

    dwError = OpenServiceForConfig(ServiceName, SERVICE_CHANGE_CONFIG, &hManager, &hService);
    if (dwError != ERROR_SUCCESS)
        goto done;

    Info.pmszRequiredPrivileges = pszList;
    if (!ChangeServiceConfig2(hService, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &Info))
    {
        dwError = GetLastError();
        ReportApiFailure(_T("ChangeServiceConfig2"), dwError);
    }
    else
    {
        _tprintf(_T("[SC] ChangeServiceConfig2 SUCCESS\n"));
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hManager);

done:
    HeapFree(GetProcessHeap(), 0, pszList);
    return dwError;
}


/* sc qprivs <service> <buffer size> */
DWORD
QueryPrivileges(LPCTSTR ServiceName,
                LPCTSTR BufferSize)
{
    SC_HANDLE hManager, hService;
    LPBYTE pBuffer;
    LPCTSTR pszName;
    DWORD dwError;
    BOOL bFirst = TRUE;

    dwError = OpenServiceForConfig(ServiceName, SERVICE_QUERY_CONFIG, &hManager, &hService);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    dwError = QueryConfigLevel(hService, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, BufferSize, &pBuffer);
    if (dwError == ERROR_SUCCESS)
    {
        _tprintf(_T("[SC] QueryServiceConfig2 SUCCESS\n\n"));
        _tprintf(_T("SERVICE_NAME: %s\n"), ServiceName);

        pszName = ((LPSERVICE_REQUIRED_PRIVILEGES_INFO)pBuffer)->pmszRequiredPrivileges;
        while (pszName != NULL && *pszName != _T('\0'))
        {
            _tprintf(bFirst ? _T("        PRIVILEGES       : %s\n")
                            : _T("                         : %s\n"), pszName);
            bFirst = FALSE;
            pszName += _tcslen(pszName) + 1;
        }
        if (bFirst)
            _tprintf(_T("        PRIVILEGES       : \n"));

        HeapFree(GetProcessHeap(), 0, pBuffer);
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hManager);
    return dwError;
}
