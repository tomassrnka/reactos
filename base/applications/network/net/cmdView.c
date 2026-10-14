/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS net command
 * FILE:            base/applications/network/net/cmdView.c
 * PURPOSE:         NET VIEW: lists the servers of a domain or the shared resources of a server
 */

#include "net.h"

#define MSG_INVALID_OPTION  3501
#define MSG_OPTION_UNKNOWN  3506

#define CACHE_MASK          0x2030
#define CACHE_BRANCHCACHE   0x2000

#ifndef STYPE_TEMPORARY
#define STYPE_TEMPORARY     0x40000000
#endif

#ifndef _SHARE_INFO_1005_DEFINED
#define _SHARE_INFO_1005_DEFINED
typedef struct _SHARE_INFO_1005
{
    DWORD shi1005_flags;
} SHARE_INFO_1005, *PSHARE_INFO_1005, *LPSHARE_INFO_1005;
#endif

typedef struct _VIEW_SHARE
{
    PWSTR pszName;
    PWSTR pszComment;
    DWORD dwType;
} VIEW_SHARE;

typedef struct _VIEW_CONNECTION
{
    PWSTR pszLocal;
    PWSTR pszRemote;
} VIEW_CONNECTION;


static
BOOL
IsOption(
    PCWSTR pszArg,
    PCWSTR pszName,
    DWORD dwMinLength,
    PCWSTR *ppszValue)
{
    PCWSTR pColon;
    size_t len;

    if (pszArg[0] != L'/')
        return FALSE;

    pColon = wcschr(pszArg, L':');
    len = pColon ? (size_t)(pColon - pszArg - 1) : wcslen(pszArg + 1);
    if (len < dwMinLength || len > wcslen(pszName))
        return FALSE;

    if (_wcsnicmp(pszArg + 1, pszName, len) != 0)
        return FALSE;

    *ppszValue = pColon ? pColon + 1 : NULL;
    return TRUE;
}


static
INT
ViewError(
    NET_API_STATUS Status)
{
    PrintErrorMessage(Status);
    return 2;
}


static
INT
EnumerateServers(
    PCWSTR pszDomain,
    BOOL bDomains)
{
    LPBYTE pBuffer = NULL;
    DWORD dwRead = 0, dwTotal = 0, ResumeHandle = 0, i, nPrinted = 0;
    NET_API_STATUS Status;

    do
    {
        Status = NetServerEnum(NULL,
                               bDomains ? 100 : 101,
                               &pBuffer,
                               MAX_PREFERRED_LENGTH,
                               &dwRead,
                               &dwTotal,
                               bDomains ? SV_TYPE_DOMAIN_ENUM : (SV_TYPE_WORKSTATION | SV_TYPE_SERVER),
                               pszDomain,
                               &ResumeHandle);
        if ((Status != NERR_Success) && (Status != ERROR_MORE_DATA))
            return ViewError(Status);

        if (dwRead > 0 && nPrinted == 0)
        {
            PrintMessageString(bDomains ? 4704 : 4700);
            ConPuts(StdOut, L"\n");
            PrintPadding(L'-', 79);
            ConPuts(StdOut, L"\n");
        }

        for (i = 0; i < dwRead; i++)
        {
            if (bDomains)
            {
                ConPrintf(StdOut, L"%-21s\n", ((PSERVER_INFO_100)pBuffer)[i].sv100_name);
            }
            else
            {
                PSERVER_INFO_101 pInfo = &((PSERVER_INFO_101)pBuffer)[i];

                ConPrintf(StdOut, L"\\\\%-21s%-56s\n", pInfo->sv101_name, pInfo->sv101_comment ? pInfo->sv101_comment : L"");
            }
        }

        nPrinted += dwRead;
        NetApiBufferFree(pBuffer);
        pBuffer = NULL;
    }
    while (Status == ERROR_MORE_DATA);

    if (nPrinted == 0)
    {
        PrintMessageString(3683);
        ConPuts(StdOut, L"\n");
        return 0;
    }

    PrintMessageString(3500);
    ConPuts(StdOut, L"\n");

    return 0;
}


static
DWORD
ShareTypeMessage(
    DWORD dwType)
{
    switch (dwType & ~(STYPE_SPECIAL | STYPE_TEMPORARY))
    {
        case STYPE_DISKTREE:
            return 4710;

        case STYPE_PRINTQ:
            return 4711;

        case STYPE_DEVICE:
            return 4712;

        case STYPE_IPC:
            return 4713;
    }

    return 0;
}


static
DWORD
CachingMessage(
    PWSTR pszServer,
    PWSTR pszName)
{
    PSHARE_INFO_1005 pInfo = NULL;
    DWORD dwFlags = 0;

    if (NetShareGetInfo(pszServer, pszName, 1005, (LPBYTE*)&pInfo) == NERR_Success)
    {
        dwFlags = pInfo->shi1005_flags;
        NetApiBufferFree(pInfo);
    }

    switch (dwFlags & CACHE_MASK)
    {
        case 0x0010:
            return MSG_SHARE_CACHE_DOCUMENTS;

        case 0x0020:
            return MSG_SHARE_CACHE_PROGRAMS;

        case 0x0030:
            return MSG_SHARE_CACHE_NONE;

        default:
            return (dwFlags & CACHE_BRANCHCACHE) ? MSG_SHARE_CACHE_BRANCHCACHE : MSG_SHARE_CACHE_MANUAL;
    }
}


/* The length of a message of netmsg.dll without its line end */
static
size_t
MessageLength(
    DWORD dwMessage)
{
    PWSTR pText = NULL;
    size_t len = 0;

    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                       hModuleNetMsg, dwMessage, LANG_USER_DEFAULT, (LPWSTR)&pText, 0, NULL) && pText)
    {
        len = wcslen(pText);
        while (len > 0 && (pText[len - 1] == L'\r' || pText[len - 1] == L'\n'))
            len--;
        LocalFree(pText);
    }

    return len;
}


/* Prints the text of a message left-aligned in a column of the given width */
static
VOID
PrintColumn(
    DWORD dwMessage,
    size_t nWidth)
{
    size_t nLength = MessageLength(dwMessage);

    ConMsgPuts(StdOut, FORMAT_MESSAGE_FROM_HMODULE, hModuleNetMsg, dwMessage, LANG_USER_DEFAULT);
    if (nLength < nWidth)
        PrintSpaces(nWidth - nLength);
}


static
VOID
FreeConnections(
    VIEW_CONNECTION *pConnections,
    DWORD cConnections)
{
    DWORD i;

    for (i = 0; i < cConnections; i++)
    {
        free(pConnections[i].pszLocal);
        free(pConnections[i].pszRemote);
    }

    if (pConnections)
        HeapFree(GetProcessHeap(), 0, pConnections);
}


static
DWORD
GetConnectionsOfType(
    DWORD dwType,
    VIEW_CONNECTION **ppConnections,
    DWORD cConnections)
{
    VIEW_CONNECTION *pConnections = *ppConnections, *pNew;
    LPNETRESOURCEW pResources, pBigger;
    DWORD dwSize = 0x1000, dwBufferSize = 0x1000, dwCount, i, dwError;
    HANDLE hEnum;

    if (WNetOpenEnumW(RESOURCE_CONNECTED, dwType, 0, NULL, &hEnum) != NO_ERROR)
        return cConnections;

    pResources = HeapAlloc(GetProcessHeap(), 0, dwBufferSize);
    if (pResources == NULL)
    {
        WNetCloseEnum(hEnum);
        return cConnections;
    }

    for (;;)
    {
        dwSize = dwBufferSize;
        dwCount = (DWORD)-1;
        dwError = WNetEnumResourceW(hEnum, &dwCount, pResources, &dwSize);
        if (dwError == ERROR_MORE_DATA && dwSize > dwBufferSize)
        {
            /* The next entry does not fit: retry with the size the call asks for */
            pBigger = HeapReAlloc(GetProcessHeap(), 0, pResources, dwSize);
            if (pBigger == NULL)
                break;
            pResources = pBigger;
            dwBufferSize = dwSize;
            continue;
        }

        if (dwError != NO_ERROR && dwError != ERROR_MORE_DATA)
            break;

        for (i = 0; i < dwCount; i++)
        {
            PWSTR pszLocal, pszRemote;

            if (pResources[i].lpLocalName == NULL || pResources[i].lpRemoteName == NULL)
                continue;

            pszLocal = _wcsdup(pResources[i].lpLocalName);
            pszRemote = _wcsdup(pResources[i].lpRemoteName);
            if (pConnections == NULL)
                pNew = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (cConnections + 1) * sizeof(*pNew));
            else
                pNew = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pConnections, (cConnections + 1) * sizeof(*pNew));
            if (pszLocal == NULL || pszRemote == NULL || pNew == NULL)
            {
                /* The column stays blank for what cannot be listed */
                free(pszLocal);
                free(pszRemote);
                if (pNew)
                    pConnections = pNew;
                dwError = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }

            pConnections = pNew;
            pConnections[cConnections].pszLocal = pszLocal;
            pConnections[cConnections].pszRemote = pszRemote;
            cConnections++;
        }

        if (dwError == ERROR_NOT_ENOUGH_MEMORY)
            break;
    }

    HeapFree(GetProcessHeap(), 0, pResources);
    WNetCloseEnum(hEnum);

    *ppConnections = pConnections;
    return cConnections;
}


/* The mapped drives and printer ports, to show them as the use of a share */
static
DWORD
GetConnections(
    VIEW_CONNECTION **ppConnections)
{
    DWORD cConnections;

    *ppConnections = NULL;
    cConnections = GetConnectionsOfType(RESOURCETYPE_DISK, ppConnections, 0);
    return GetConnectionsOfType(RESOURCETYPE_PRINT, ppConnections, cConnections);
}


static
VOID
FreeShares(
    VIEW_SHARE *pShares,
    DWORD cShares)
{
    DWORD i;

    for (i = 0; i < cShares; i++)
    {
        free(pShares[i].pszName);
        free(pShares[i].pszComment);
    }

    if (pShares)
        HeapFree(GetProcessHeap(), 0, pShares);
}


static
INT
EnumerateServerShares(
    PWSTR pszServer,
    BOOL bAll,
    BOOL bCache)
{
    PSHARE_INFO_1 pBuffer = NULL;
    VIEW_SHARE *pShares = NULL, *pNewShares;
    VIEW_CONNECTION *pConnections = NULL;
    DWORD dwRead = 0, dwTotal = 0, ResumeHandle = 0, i, j, cShares = 0, cConnections = 0;
    size_t nName, nType, nUsed, nComment;
    PWSTR pszBare;
    NET_API_STATUS Status;
    INT result = 0;

    /* All batches are read first: the column widths follow the longest entry */
    do
    {
        Status = NetShareEnum(pszServer,
                              1,
                              (LPBYTE*)&pBuffer,
                              MAX_PREFERRED_LENGTH,
                              &dwRead,
                              &dwTotal,
                              &ResumeHandle);
        if ((Status != NERR_Success) && (Status != ERROR_MORE_DATA))
        {
            result = ViewError(Status);
            goto done;
        }

        for (i = 0; i < dwRead; i++)
        {
            size_t lenName = wcslen(pBuffer[i].shi1_netname);
            PWSTR pszCache = NULL;

            if (!bAll && lenName > 0 && pBuffer[i].shi1_netname[lenName - 1] == L'$')
                continue;

            if (pShares == NULL)
                pNewShares = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (cShares + 1) * sizeof(*pNewShares));
            else
                pNewShares = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pShares, (cShares + 1) * sizeof(*pNewShares));
            if (pNewShares == NULL)
            {
                NetApiBufferFree(pBuffer);
                result = ViewError(ERROR_NOT_ENOUGH_MEMORY);
                goto done;
            }

            pShares = pNewShares;
            pShares[cShares].pszName = _wcsdup(pBuffer[i].shi1_netname);
            pShares[cShares].dwType = pBuffer[i].shi1_type;

            if (bCache)
                pszCache = GetExeMessage(CachingMessage(pszServer, pBuffer[i].shi1_netname));

            pShares[cShares].pszComment = _wcsdup(pszCache ? pszCache : (pBuffer[i].shi1_remark ? pBuffer[i].shi1_remark : L""));
            if (pszCache)
                LocalFree(pszCache);

            cShares++;

            if (pShares[cShares - 1].pszName == NULL || pShares[cShares - 1].pszComment == NULL)
            {
                NetApiBufferFree(pBuffer);
                result = ViewError(ERROR_NOT_ENOUGH_MEMORY);
                goto done;
            }
        }

        NetApiBufferFree(pBuffer);
        pBuffer = NULL;
    }
    while (Status == ERROR_MORE_DATA);

    if (cShares == 0)
    {
        PrintMessageString(3683);
        ConPuts(StdOut, L"\n");
        goto done;
    }

    cConnections = GetConnections(&pConnections);

    /* Each column is as wide as its label or its longest entry, plus two */
    nName = MessageLength(4744);
    nType = MessageLength(4745);
    nUsed = MessageLength(4746);
    nComment = MessageLength(4747);
    for (i = 0; i < cShares; i++)
    {
        DWORD dwTypeMessage = ShareTypeMessage(pShares[i].dwType);

        nName = max(nName, wcslen(pShares[i].pszName));
        nComment = max(nComment, wcslen(pShares[i].pszComment));
        if (dwTypeMessage != 0)
            nType = max(nType, MessageLength(dwTypeMessage));
    }
    for (j = 0; j < cConnections; j++)
        nUsed = max(nUsed, wcslen(pConnections[j].pszLocal));

    nName += 2;
    nType += 2;
    nUsed += 2;
    nComment += 2;

    pszBare = pszServer;
    while (*pszBare == L'\\')
        pszBare++;

    PrintMessageStringV(4377, pszServer);
    ConPuts(StdOut, L"\n\n\n");
    PrintColumn(4744, nName);
    PrintColumn(4745, nType);
    PrintColumn(4746, nUsed);
    PrintColumn(4747, nComment);
    ConPuts(StdOut, L"\n\n");
    PrintPadding(L'-', 79);
    ConPuts(StdOut, L"\n");

    for (i = 0; i < cShares; i++)
    {
        DWORD dwTypeMessage = ShareTypeMessage(pShares[i].dwType);
        PCWSTR pszUsedAs = L"";
        WCHAR szRemote[MAX_PATH];

        ConPrintf(StdOut, L"%-*s", (int)nName, pShares[i].pszName);
        if (dwTypeMessage != 0)
            PrintColumn(dwTypeMessage, nType);
        else
            PrintSpaces(nType);

        StringCchPrintfW(szRemote, ARRAYSIZE(szRemote), L"\\\\%s\\%s", pszBare, pShares[i].pszName);
        for (j = 0; j < cConnections; j++)
        {
            if (_wcsicmp(pConnections[j].pszRemote, szRemote) == 0)
            {
                pszUsedAs = pConnections[j].pszLocal;
                break;
            }
        }

        ConPrintf(StdOut, L"%-*s", (int)nUsed, pszUsedAs);
        ConPrintf(StdOut, L"%-*s\n", (int)nComment, pShares[i].pszComment);
    }

    PrintMessageString(3500);
    ConPuts(StdOut, L"\n");

done:
    FreeConnections(pConnections, cConnections);
    FreeShares(pShares, cShares);
    return result;
}


INT
cmdView(
    INT argc,
    WCHAR **argv)
{
    PWSTR pszServer = NULL;
    PCWSTR pszDomain = NULL;
    PCWSTR pszValue;
    BOOL bAll = FALSE, bCache = FALSE, bDomain = FALSE;
    INT i;

    for (i = 2; i < argc; i++)
    {
        if (argv[i][0] == L'/')
        {
            if (wcscmp(argv[i], L"/?") == 0)
            {
                return PrintCommandSyntax(MSG_VIEW_SYNTAX, 0, NULL);
            }
            else if (_wcsicmp(argv[i], L"/help") == 0)
            {
                PrintCommandHelp(MSG_VIEW_SYNTAX, MSG_VIEW_HELP);
                return 1;
            }
            else if (IsOption(argv[i], L"all", 1, &pszValue))
            {
                bAll = TRUE;
            }
            else if (IsOption(argv[i], L"cache", 1, &pszValue))
            {
                bCache = TRUE;
            }
            else if (IsOption(argv[i], L"domain", 1, &pszValue))
            {
                bDomain = TRUE;
                pszDomain = (pszValue && *pszValue) ? pszValue : NULL;
            }
            else if (IsOption(argv[i], L"network", 1, &pszValue))
            {
                return PrintCommandError(MSG_INVALID_OPTION, NULL);
            }
            else
            {
                WCHAR szOption[64];
                PWSTR pColon;

                StringCchCopyW(szOption, ARRAYSIZE(szOption), argv[i]);
                pColon = wcschr(szOption, L':');
                if (pColon)
                    *pColon = UNICODE_NULL;
                _wcsupr(szOption);

                return PrintCommandSyntax(MSG_VIEW_SYNTAX, MSG_OPTION_UNKNOWN, szOption);
            }
        }
        else if (pszServer == NULL)
        {
            pszServer = argv[i];
        }
        else
        {
            return PrintCommandSyntax(MSG_VIEW_SYNTAX, 0, NULL);
        }
    }

    if (pszServer != NULL)
        return EnumerateServerShares(pszServer, bAll, bCache);

    return EnumerateServers(pszDomain, bDomain && pszDomain == NULL);
}

/* EOF */
