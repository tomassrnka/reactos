/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/applications/sc/triggerinfo.c
 * PURPOSE:     Query/Set the start and stop triggers of a service
 */

#include "sc.h"

#define MAX_TRIGGERS    64
#define MAX_ITEMS       64
#define MAX_PARTS       (MAX_ITEMS + 3)

/* Subtypes of the triggers that have a fixed one */
static const GUID GuidNetworkOn   = {0x4f27f2de, 0x14e2, 0x430b, {0xa5, 0x49, 0x7c, 0xd4, 0x8c, 0xbc, 0x82, 0x45}};
static const GUID GuidNetworkOff  = {0xcc4ba62a, 0x162e, 0x4648, {0x84, 0x7a, 0xb6, 0xbd, 0xf9, 0x93, 0xe3, 0x35}};
static const GUID GuidDomainJoin  = {0x1ce20aba, 0x9851, 0x4421, {0x94, 0x30, 0x1d, 0xde, 0xb7, 0x66, 0xe8, 0x09}};
static const GUID GuidDomainLeave = {0xddaf516e, 0x58c2, 0x4866, {0x95, 0x74, 0xc3, 0xb6, 0x15, 0xd4, 0x2e, 0xa1}};
static const GUID GuidPortOpen    = {0xb7569e07, 0x8421, 0x4ee0, {0xad, 0x10, 0x86, 0x91, 0x5a, 0xfd, 0xad, 0x09}};
static const GUID GuidPortClose   = {0xa144ed38, 0x8e12, 0x4de4, {0x9d, 0x96, 0xe6, 0x47, 0x40, 0xb1, 0xa5, 0x24}};
static const GUID GuidMachinePolicy = {0x659fcae6, 0x5bdb, 0x4da9, {0xb1, 0xff, 0xca, 0x2a, 0x17, 0x8d, 0x46, 0xe0}};
static const GUID GuidUserPolicy  = {0x54fb46c8, 0xf089, 0x464c, {0xb1, 0xfd, 0x59, 0xd1, 0xb6, 0x2c, 0x3b, 0x50}};
static const GUID GuidRpcEvent    = {0xbc90d167, 0x9470, 0x4139, {0xa9, 0xba, 0xbe, 0x0b, 0xbb, 0xf5, 0xb7, 0x4d}};
static const GUID GuidPipeEvent   = {0x1f81d131, 0x3fac, 0x4537, {0x9e, 0x0c, 0x7e, 0x7b, 0x0c, 0x2f, 0x4b, 0x55}};

typedef struct _TRIGGER_NAME
{
    DWORD dwType;
    const GUID *pGuid;
    LPCTSTR pszLabel;
} TRIGGER_NAME;

/* The names the query gives a subtype of the triggers with a fixed one */
static const TRIGGER_NAME SubtypeNames[] =
{
    { SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY, &GuidNetworkOn,     _T("FIRST IP ADDRESS AVAILABLE") },
    { SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY, &GuidNetworkOff,    _T("NO IP ADDRESS AVAILABLE") },
    { SERVICE_TRIGGER_TYPE_DOMAIN_JOIN,             &GuidDomainJoin,    _T("DOMAIN JOINED") },
    { SERVICE_TRIGGER_TYPE_DOMAIN_JOIN,             &GuidDomainLeave,   _T("NOT DOMAIN JOINED") },
    { SERVICE_TRIGGER_TYPE_FIREWALL_PORT_EVENT,     &GuidPortOpen,      _T("PORT OPEN") },
    { SERVICE_TRIGGER_TYPE_FIREWALL_PORT_EVENT,     &GuidPortClose,     _T("PORT CLOSE") },
    { SERVICE_TRIGGER_TYPE_GROUP_POLICY,            &GuidMachinePolicy, _T("MACHINE POLICY PRESENT") },
    { SERVICE_TRIGGER_TYPE_GROUP_POLICY,            &GuidUserPolicy,    _T("USER POLICY PRESENT") },
    { SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT,        &GuidRpcEvent,      _T("RPC INTERFACE EVENT") },
    { SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT,        &GuidPipeEvent,     _T("NAMED PIPE EVENT") },
};

static VOID
TriggerInfoUsage(VOID)
{
    _tprintf(_T("DESCRIPTION:\n")
             _T("        Changes the trigger parameters of a service.\n")
             _T("USAGE:\n")
             _T("        sc <server> triggerinfo [service name] <option1> <option2>...\n")
             _T("\n")
             _T("OPTIONS:\n")
             _T("        start/device/UUID/HwId1/...     Start on arrival of a device interface class,\n")
             _T("                                        for the hardware or compatible IDs given\n")
             _T("        start/custom/UUID/data0/..      Start on an event of an ETW provider, with\n")
             _T("        stop/custom/UUID/data0/...      binary data items in hexadecimal (ABCDABCD)\n")
             _T("        start/strcustom/UUID/data0/..   The same with string data items\n")
             _T("        stop/strcustom/UUID/data0/..\n")
             _T("        start/lvlcustom/UUID/data0/..   The same for events of at least a level (0x01)\n")
             _T("        stop/lvlcustom/UUID/data0/..\n")
             _T("        start/kwanycustom/UUID/data0/.. The same for events with any of the keyword\n")
             _T("        stop/kwanycustom/UUID/data0/..  bits (0x0A03)\n")
             _T("        start/kwallcustom/UUID/data0/.. The same for events with all of the keyword\n")
             _T("        stop/kwallcustom/UUID/data0/..  bits (0x0A03)\n")
             _T("        start/networkon                 Start on the first IP address\n")
             _T("        stop/networkoff                 Stop on the last IP address gone\n")
             _T("        start/domainjoin                Start on domain join\n")
             _T("        stop/domainleave                Stop on domain leave\n")
             _T("        start/portopen/parameter        Start on the opening of a network port; the\n")
             _T("        stop/portclose/parameter        parameter is portnumber;protocol;imagepath;service\n")
             _T("        start/machinepolicy             Start when machine group policy is present\n")
             _T("        start/userpolicy                Start when user group policy is present\n")
             _T("        start/rpcinterface/UUID         Start on a request for an RPC interface\n")
             _T("        start/namedpipe/pipename        Start on a request for a named pipe, the name\n")
             _T("                                        without the \\\\.\\pipe\\ part\n")
             _T("        delete                          Delete the existing trigger parameters\n"));
}

VOID
QueryTriggerInfoUsage(VOID)
{
    _tprintf(_T("DESCRIPTION:\n")
             _T("        Retrieves the trigger information of a service.\n")
             _T("USAGE:\n")
             _T("        sc <server> qtriggerinfo [service name] <bufferSize>\n"));
}

VOID
SetPrivilegesUsage(VOID)
{
    _tprintf(_T("DESCRIPTION:\n")
             _T("        Changes the required privileges setting of a service. The privileges\n")
             _T("        are applied when the service process starts; a service without the\n")
             _T("        setting requires all that its account is allowed.\n")
             _T("USAGE:\n")
             _T("        sc <server> privs [service name] [privileges]\n")
             _T("\n")
             _T("OPTIONS:\n")
             _T("        privileges = <Privileges(separated by / (forward slash))>\n")
             _T("                     [E.g., SeBackupPrivilege/SeRestorePrivilege]\n"));
}

VOID
QueryPrivilegesUsage(VOID)
{
    _tprintf(_T("DESCRIPTION:\n")
             _T("        Queries the required privileges setting of a service.\n")
             _T("USAGE:\n")
             _T("        sc <server> qprivs [service name] <bufferSize>\n"));
}

VOID
SetTriggerInfoUsage(VOID)
{
    TriggerInfoUsage();
}


/* ------------------------------------------------------------------ parsing */

static BOOL
HexValue(TCHAR ch,
         PDWORD pdwValue)
{
    if (ch >= _T('0') && ch <= _T('9'))
        *pdwValue = ch - _T('0');
    else if (ch >= _T('a') && ch <= _T('f'))
        *pdwValue = ch - _T('a') + 10;
    else if (ch >= _T('A') && ch <= _T('F'))
        *pdwValue = ch - _T('A') + 10;
    else
        return FALSE;
    return TRUE;
}

/* A GUID as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, without braces */
static BOOL
ParseGuid(LPCTSTR psz,
          GUID *pGuid)
{
    DWORD dwValue, dwDigit;
    BYTE Bytes[16];
    int i, nByte = 0;
    ULONG ulData1 = 0;
    USHORT usData2 = 0, usData3 = 0;

    if (_tcslen(psz) != 36 || psz[8] != _T('-') || psz[13] != _T('-') ||
        psz[18] != _T('-') || psz[23] != _T('-'))
        return FALSE;

    for (i = 0; i < 36; i++)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            continue;
        if (!HexValue(psz[i], &dwDigit))
            return FALSE;
    }

    for (i = 0; i < 8; i++) { HexValue(psz[i], &dwValue); ulData1 = (ulData1 << 4) | dwValue; }
    for (i = 9; i < 13; i++) { HexValue(psz[i], &dwValue); usData2 = (USHORT)((usData2 << 4) | dwValue); }
    for (i = 14; i < 18; i++) { HexValue(psz[i], &dwValue); usData3 = (USHORT)((usData3 << 4) | dwValue); }

    for (i = 19; i < 36; i++)
    {
        DWORD dwHigh;

        if (i == 23)
            continue;
        HexValue(psz[i], &dwHigh);
        HexValue(psz[i + 1], &dwValue);
        Bytes[nByte++] = (BYTE)((dwHigh << 4) | dwValue);
        i++;
    }

    pGuid->Data1 = ulData1;
    pGuid->Data2 = usData2;
    pGuid->Data3 = usData3;
    memcpy(pGuid->Data4, Bytes, 8);
    return TRUE;
}

/* Hexadecimal text as the bytes it stands for; FALSE if it is not an even number of digits */
static BOOL
ParseHexBytes(LPCTSTR psz,
              PBYTE *ppData,
              PDWORD pcbData)
{
    SIZE_T cch = _tcslen(psz), i;
    PBYTE pData;
    DWORD dwHigh, dwLow;

    *ppData = NULL;
    if (cch == 0 || (cch % 2) != 0)
        return FALSE;

    pData = HeapAlloc(GetProcessHeap(), 0, cch / 2);
    if (pData == NULL)
        return FALSE;

    for (i = 0; i < cch; i += 2)
    {
        if (!HexValue(psz[i], &dwHigh) || !HexValue(psz[i + 1], &dwLow))
        {
            HeapFree(GetProcessHeap(), 0, pData);
            return FALSE;
        }
        pData[i / 2] = (BYTE)((dwHigh << 4) | dwLow);
    }

    *ppData = pData;
    *pcbData = (DWORD)(cch / 2);
    return TRUE;
}

/* A hexadecimal number, with or without 0x */
static BOOL
ParseHexNumber(LPCTSTR psz,
               PULONGLONG pullValue)
{
    DWORD dwDigit;
    ULONGLONG ullValue = 0;

    if (psz[0] == _T('0') && (psz[1] == _T('x') || psz[1] == _T('X')))
        psz += 2;
    if (*psz == _T('\0'))
        return FALSE;

    for (; *psz != _T('\0'); psz++)
    {
        if (!HexValue(*psz, &dwDigit))
            return FALSE;
        ullValue = (ullValue << 4) | dwDigit;
    }

    *pullValue = ullValue;
    return TRUE;
}

typedef struct _TRIGGER_BUILD
{
    SERVICE_TRIGGER Triggers[MAX_TRIGGERS];
    GUID Guids[MAX_TRIGGERS];
    SERVICE_TRIGGER_SPECIFIC_DATA_ITEM Items[MAX_TRIGGERS][MAX_ITEMS];
    PVOID Allocations[MAX_TRIGGERS * MAX_ITEMS];
    DWORD cAllocations;
} TRIGGER_BUILD, *PTRIGGER_BUILD;

static BOOL
AddItem(PTRIGGER_BUILD pBuild,
        PSERVICE_TRIGGER pTrigger,
        DWORD dwType,
        PBYTE pData,
        DWORD cbData)
{
    PSERVICE_TRIGGER_SPECIFIC_DATA_ITEM pItem;

    pBuild->Allocations[pBuild->cAllocations++] = pData;
    pItem = &pTrigger->pDataItems[pTrigger->cDataItems++];
    pItem->dwDataType = dwType;
    pItem->cbData = cbData;
    pItem->pData = pData;
    return TRUE;
}

static BOOL
AddStringItem(PTRIGGER_BUILD pBuild,
              PSERVICE_TRIGGER pTrigger,
              LPCTSTR pszText,
              LPCTSTR pszSuffix)
{
    SIZE_T cch = _tcslen(pszText) + _tcslen(pszSuffix) + 1;
    LPTSTR psz = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cch * sizeof(TCHAR));

    if (psz == NULL)
        return FALSE;
    _tcscpy(psz, pszText);
    _tcscat(psz, pszSuffix);
    return AddItem(pBuild, pTrigger, SERVICE_TRIGGER_DATA_TYPE_STRING, (PBYTE)psz, (DWORD)(cch * sizeof(TCHAR)));
}

/*
 * One option, start/<kind>/<parameters> or stop/<kind>/<parameters>, as a trigger. Returns ERROR_SUCCESS, or
 * ERROR_INVALID_COMMAND_LINE for an option that is not understood (the usage is shown), or ERROR_INVALID_DATA
 * for hexadecimal data that is not (reported here).
 */
static DWORD
ParseOption(PTRIGGER_BUILD pBuild,
            DWORD dwIndex,
            LPCTSTR pszOption)
{
    PSERVICE_TRIGGER pTrigger = &pBuild->Triggers[dwIndex];
    LPCTSTR Parts[MAX_PARTS];
    LPTSTR pszCopy, psz;
    DWORD cParts = 0, i, dwError = ERROR_SUCCESS;
    BOOL bStart;
    LPCTSTR pszKind;
    ULONGLONG ullValue;

    pszCopy = HeapAlloc(GetProcessHeap(), 0, (_tcslen(pszOption) + 1) * sizeof(TCHAR));
    if (pszCopy == NULL)
        return ERROR_OUTOFMEMORY;
    _tcscpy(pszCopy, pszOption);

    for (psz = pszCopy; psz != NULL && cParts < MAX_PARTS; )
    {
        Parts[cParts++] = psz;
        psz = _tcschr(psz, _T('/'));
        if (psz != NULL)
            *psz++ = _T('\0');
    }

    if (cParts < 2)
        goto usage;

    if (!_tcsicmp(Parts[0], _T("start")))
        bStart = TRUE;
    else if (!_tcsicmp(Parts[0], _T("stop")))
        bStart = FALSE;
    else
        goto usage;

    pszKind = Parts[1];
    pTrigger->dwAction = bStart ? SERVICE_TRIGGER_ACTION_SERVICE_START : SERVICE_TRIGGER_ACTION_SERVICE_STOP;
    pTrigger->pTriggerSubtype = &pBuild->Guids[dwIndex];
    pTrigger->pDataItems = pBuild->Items[dwIndex];
    pTrigger->cDataItems = 0;

    if (!_tcsicmp(pszKind, _T("networkon")) || !_tcsicmp(pszKind, _T("networkoff")))
    {
        pTrigger->dwTriggerType = SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY;
        *pTrigger->pTriggerSubtype = !_tcsicmp(pszKind, _T("networkon")) ? GuidNetworkOn : GuidNetworkOff;
    }
    else if (!_tcsicmp(pszKind, _T("domainjoin")) || !_tcsicmp(pszKind, _T("domainleave")))
    {
        pTrigger->dwTriggerType = SERVICE_TRIGGER_TYPE_DOMAIN_JOIN;
        *pTrigger->pTriggerSubtype = !_tcsicmp(pszKind, _T("domainjoin")) ? GuidDomainJoin : GuidDomainLeave;
    }
    else if (!_tcsicmp(pszKind, _T("machinepolicy")) || !_tcsicmp(pszKind, _T("userpolicy")))
    {
        pTrigger->dwTriggerType = SERVICE_TRIGGER_TYPE_GROUP_POLICY;
        *pTrigger->pTriggerSubtype = !_tcsicmp(pszKind, _T("machinepolicy")) ? GuidMachinePolicy : GuidUserPolicy;
    }
    else if (!_tcsicmp(pszKind, _T("portopen")) || !_tcsicmp(pszKind, _T("portclose")))
    {
        if (cParts < 3)
            goto usage;

        pTrigger->dwTriggerType = SERVICE_TRIGGER_TYPE_FIREWALL_PORT_EVENT;
        *pTrigger->pTriggerSubtype = !_tcsicmp(pszKind, _T("portopen")) ? GuidPortOpen : GuidPortClose;
        if (!AddStringItem(pBuild, pTrigger, Parts[2], _T(";")))
            dwError = ERROR_OUTOFMEMORY;
    }
    else if (!_tcsicmp(pszKind, _T("rpcinterface")) || !_tcsicmp(pszKind, _T("namedpipe")))
    {
        if (cParts < 3)
            goto usage;

        pTrigger->dwTriggerType = SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT;
        *pTrigger->pTriggerSubtype = !_tcsicmp(pszKind, _T("rpcinterface")) ? GuidRpcEvent : GuidPipeEvent;
        if (!AddStringItem(pBuild, pTrigger, Parts[2], _T("")))
            dwError = ERROR_OUTOFMEMORY;
    }
    else if (!_tcsicmp(pszKind, _T("device")))
    {
        if (cParts < 3 || !ParseGuid(Parts[2], pTrigger->pTriggerSubtype))
            goto usage;

        pTrigger->dwTriggerType = SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL;
        for (i = 3; i < cParts && dwError == ERROR_SUCCESS; i++)
        {
            if (!AddStringItem(pBuild, pTrigger, Parts[i], _T("")))
                dwError = ERROR_OUTOFMEMORY;
        }
    }
    else if (!_tcsicmp(pszKind, _T("custom")) || !_tcsicmp(pszKind, _T("strcustom")) ||
             !_tcsicmp(pszKind, _T("lvlcustom")) || !_tcsicmp(pszKind, _T("kwanycustom")) ||
             !_tcsicmp(pszKind, _T("kwallcustom")))
    {
        if (cParts < 3 || !ParseGuid(Parts[2], pTrigger->pTriggerSubtype))
            goto usage;

        pTrigger->dwTriggerType = SERVICE_TRIGGER_TYPE_CUSTOM;
        for (i = 3; i < cParts && dwError == ERROR_SUCCESS; i++)
        {
            if (!_tcsicmp(pszKind, _T("strcustom")))
            {
                if (!AddStringItem(pBuild, pTrigger, Parts[i], _T("")))
                    dwError = ERROR_OUTOFMEMORY;
            }
            else if (!_tcsicmp(pszKind, _T("custom")))
            {
                PBYTE pData;
                DWORD cbData;

                if (!ParseHexBytes(Parts[i], &pData, &cbData))
                {
                    ReportApiFailure(_T("CryptStringToBinary: Hex data string to binary conversion"), ERROR_INVALID_DATA);
                    dwError = ERROR_INVALID_DATA;
                    break;
                }
                AddItem(pBuild, pTrigger, SERVICE_TRIGGER_DATA_TYPE_BINARY, pData, cbData);
            }
            else
            {
                PBYTE pData;
                BOOL bLevel = !_tcsicmp(pszKind, _T("lvlcustom"));

                if (!ParseHexNumber(Parts[i], &ullValue))
                    goto usage;

                pData = HeapAlloc(GetProcessHeap(), 0, bLevel ? sizeof(BYTE) : sizeof(ULONGLONG));
                if (pData == NULL)
                {
                    dwError = ERROR_OUTOFMEMORY;
                    break;
                }

                if (bLevel)
                    *pData = (BYTE)ullValue;
                else
                    memcpy(pData, &ullValue, sizeof(ULONGLONG));

                AddItem(pBuild, pTrigger,
                        bLevel ? SERVICE_TRIGGER_DATA_TYPE_LEVEL :
                        (!_tcsicmp(pszKind, _T("kwanycustom")) ? SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ANY
                                                               : SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ALL),
                        pData, bLevel ? sizeof(BYTE) : sizeof(ULONGLONG));
            }
        }
    }
    else
    {
        goto usage;
    }

    HeapFree(GetProcessHeap(), 0, pszCopy);
    return dwError;

usage:
    HeapFree(GetProcessHeap(), 0, pszCopy);
    return ERROR_INVALID_COMMAND_LINE;
}


/* sc triggerinfo <service> <option>... */
DWORD
SetTriggerInfo(LPCTSTR ServiceName,
               LPCTSTR *Options,
               INT OptionCount)
{
    SC_HANDLE hManager, hService;
    PTRIGGER_BUILD pBuild;
    SERVICE_TRIGGER_INFO Info;
    DWORD dwError = ERROR_SUCCESS, i, cTriggers = 0;

    pBuild = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pBuild));
    if (pBuild == NULL)
    {
        ReportApiFailure(_T("HeapAlloc"), ERROR_OUTOFMEMORY);
        return ERROR_OUTOFMEMORY;
    }

    if (_tcsicmp(Options[0], _T("delete")) != 0)
    {
        if (OptionCount > MAX_TRIGGERS)
            OptionCount = MAX_TRIGGERS;

        for (i = 0; i < (DWORD)OptionCount; i++)
        {
            dwError = ParseOption(pBuild, i, Options[i]);
            if (dwError == ERROR_INVALID_COMMAND_LINE)
                TriggerInfoUsage();
            if (dwError != ERROR_SUCCESS)
                goto done;
        }
        cTriggers = OptionCount;
    }

    dwError = OpenServiceForConfig(ServiceName, SERVICE_CHANGE_CONFIG, &hManager, &hService);
    if (dwError != ERROR_SUCCESS)
        goto done;

    Info.cTriggers = cTriggers;
    Info.pTriggers = cTriggers ? pBuild->Triggers : NULL;
    Info.pReserved = NULL;
    if (!ChangeServiceConfig2(hService, SERVICE_CONFIG_TRIGGER_INFO, &Info))
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
    for (i = 0; i < pBuild->cAllocations; i++)
        HeapFree(GetProcessHeap(), 0, pBuild->Allocations[i]);
    HeapFree(GetProcessHeap(), 0, pBuild);
    return dwError;
}


/* ------------------------------------------------------------------ query */

static VOID
PrintGuid(const GUID *pGuid)
{
    _tprintf(_T("%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x"),
             pGuid->Data1, pGuid->Data2, pGuid->Data3,
             pGuid->Data4[0], pGuid->Data4[1], pGuid->Data4[2], pGuid->Data4[3],
             pGuid->Data4[4], pGuid->Data4[5], pGuid->Data4[6], pGuid->Data4[7]);
}

static LPCTSTR
GetTriggerTypeName(DWORD dwType)
{
    switch (dwType)
    {
        case SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL: return _T("DEVICE INTERFACE ARRIVAL");
        case SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY:  return _T("IP ADDRESS AVAILABILITY");
        case SERVICE_TRIGGER_TYPE_DOMAIN_JOIN:              return _T("DOMAIN JOINED STATUS");
        case SERVICE_TRIGGER_TYPE_FIREWALL_PORT_EVENT:      return _T("FIREWALL PORT EVENT");
        case SERVICE_TRIGGER_TYPE_GROUP_POLICY:             return _T("GROUP POLICY");
        case SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT:         return _T("NETWORK EVENT");
        case SERVICE_TRIGGER_TYPE_CUSTOM:                   return _T("CUSTOM");
    }
    return _T("UNKNOWN");
}

static VOID
PrintTrigger(const SERVICE_TRIGGER *pTrigger)
{
    LPCTSTR pszLabel = NULL;
    DWORD i, j;

    _tprintf(pTrigger->dwAction == SERVICE_TRIGGER_ACTION_SERVICE_START ? _T("        START SERVICE \n")
                                                                         : _T("        STOP SERVICE\n"));

    _tprintf(_T("          %-29s: "), GetTriggerTypeName(pTrigger->dwTriggerType));
    PrintGuid(pTrigger->pTriggerSubtype);

    if (pTrigger->dwTriggerType == SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL)
        pszLabel = _T("INTERFACE CLASS GUID");
    else if (pTrigger->dwTriggerType == SERVICE_TRIGGER_TYPE_CUSTOM)
        pszLabel = _T("ETW PROVIDER UUID");
    else
    {
        for (i = 0; i < ARRAYSIZE(SubtypeNames); i++)
        {
            if (SubtypeNames[i].dwType == pTrigger->dwTriggerType &&
                IsEqualGUID(SubtypeNames[i].pGuid, pTrigger->pTriggerSubtype))
            {
                pszLabel = SubtypeNames[i].pszLabel;
                break;
            }
        }
    }
    if (pszLabel != NULL)
        _tprintf(_T(" [%s]"), pszLabel);
    _tprintf(_T("\n"));

    for (i = 0; i < pTrigger->cDataItems; i++)
    {
        const SERVICE_TRIGGER_SPECIFIC_DATA_ITEM *pItem = &pTrigger->pDataItems[i];

        _tprintf(_T("            %-27s: "), _T("DATA"));
        switch (pItem->dwDataType)
        {
            case SERVICE_TRIGGER_DATA_TYPE_STRING:
                _tprintf(_T("%.*s"), (int)(pItem->cbData / sizeof(WCHAR)), (LPCWSTR)pItem->pData);
                break;

            case SERVICE_TRIGGER_DATA_TYPE_LEVEL:
                _tprintf(_T("0x%02x [%lu]"), pItem->pData[0], pItem->dwDataType);
                break;

            case SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ANY:
            case SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ALL:
            {
                ULONGLONG ullValue = 0;

                memcpy(&ullValue, pItem->pData, min(pItem->cbData, sizeof(ullValue)));
                _tprintf(_T("0x%016I64x [%lu]"), ullValue, pItem->dwDataType);
                break;
            }

            default:
                for (j = 0; j < pItem->cbData; j++)
                    _tprintf(j ? _T(" %02x") : _T("%02x"), pItem->pData[j]);
                break;
        }
        _tprintf(_T("\n"));
    }
}


/* sc qtriggerinfo <service> <buffer size> */
DWORD
QueryTriggerInfo(LPCTSTR ServiceName,
                 LPCTSTR BufferSize)
{
    SC_HANDLE hManager, hService;
    LPBYTE pBuffer;
    PSERVICE_TRIGGER_INFO pInfo;
    DWORD dwError, i;

    dwError = OpenServiceForConfig(ServiceName, SERVICE_QUERY_CONFIG, &hManager, &hService);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    dwError = QueryConfigLevel(hService, SERVICE_CONFIG_TRIGGER_INFO, BufferSize, &pBuffer);
    if (dwError == ERROR_SUCCESS)
    {
        pInfo = (PSERVICE_TRIGGER_INFO)pBuffer;

        _tprintf(_T("[SC] QueryServiceConfig2 SUCCESS\n\n"));
        if (pInfo->cTriggers == 0)
        {
            _tprintf(_T("    The service %s has not registered for any start or stop triggers.\n"), ServiceName);
        }
        else
        {
            _tprintf(_T("SERVICE_NAME: %s\n\n"), ServiceName);
            for (i = 0; i < pInfo->cTriggers; i++)
                PrintTrigger(&pInfo->pTriggers[i]);
        }

        HeapFree(GetProcessHeap(), 0, pBuffer);
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hManager);
    return dwError;
}
