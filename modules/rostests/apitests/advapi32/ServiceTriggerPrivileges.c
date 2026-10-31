/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO and SERVICE_CONFIG_TRIGGER_INFO of
 *              QueryServiceConfig2 and ChangeServiceConfig2, the owner of the service database lock,
 *              and the size of EnumDependentServices
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * It creates services of its own, and runs on Windows.
 */

#include "precomp.h"

#define SERVICE_NAME    L"advapi32_apitest_TriggerPriv"
#define DEP_BASE        L"advapi32_apitest_DepBase"
#define DEP_ONE         L"advapi32_apitest_DepOne"
#define DEP_TWO         L"advapi32_apitest_DepTwo"
#define DEP_THREE       L"advapi32_apitest_DepThree"

#define LEVEL_PRIVILEGES    SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO
#define LEVEL_TRIGGERS      SERVICE_CONFIG_TRIGGER_INFO

static const GUID IpGuid = {0x4f27f2de, 0x14e2, 0x430b, {0xa5, 0x49, 0x7c, 0xd4, 0x8c, 0xbc, 0x82, 0x45}};
static const GUID PolicyGuid = {0x659fcae6, 0x5bdb, 0x4da9, {0xb1, 0xff, 0xca, 0x2a, 0x17, 0x8d, 0x46, 0xe0}};
static const GUID CustomGuid = {0x11111111, 0x2222, 0x3333, {0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55}};
static const GUID DeviceGuid = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
static const GUID PipeGuid = {0x1f81d131, 0x3fac, 0x4537, {0x9e, 0x0c, 0x7e, 0x7b, 0x0c, 0x2f, 0x4b, 0x55}};

static SC_HANDLE Manager;

static BOOL InBuffer(const void *p, const void *Buffer, DWORD Size, SIZE_T Length)
{
    return (const BYTE *)p >= (const BYTE *)Buffer && (SIZE_T)Size >= Length &&
           (const BYTE *)p <= (const BYTE *)Buffer + Size - Length;
}

static SC_HANDLE CreateTestService(PCWSTR Name, PCWSTR Dependencies)
{
    return CreateServiceW(Manager, Name, Name, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                          SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, L"C:\\ReactOS\\system32\\cmd.exe",
                          NULL, NULL, Dependencies, NULL, NULL);
}

static void DeleteTestService(PCWSTR Name)
{
    SC_HANDLE Service = OpenServiceW(Manager, Name, DELETE);

    if (Service)
    {
        DeleteService(Service);
        CloseServiceHandle(Service);
    }
}

/* ------------------------------------------------------------------ the registry */

static BYTE *ReadValue(PCWSTR Service, PCWSTR SubKey, PCWSTR Value, DWORD *Type, DWORD *Size)
{
    WCHAR Path[256];
    HKEY Key;
    BYTE *Data;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"SYSTEM\\CurrentControlSet\\Services\\%ls%ls%ls", Service,
                     SubKey[0] ? L"\\" : L"", SubKey);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return NULL;
    *Size = 0;
    if (RegQueryValueExW(Key, Value, NULL, Type, NULL, Size) != ERROR_SUCCESS)
    {
        RegCloseKey(Key);
        return NULL;
    }
    Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *Size + sizeof(WCHAR));
    if (Data && RegQueryValueExW(Key, Value, NULL, Type, Data, Size) != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, Data);
        Data = NULL;
    }
    RegCloseKey(Key);
    return Data;
}

static BOOL KeyExists(PCWSTR Service, PCWSTR SubKey)
{
    WCHAR Path[256];
    HKEY Key;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"SYSTEM\\CurrentControlSet\\Services\\%ls\\%ls", Service, SubKey);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(Key);
    return TRUE;
}

/* ------------------------------------------------------------------ required privileges */

/* The characters of a multi-string with the ending null of each string and the empty string */
static DWORD MultiLength(PCWSTR Multi)
{
    PCWSTR p = Multi;

    while (*p)
        p += wcslen(p) + 1;
    return (DWORD)(p - Multi) + 1;
}

/* Queries the privileges and checks the buffer; returns it (and the multi-string in it), or NULL */
static BYTE *QueryPrivileges(SC_HANDLE Service, BOOL Unicode, DWORD *Size, PVOID *Multi)
{
    DWORD Needed = 0xdeadbeef, Needed2;
    BYTE *Buffer;
    BOOL Ret;

    *Multi = NULL;
    SetLastError(0xdeadbeef);
    Ret = Unicode ? QueryServiceConfig2W(Service, LEVEL_PRIVILEGES, NULL, 0, &Needed)
                  : QueryServiceConfig2A(Service, LEVEL_PRIVILEGES, NULL, 0, &Needed);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "privileges: Ret %d, error %lu\n", Ret, GetLastError());
    ok(Needed >= sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW) && Needed < 8192, "privileges: Needed %lu\n", Needed);
    if (Ret || Needed < sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW) || Needed >= 8192)
        return NULL;

    *Size = Needed;
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Needed);
    if (!Buffer)
        return NULL;

    /* One byte short is short */
    Needed2 = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = Unicode ? QueryServiceConfig2W(Service, LEVEL_PRIVILEGES, Buffer, Needed - 1, &Needed2)
                  : QueryServiceConfig2A(Service, LEVEL_PRIVILEGES, Buffer, Needed - 1, &Needed2);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "privileges, one byte short: Ret %d, error %lu\n",
       Ret, GetLastError());
    ok(Needed2 == Needed, "privileges: Needed %lu, %lu with one byte short\n", Needed, Needed2);

    SetLastError(0xdeadbeef);
    Needed2 = 0xdeadbeef;
    Ret = Unicode ? QueryServiceConfig2W(Service, LEVEL_PRIVILEGES, Buffer, Needed, &Needed2)
                  : QueryServiceConfig2A(Service, LEVEL_PRIVILEGES, Buffer, Needed, &Needed2);
    ok(Ret, "privileges: failed with the size it asked for, error %lu\n", GetLastError());
    if (!Ret)
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return NULL;
    }
    *Multi = ((SERVICE_REQUIRED_PRIVILEGES_INFOW *)Buffer)->pmszRequiredPrivileges;
    return Buffer;
}

/* The privileges the service has now are exactly Expected (a multi-string), or none if it is NULL */
static void CheckPrivileges(SC_HANDLE Service, PCWSTR Expected, PCSTR ExpectedA, const char *What)
{
    DWORD Size, Type, RegSize;
    BYTE *Buffer, *Value;
    PVOID Multi;

    Buffer = QueryPrivileges(Service, TRUE, &Size, &Multi);
    if (Buffer)
    {
        if (!Expected)
        {
            ok(Multi == NULL, "%s: privileges %p, expected none\n", What, Multi);
            ok(Size == sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW), "%s: Needed %lu\n", What, Size);
        }
        else
        {
            DWORD Length = MultiLength(Expected);

            ok(Size == sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW) + Length * sizeof(WCHAR), "%s: Needed %lu\n", What, Size);
            ok(Multi != NULL && InBuffer(Multi, Buffer, Size, Length * sizeof(WCHAR)) &&
               (BYTE *)Multi >= Buffer + sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW),
               "%s: privileges %p (buffer %p, %lu bytes)\n", What, Multi, Buffer, Size);
            if (Multi && InBuffer(Multi, Buffer, Size, Length * sizeof(WCHAR)))
                ok(!memcmp(Multi, Expected, Length * sizeof(WCHAR)), "%s: privileges differ\n", What);
        }
        HeapFree(GetProcessHeap(), 0, Buffer);
    }

    Buffer = QueryPrivileges(Service, FALSE, &Size, &Multi);
    if (Buffer)
    {
        if (!ExpectedA)
        {
            ok(Multi == NULL, "%s (A): privileges %p, expected none\n", What, Multi);
        }
        else
        {
            PCSTR p = ExpectedA;

            while (*p)
                p += strlen(p) + 1;
            /* the size is that of the Unicode string for the ANSI function as well */
            ok(Size == sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOA) + ((p - ExpectedA) + 1) * sizeof(WCHAR),
               "%s (A): Needed %lu\n", What, Size);
            ok(Multi != NULL && InBuffer(Multi, Buffer, Size, (p - ExpectedA) + 1) &&
               !memcmp(Multi, ExpectedA, (p - ExpectedA) + 1), "%s (A): privileges %p\n", What, Multi);
        }
        HeapFree(GetProcessHeap(), 0, Buffer);
    }

    /* What is kept: a REG_MULTI_SZ value of the service key */
    Value = ReadValue(SERVICE_NAME, L"", L"RequiredPrivileges", &Type, &RegSize);
    if (!Expected)
    {
        ok(Value == NULL, "%s: RequiredPrivileges is in the registry\n", What);
    }
    else
    {
        ok(Value != NULL && Type == REG_MULTI_SZ, "%s: RequiredPrivileges %p, type %lu\n", What, Value, Type);
        if (Value && Type == REG_MULTI_SZ)
            ok(RegSize >= MultiLength(Expected) * sizeof(WCHAR) - sizeof(WCHAR) &&
               !memcmp(Value, Expected, MultiLength(Expected) * sizeof(WCHAR) - sizeof(WCHAR)),
               "%s: RequiredPrivileges differs (%lu bytes)\n", What, RegSize);
    }
    if (Value)
        HeapFree(GetProcessHeap(), 0, Value);
}

static BOOL SetPrivileges(SC_HANDLE Service, PCWSTR Multi, DWORD *Error)
{
    SERVICE_REQUIRED_PRIVILEGES_INFOW Info;
    BOOL Ret;

    Info.pmszRequiredPrivileges = (PWSTR)Multi;
    SetLastError(0xdeadbeef);
    Ret = ChangeServiceConfig2W(Service, LEVEL_PRIVILEGES, &Info);
    *Error = Ret ? ERROR_SUCCESS : GetLastError();
    return Ret;
}

static void TestPrivileges(SC_HANDLE Service)
{
    static const WCHAR Two[] = L"SeChangeNotifyPrivilege\0SeImpersonatePrivilege\0";
    static const CHAR TwoA[] = "SeChangeNotifyPrivilege\0SeImpersonatePrivilege\0";
    static const WCHAR Lower[] = L"sechangenotifyprivilege\0";
    static const WCHAR Tcb[] = L"SeTcbPrivilege\0";
    static const WCHAR Unknown[] = L"SeNoSuchPrivilegeInTheWorld\0";
    static const WCHAR Mixed[] = L"SeTcbPrivilege\0SeNoSuchPrivilegeInTheWorld\0";
    static const WCHAR Empty[] = L"";
    SERVICE_REQUIRED_PRIVILEGES_INFOA InfoA;
    SERVICE_REQUIRED_PRIVILEGES_INFOW InfoW;
    DWORD Error;
    BOOL Ret;

    /* nothing is set */
    CheckPrivileges(Service, NULL, NULL, "initially");

    /* a list, in the order it was given */
    Ret = SetPrivileges(Service, Two, &Error);
    ok(Ret, "set two: error %lu\n", Error);
    CheckPrivileges(Service, Two, TwoA, "two");

    /* the names of the privileges are taken as they come */
    Ret = SetPrivileges(Service, Lower, &Error);
    ok(Ret, "set lower case: error %lu\n", Error);
    CheckPrivileges(Service, Lower, "sechangenotifyprivilege\0", "lower case");

    /* a list that has no privilege that exists changes nothing, and no error */
    Ret = SetPrivileges(Service, Unknown, &Error);
    ok(Ret, "set unknown: error %lu\n", Error);
    CheckPrivileges(Service, Lower, "sechangenotifyprivilege\0", "after unknown");

    /* so does a list of those that exist and those that do not */
    Ret = SetPrivileges(Service, Mixed, &Error);
    ok(Ret, "set mixed: error %lu\n", Error);
    CheckPrivileges(Service, Lower, "sechangenotifyprivilege\0", "after mixed");

    /* the list ends at the first empty string */
    Ret = SetPrivileges(Service, L"SeTcbPrivilege\0\0SeChangeNotifyPrivilege\0", &Error);
    ok(Ret, "set with an empty string in it: error %lu\n", Error);
    CheckPrivileges(Service, Tcb, "SeTcbPrivilege\0", "ended by an empty string");

    /* no list at all changes nothing; an empty one removes the setting */
    SetLastError(0xdeadbeef);
    InfoW.pmszRequiredPrivileges = NULL;
    Ret = ChangeServiceConfig2W(Service, LEVEL_PRIVILEGES, &InfoW);
    ok(Ret, "set NULL: error %lu\n", GetLastError());
    CheckPrivileges(Service, Tcb, "SeTcbPrivilege\0", "after NULL");
    Ret = SetPrivileges(Service, Empty, &Error);
    ok(Ret, "set empty: error %lu\n", Error);
    CheckPrivileges(Service, NULL, NULL, "emptied");

    /* the same with the ANSI function */
    InfoA.pmszRequiredPrivileges = (PSTR)TwoA;
    SetLastError(0xdeadbeef);
    Ret = ChangeServiceConfig2A(Service, LEVEL_PRIVILEGES, &InfoA);
    ok(Ret, "set two (A): error %lu\n", GetLastError());
    CheckPrivileges(Service, Two, TwoA, "two (A)");
    InfoA.pmszRequiredPrivileges = (PSTR)"";
    SetLastError(0xdeadbeef);
    Ret = ChangeServiceConfig2A(Service, LEVEL_PRIVILEGES, &InfoA);
    ok(Ret, "set empty (A): error %lu\n", GetLastError());
    CheckPrivileges(Service, NULL, NULL, "emptied (A)");
}

/* ------------------------------------------------------------------ triggers */

typedef struct
{
    SERVICE_TRIGGER_INFO Info;
    SERVICE_TRIGGER Triggers[4];
    SERVICE_TRIGGER_SPECIFIC_DATA_ITEM Items[4][3];
} TRIGGERS;

static BOOL SetTriggers(SC_HANDLE Service, SERVICE_TRIGGER_INFO *Info, DWORD *Error)
{
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = ChangeServiceConfig2W(Service, LEVEL_TRIGGERS, Info);
    *Error = Ret ? ERROR_SUCCESS : GetLastError();
    return Ret;
}

/* The size of the triggers in the buffer the SCM returns: the structures, the data and a subtype each */
static DWORD TriggerSize(const SERVICE_TRIGGER_INFO *Info)
{
    DWORD Size = sizeof(SERVICE_TRIGGER_INFO) + Info->cTriggers * sizeof(SERVICE_TRIGGER), i, j;

    for (i = 0; i < Info->cTriggers; i++)
    {
        Size += sizeof(GUID) + Info->pTriggers[i].cDataItems * sizeof(SERVICE_TRIGGER_SPECIFIC_DATA_ITEM);
        for (j = 0; j < Info->pTriggers[i].cDataItems; j++)
            Size += Info->pTriggers[i].pDataItems[j].cbData;
    }
    return Size;
}

/* The triggers of the service are the ones in Expected; Expected->cTriggers 0 for none */
static void CheckTriggers(SC_HANDLE Service, BOOL Unicode, const SERVICE_TRIGGER_INFO *Expected, const char *What)
{
    DWORD Needed = 0xdeadbeef, Needed2 = 0xdeadbeef, Size, i, j;
    SERVICE_TRIGGER_INFO *Info;
    BYTE *Buffer;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = Unicode ? QueryServiceConfig2W(Service, LEVEL_TRIGGERS, NULL, 0, &Needed)
                  : QueryServiceConfig2A(Service, LEVEL_TRIGGERS, NULL, 0, &Needed);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "%s: Ret %d, error %lu\n", What, Ret, GetLastError());
    Size = TriggerSize(Expected);
    ok(Needed == Size, "%s: Needed %lu, expected %lu\n", What, Needed, Size);
    if (Ret || Needed != Size)
        return;

    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Buffer)
        return;

    if (Size > sizeof(SERVICE_TRIGGER_INFO))
    {
        SetLastError(0xdeadbeef);
        Ret = Unicode ? QueryServiceConfig2W(Service, LEVEL_TRIGGERS, Buffer, Size - 1, &Needed2)
                      : QueryServiceConfig2A(Service, LEVEL_TRIGGERS, Buffer, Size - 1, &Needed2);
        ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "%s, one byte short: Ret %d, error %lu\n",
           What, Ret, GetLastError());
        ok(Needed2 == Size, "%s: Needed %lu with one byte short\n", What, Needed2);
    }

    /* the size comes back only when the buffer is too small */
    Needed2 = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = Unicode ? QueryServiceConfig2W(Service, LEVEL_TRIGGERS, Buffer, Size, &Needed2)
                  : QueryServiceConfig2A(Service, LEVEL_TRIGGERS, Buffer, Size, &Needed2);
    ok(Ret, "%s: failed with the size it asked for, error %lu\n", What, GetLastError());
    ok(Needed2 == 0xdeadbeef, "%s: Needed %lu after success\n", What, Needed2);
    if (!Ret)
        goto done;

    Info = (SERVICE_TRIGGER_INFO *)Buffer;
    ok(Info->cTriggers == Expected->cTriggers, "%s: %lu triggers\n", What, Info->cTriggers);
    ok(Info->pReserved == NULL, "%s: pReserved %p\n", What, Info->pReserved);
    if (Info->cTriggers != Expected->cTriggers || Expected->cTriggers == 0)
    {
        ok(Info->pTriggers == NULL, "%s: pTriggers %p without triggers\n", What, Info->pTriggers);
        goto done;
    }
    if (!InBuffer(Info->pTriggers, Buffer, Size, Info->cTriggers * sizeof(SERVICE_TRIGGER)))
    {
        ok(0, "%s: pTriggers %p (buffer %p, %lu bytes)\n", What, Info->pTriggers, Buffer, Size);
        goto done;
    }

    for (i = 0; i < Info->cTriggers; i++)
    {
        const SERVICE_TRIGGER *Got = &Info->pTriggers[i], *Want = &Expected->pTriggers[i];

        ok(Got->dwTriggerType == Want->dwTriggerType, "%s: %lu: type %lu\n", What, i, Got->dwTriggerType);
        ok(Got->dwAction == Want->dwAction, "%s: %lu: action %lu\n", What, i, Got->dwAction);
        ok(Got->pTriggerSubtype && InBuffer(Got->pTriggerSubtype, Buffer, Size, sizeof(GUID)) &&
           IsEqualGUID(Got->pTriggerSubtype, Want->pTriggerSubtype), "%s: %lu: subtype %p\n", What, i, Got->pTriggerSubtype);
        ok(Got->cDataItems == Want->cDataItems, "%s: %lu: %lu data items\n", What, i, Got->cDataItems);
        if (Got->cDataItems != Want->cDataItems || Want->cDataItems == 0)
        {
            ok(Got->pDataItems == NULL || Want->cDataItems != 0, "%s: %lu: pDataItems %p\n", What, i, Got->pDataItems);
            continue;
        }
        if (!InBuffer(Got->pDataItems, Buffer, Size, Got->cDataItems * sizeof(SERVICE_TRIGGER_SPECIFIC_DATA_ITEM)))
        {
            ok(0, "%s: %lu: pDataItems %p\n", What, i, Got->pDataItems);
            continue;
        }
        for (j = 0; j < Got->cDataItems; j++)
        {
            const SERVICE_TRIGGER_SPECIFIC_DATA_ITEM *GotItem = &Got->pDataItems[j], *WantItem = &Want->pDataItems[j];

            ok(GotItem->dwDataType == WantItem->dwDataType, "%s: %lu/%lu: data type %lu\n", What, i, j, GotItem->dwDataType);
            ok(GotItem->cbData == WantItem->cbData, "%s: %lu/%lu: %lu bytes\n", What, i, j, GotItem->cbData);
            ok(GotItem->pData && InBuffer(GotItem->pData, Buffer, Size, GotItem->cbData) &&
               GotItem->cbData == WantItem->cbData && !memcmp(GotItem->pData, WantItem->pData, GotItem->cbData),
               "%s: %lu/%lu: data %p\n", What, i, j, GotItem->pData);
        }
    }

done:
    HeapFree(GetProcessHeap(), 0, Buffer);
}

static void CheckTriggersAll(SC_HANDLE Service, const SERVICE_TRIGGER_INFO *Expected, const char *What)
{
    CheckTriggers(Service, TRUE, Expected, What);
    CheckTriggers(Service, FALSE, Expected, What);
}

static void CheckTriggerRegistry(const SERVICE_TRIGGER_INFO *Expected)
{
    DWORD Type, Size, i, j, Value;
    WCHAR Sub[32], Name[32];
    BYTE *Data;

    for (i = 0; i < Expected->cTriggers; i++)
    {
        const SERVICE_TRIGGER *Want = &Expected->pTriggers[i];

        StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"TriggerInfo\\%lu", i);
        Data = ReadValue(SERVICE_NAME, Sub, L"Type", &Type, &Size);
        ok(Data && Type == REG_DWORD && Size == sizeof(DWORD) && *(DWORD *)Data == Want->dwTriggerType,
           "%lu: Type %p\n", i, Data);
        HeapFree(GetProcessHeap(), 0, Data);
        Data = ReadValue(SERVICE_NAME, Sub, L"Action", &Type, &Size);
        ok(Data && Type == REG_DWORD && Size == sizeof(DWORD) && *(DWORD *)Data == Want->dwAction, "%lu: Action %p\n", i, Data);
        HeapFree(GetProcessHeap(), 0, Data);
        Data = ReadValue(SERVICE_NAME, Sub, L"Guid", &Type, &Size);
        ok(Data && Type == REG_BINARY && Size == sizeof(GUID) && !memcmp(Data, Want->pTriggerSubtype, sizeof(GUID)),
           "%lu: Guid %p\n", i, Data);
        HeapFree(GetProcessHeap(), 0, Data);
        for (j = 0; j < Want->cDataItems; j++)
        {
            StringCchPrintfW(Name, ARRAYSIZE(Name), L"Data%lu", j);
            Data = ReadValue(SERVICE_NAME, Sub, Name, &Type, &Size);
            ok(Data && Type == REG_BINARY && Size == Want->pDataItems[j].cbData &&
               !memcmp(Data, Want->pDataItems[j].pData, Size), "%lu/%lu: %ls %p\n", i, j, Name, Data);
            HeapFree(GetProcessHeap(), 0, Data);
            StringCchPrintfW(Name, ARRAYSIZE(Name), L"DataType%lu", j);
            Data = ReadValue(SERVICE_NAME, Sub, Name, &Type, &Size);
            Value = Data ? *(DWORD *)Data : 0;
            ok(Data && Type == REG_DWORD && Value == Want->pDataItems[j].dwDataType, "%lu/%lu: %ls %p\n", i, j, Name, Data);
            HeapFree(GetProcessHeap(), 0, Data);
        }
    }
    StringCchPrintfW(Sub, ARRAYSIZE(Sub), L"TriggerInfo\\%lu", Expected->cTriggers);
    ok(!KeyExists(SERVICE_NAME, Sub), "there is a trigger %lu too many\n", Expected->cTriggers);
}

static void TestTriggers(SC_HANDLE Service)
{
    static const WCHAR Text[] = L"abc";
    static const WCHAR Device1[] = L"USB\\VID_1234";
    static const WCHAR Device2[] = L"second";
    static BYTE Binary[3] = { 1, 2, 3 };
    static BYTE Level = 4;
    static BYTE Keyword[8] = { 3, 0x0a, 0, 0, 0, 0, 0, 0 };
    SERVICE_TRIGGER_INFO Info;
    SERVICE_TRIGGER Triggers[3];
    SERVICE_TRIGGER_SPECIFIC_DATA_ITEM Items[4];
    SERVICE_TRIGGER_INFO NoTriggers = { 0, NULL, NULL };
    DWORD Error, Needed, i;
    BYTE Buffer[256];
    BOOL Ret;

    CheckTriggersAll(Service, &NoTriggers, "initially");
    ok(!KeyExists(SERVICE_NAME, L"TriggerInfo"), "TriggerInfo exists without triggers\n");

    /* removing what is not there is not valid */
    Ret = SetTriggers(Service, &NoTriggers, &Error);
    ok(!Ret && Error == ERROR_INVALID_PARAMETER, "delete without triggers: Ret %d, error %lu\n", Ret, Error);

    /* one trigger, no data */
    memset(Triggers, 0, sizeof(Triggers));
    Triggers[0].dwTriggerType = SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY;
    Triggers[0].dwAction = SERVICE_TRIGGER_ACTION_SERVICE_START;
    Triggers[0].pTriggerSubtype = (GUID *)&IpGuid;
    Info.cTriggers = 1;
    Info.pTriggers = Triggers;
    Info.pReserved = NULL;
    Ret = SetTriggers(Service, &Info, &Error);
    ok(Ret, "one trigger: error %lu\n", Error);
    CheckTriggersAll(Service, &Info, "one trigger");
    CheckTriggerRegistry(&Info);

    /* two, of which one has two data items, a string and bytes; the data comes last, the last item first */
    memset(Triggers, 0, sizeof(Triggers));
    Items[0].dwDataType = SERVICE_TRIGGER_DATA_TYPE_STRING;
    Items[0].cbData = sizeof(Text);
    Items[0].pData = (BYTE *)Text;
    Items[1].dwDataType = SERVICE_TRIGGER_DATA_TYPE_BINARY;
    Items[1].cbData = sizeof(Binary);
    Items[1].pData = Binary;
    Triggers[0].dwTriggerType = SERVICE_TRIGGER_TYPE_CUSTOM;
    Triggers[0].dwAction = SERVICE_TRIGGER_ACTION_SERVICE_START;
    Triggers[0].pTriggerSubtype = (GUID *)&CustomGuid;
    Triggers[0].cDataItems = 2;
    Triggers[0].pDataItems = Items;
    Triggers[1].dwTriggerType = SERVICE_TRIGGER_TYPE_GROUP_POLICY;
    Triggers[1].dwAction = SERVICE_TRIGGER_ACTION_SERVICE_STOP;
    Triggers[1].pTriggerSubtype = (GUID *)&PolicyGuid;
    Info.cTriggers = 2;
    Ret = SetTriggers(Service, &Info, &Error);
    ok(Ret, "two triggers: error %lu\n", Error);
    CheckTriggersAll(Service, &Info, "two triggers");
    CheckTriggerRegistry(&Info);

    /* a query that has room for the structures only, and what it reports */
    Needed = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = QueryServiceConfig2W(Service, LEVEL_TRIGGERS, Buffer, sizeof(SERVICE_TRIGGER_INFO), &Needed);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER && Needed == TriggerSize(&Info),
       "structures only: Ret %d, error %lu, Needed %lu\n", Ret, GetLastError(), Needed);

    /* a device trigger takes strings, the level and the keywords of an ETW provider their sizes */
    memset(Triggers, 0, sizeof(Triggers));
    Items[0].dwDataType = SERVICE_TRIGGER_DATA_TYPE_STRING;
    Items[0].cbData = sizeof(Device1);
    Items[0].pData = (BYTE *)Device1;
    Items[1].dwDataType = SERVICE_TRIGGER_DATA_TYPE_STRING;
    Items[1].cbData = sizeof(Device2);
    Items[1].pData = (BYTE *)Device2;
    Triggers[0].dwTriggerType = SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL;
    Triggers[0].dwAction = SERVICE_TRIGGER_ACTION_SERVICE_START;
    Triggers[0].pTriggerSubtype = (GUID *)&DeviceGuid;
    Triggers[0].cDataItems = 2;
    Triggers[0].pDataItems = Items;
    Info.cTriggers = 1;
    Ret = SetTriggers(Service, &Info, &Error);
    ok(Ret, "device: error %lu\n", Error);
    CheckTriggersAll(Service, &Info, "device");

    memset(Triggers, 0, sizeof(Triggers));
    Items[0].dwDataType = SERVICE_TRIGGER_DATA_TYPE_LEVEL;
    Items[0].cbData = sizeof(Level);
    Items[0].pData = &Level;
    Items[1].dwDataType = SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ANY;
    Items[1].cbData = sizeof(Keyword);
    Items[1].pData = Keyword;
    Items[2].dwDataType = SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ALL;
    Items[2].cbData = sizeof(Keyword);
    Items[2].pData = Keyword;
    Triggers[0].dwTriggerType = SERVICE_TRIGGER_TYPE_CUSTOM;
    Triggers[0].dwAction = SERVICE_TRIGGER_ACTION_SERVICE_STOP;
    Triggers[0].pTriggerSubtype = (GUID *)&CustomGuid;
    Triggers[0].cDataItems = 3;
    Triggers[0].pDataItems = Items;
    Ret = SetTriggers(Service, &Info, &Error);
    ok(Ret, "level and keywords: error %lu\n", Error);
    CheckTriggersAll(Service, &Info, "level and keywords");

    /* what is refused, and that the triggers stay as they were */
    for (i = 0; i < 11; i++)
    {
        static const char *Names[11] = { "type 0", "type 8", "action 0", "action 3", "no subtype", "level of 2 bytes",
                                         "keywords of 4 bytes", "data type 0", "data of 0 bytes", "device with bytes",
                                         "endpoint with another subtype" };
        static const DWORD Errors[11] = { ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER,
                                          ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER,
                                          ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER,
                                          ERROR_INVALID_PARAMETER, ERROR_NOT_FOUND };
        SERVICE_TRIGGER_INFO Bad;
        SERVICE_TRIGGER BadTrigger;
        SERVICE_TRIGGER_SPECIFIC_DATA_ITEM BadItem;

        BadTrigger.dwTriggerType = SERVICE_TRIGGER_TYPE_CUSTOM;
        BadTrigger.dwAction = SERVICE_TRIGGER_ACTION_SERVICE_START;
        BadTrigger.pTriggerSubtype = (GUID *)&CustomGuid;
        BadTrigger.cDataItems = 0;
        BadTrigger.pDataItems = NULL;
        BadItem.dwDataType = SERVICE_TRIGGER_DATA_TYPE_BINARY;
        BadItem.cbData = sizeof(Binary);
        BadItem.pData = Binary;
        switch (i)
        {
            case 0: BadTrigger.dwTriggerType = 0; break;
            case 1: BadTrigger.dwTriggerType = 8; break;
            case 2: BadTrigger.dwAction = 0; break;
            case 3: BadTrigger.dwAction = 3; break;
            case 4: BadTrigger.pTriggerSubtype = NULL; break;
            case 5: BadItem.dwDataType = SERVICE_TRIGGER_DATA_TYPE_LEVEL; BadItem.cbData = 2; break;
            case 6: BadItem.dwDataType = SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ANY; BadItem.cbData = 4; break;
            case 7: BadItem.dwDataType = 0; break;
            case 8: BadItem.cbData = 0; break;
            case 9: BadTrigger.dwTriggerType = SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL; break;
            case 10:
                BadTrigger.dwTriggerType = SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT;
                BadItem.dwDataType = SERVICE_TRIGGER_DATA_TYPE_STRING;
                break;
        }
        if (i >= 5)
        {
            BadTrigger.cDataItems = 1;
            BadTrigger.pDataItems = &BadItem;
        }
        Bad.cTriggers = 1;
        Bad.pTriggers = &BadTrigger;
        Bad.pReserved = NULL;
        Ret = SetTriggers(Service, &Bad, &Error);
        ok(!Ret && Error == Errors[i], "%s: Ret %d, error %lu\n", Names[i], Ret, Error);
    }
    CheckTriggersAll(Service, &Info, "after what is refused");

    /* a trigger of a group policy takes no data; of the network endpoints, an RPC interface or a pipe, and its name */
    memset(Triggers, 0, sizeof(Triggers));
    Items[0].dwDataType = SERVICE_TRIGGER_DATA_TYPE_STRING;
    Items[0].cbData = sizeof(Text);
    Items[0].pData = (BYTE *)Text;
    Triggers[0].dwTriggerType = SERVICE_TRIGGER_TYPE_GROUP_POLICY;
    Triggers[0].dwAction = SERVICE_TRIGGER_ACTION_SERVICE_START;
    Triggers[0].pTriggerSubtype = (GUID *)&PolicyGuid;
    Triggers[0].cDataItems = 1;
    Triggers[0].pDataItems = Items;
    Ret = SetTriggers(Service, &Info, &Error);
    ok(!Ret && Error == ERROR_NOT_SUPPORTED, "group policy with data: Ret %d, error %lu\n", Ret, Error);
    Triggers[0].dwTriggerType = SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT;
    Triggers[0].pTriggerSubtype = (GUID *)&PipeGuid;
    Ret = SetTriggers(Service, &Info, &Error);
    ok(Ret, "named pipe: error %lu\n", Error);
    CheckTriggersAll(Service, &Info, "named pipe");

    /* no triggers remove them, and the key */
    Ret = SetTriggers(Service, &NoTriggers, &Error);
    ok(Ret, "delete: error %lu\n", Error);
    CheckTriggersAll(Service, &NoTriggers, "deleted");
    ok(!KeyExists(SERVICE_NAME, L"TriggerInfo"), "TriggerInfo exists after the delete\n");

    /* the most there are */
    {
        SERVICE_TRIGGER Many[64];

        for (i = 0; i < 64; i++)
            Many[i] = Triggers[0];
        for (i = 0; i < 64; i++)
        {
            Many[i].dwTriggerType = SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY;
            Many[i].pTriggerSubtype = (GUID *)&IpGuid;
            Many[i].cDataItems = 0;
            Many[i].pDataItems = NULL;
        }
        Info.cTriggers = 64;
        Info.pTriggers = Many;
        Ret = SetTriggers(Service, &Info, &Error);
        ok(Ret, "64 triggers: error %lu\n", Error);
        /* 64 of them are 64 * (32 + 16) bytes, more than the 8 KB a query is bounded by, on 64-bit */
        Needed = 0xdeadbeef;
        SetLastError(0xdeadbeef);
        Ret = QueryServiceConfig2W(Service, LEVEL_TRIGGERS, NULL, 0, &Needed);
        ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER && Needed == TriggerSize(&Info),
           "64 triggers: Ret %d, error %lu, Needed %lu\n", Ret, GetLastError(), Needed);
        Info.cTriggers = 0;
        Info.pTriggers = NULL;
        Ret = SetTriggers(Service, &Info, &Error);
        ok(Ret, "delete 64: error %lu\n", Error);
    }
}

/* ------------------------------------------------------------------ lock owner */

static void TestLockOwner(void)
{
    SC_HANDLE Lock;
    SC_HANDLE Query;
    DWORD Needed = 0, NeededA = 0, User;
    QUERY_SERVICE_LOCK_STATUSW *LockW;
    QUERY_SERVICE_LOCK_STATUSA *LockA;
    WCHAR UserName[256];
    BYTE *BufferW, *BufferA;
    PCWSTR Owner, Slash;

    Query = OpenSCManagerW(NULL, NULL, SC_MANAGER_LOCK | SC_MANAGER_QUERY_LOCK_STATUS);
    ok(Query != NULL, "OpenSCManagerW failed %lu\n", GetLastError());
    if (!Query)
        return;

    Lock = (SC_HANDLE)LockServiceDatabase(Query);
    ok(Lock != NULL, "LockServiceDatabase failed %lu\n", GetLastError());
    if (!Lock)
    {
        CloseServiceHandle(Query);
        return;
    }

    SetLastError(0xdeadbeef);
    ok(!QueryServiceLockStatusW(Query, NULL, 0, &Needed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
       "QueryServiceLockStatusW: error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!QueryServiceLockStatusA(Query, NULL, 0, &NeededA) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
       "QueryServiceLockStatusA: error %lu\n", GetLastError());
    BufferW = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Needed + 64);
    BufferA = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NeededA + 64);
    if (BufferW && BufferA && Needed >= sizeof(*LockW) && NeededA >= sizeof(*LockA))
    {
        LockW = (QUERY_SERVICE_LOCK_STATUSW *)BufferW;
        LockA = (QUERY_SERVICE_LOCK_STATUSA *)BufferA;
        ok(QueryServiceLockStatusW(Query, LockW, Needed, &Needed), "QueryServiceLockStatusW failed %lu\n", GetLastError());
        ok(QueryServiceLockStatusA(Query, LockA, NeededA, &NeededA), "QueryServiceLockStatusA failed %lu\n", GetLastError());
        ok(LockW->fIsLocked == 1 && LockA->fIsLocked == 1, "locked %lu, %lu\n", LockW->fIsLocked, LockA->fIsLocked);

        /* the owner is the user that locked it: DOMAIN\name */
        Owner = LockW->lpLockOwner;
        ok(Owner && InBuffer(Owner, BufferW, Needed, sizeof(WCHAR)) && *Owner, "owner %p\n", Owner);
        ok(LockA->lpLockOwner && *LockA->lpLockOwner, "owner (A) %p\n", LockA->lpLockOwner);
        if (Owner && *Owner)
        {
            Slash = wcsrchr(Owner, L'\\');
            ok(Slash != NULL, "the owner %ls is not DOMAIN\\name\n", Owner);
            User = ARRAYSIZE(UserName);
            if (Slash && GetUserNameW(UserName, &User))
                ok(!lstrcmpiW(Slash + 1, UserName), "the owner is %ls, the user is %ls\n", Owner, UserName);
            if (LockA->lpLockOwner)
            {
                WCHAR OwnerA[256];

                MultiByteToWideChar(CP_ACP, 0, LockA->lpLockOwner, -1, OwnerA, ARRAYSIZE(OwnerA));
                ok(!lstrcmpW(OwnerA, Owner), "owner %ls (A), %ls (W)\n", OwnerA, Owner);
            }
            /* the size has room for the name and no more than its characters */
            ok(Needed >= sizeof(*LockW) + (wcslen(Owner) + 1) * sizeof(WCHAR), "Needed %lu\n", Needed);
            ok(NeededA >= sizeof(*LockA) + strlen(LockA->lpLockOwner ? LockA->lpLockOwner : "") + 1, "Needed (A) %lu\n", NeededA);
        }
    }
    HeapFree(GetProcessHeap(), 0, BufferW);
    HeapFree(GetProcessHeap(), 0, BufferA);

    ok(UnlockServiceDatabase((SC_LOCK)Lock), "UnlockServiceDatabase failed %lu\n", GetLastError());

    /* unlocked: no owner */
    Needed = 0;
    QueryServiceLockStatusW(Query, NULL, 0, &Needed);
    BufferW = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Needed + 64);
    if (BufferW && Needed >= sizeof(*LockW) && QueryServiceLockStatusW(Query, (LPQUERY_SERVICE_LOCK_STATUSW)BufferW, Needed, &Needed))
    {
        LockW = (QUERY_SERVICE_LOCK_STATUSW *)BufferW;
        ok(LockW->fIsLocked == 0, "locked after the unlock: %lu\n", LockW->fIsLocked);
        ok(LockW->lpLockOwner != NULL && *LockW->lpLockOwner == 0, "owner after the unlock %p\n", LockW->lpLockOwner);
        ok(LockW->dwLockDuration == 0, "duration %lu\n", LockW->dwLockDuration);
    }
    HeapFree(GetProcessHeap(), 0, BufferW);
    CloseServiceHandle(Query);
}

/* ------------------------------------------------------------------ dependents */

static void TestDependents(void)
{
    static const PCWSTR Names[3] = { DEP_ONE, DEP_TWO, DEP_THREE };
    SC_HANDLE Base = NULL, One = NULL, Two = NULL, Three = NULL;
    DWORD Needed = 0, Count = 0xdeadbeef, Size, Actual, i, j;
    ENUM_SERVICE_STATUSW *Entries;
    BYTE *Buffer;
    BOOL Ret;

    Base = CreateTestService(DEP_BASE, NULL);
    One = CreateTestService(DEP_ONE, DEP_BASE L"\0");
    Two = CreateTestService(DEP_TWO, DEP_BASE L"\0");
    Three = CreateTestService(DEP_THREE, DEP_ONE L"\0");
    ok(Base && One && Two && Three, "CreateService: %p %p %p %p, error %lu\n", Base, One, Two, Three, GetLastError());
    if (!(Base && One && Two && Three))
        goto done;

    /* two of the base's dependents, and the one of the first of them */
    SetLastError(0xdeadbeef);
    Ret = EnumDependentServicesW(Base, SERVICE_STATE_ALL, NULL, 0, &Needed, &Count);
    ok(!Ret && GetLastError() == ERROR_MORE_DATA, "EnumDependentServicesW: Ret %d, error %lu\n", Ret, GetLastError());

    /* what the elements take: the array of ENUM_SERVICE_STATUSW and the two names (the display name is the name) */
    Actual = 3 * sizeof(ENUM_SERVICE_STATUSW);
    for (i = 0; i < 3; i++)
        Actual += 2 * (DWORD)((wcslen(Names[i]) + 1) * sizeof(WCHAR));
    ok(Needed >= Actual, "Needed %lu, the elements take %lu\n", Needed, Actual);
    /* Windows says more than that, by an amount of its own */

    /* a buffer of the size it asks for is enough and nothing of the last string is cut */
    Size = Needed;
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + 64);
    memset(Buffer + Size, 0xaa, 64);
    Count = 0;
    SetLastError(0xdeadbeef);
    Ret = EnumDependentServicesW(Base, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSW)Buffer, Size, &Needed, &Count);
    ok(Ret, "EnumDependentServicesW with %lu bytes failed, error %lu\n", Size, GetLastError());
    ok(Count == 3, "Count %lu\n", Count);
    ok(Buffer[Size] == 0xaa, "the SCM wrote past the buffer\n");
    if (Ret && Count == 3)
    {
        Entries = (ENUM_SERVICE_STATUSW *)Buffer;
        for (i = 0; i < 3; i++)
        {
            BOOL Known = FALSE;

            ok(Entries[i].lpServiceName && InBuffer(Entries[i].lpServiceName, Buffer, Size, sizeof(WCHAR)) &&
               Entries[i].lpDisplayName && InBuffer(Entries[i].lpDisplayName, Buffer, Size, sizeof(WCHAR)),
               "%lu: pointers %p %p\n", i, Entries[i].lpServiceName, Entries[i].lpDisplayName);
            for (j = 0; j < 3; j++)
            {
                if (InBuffer(Entries[i].lpServiceName, Buffer, Size, (wcslen(Names[j]) + 1) * sizeof(WCHAR)) &&
                    !lstrcmpW(Entries[i].lpServiceName, Names[j]))
                    Known = TRUE;
            }
            ok(Known, "%lu: name %ls\n", i, Entries[i].lpServiceName);
        }
        /* the dependents of a service go before it: three before one */
        for (i = 0; i < 3; i++)
        {
            if (!lstrcmpW(Entries[i].lpServiceName, DEP_THREE))
            {
                for (j = 0; j < 3; j++)
                    if (!lstrcmpW(Entries[j].lpServiceName, DEP_ONE))
                        ok(i < j, "%ls goes after %ls\n", DEP_THREE, DEP_ONE);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, Buffer);

    /* a buffer of what they take is enough, though it is less than it asks for on Windows */
    Size = Actual;
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    Count = 0;
    SetLastError(0xdeadbeef);
    Ret = EnumDependentServicesW(Base, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSW)Buffer, Size, &Needed, &Count);
    ok(Ret && Count == 3, "EnumDependentServicesW with %lu bytes: Ret %d, error %lu, Count %lu\n", Size, Ret, GetLastError(), Count);
    ok(!Ret || Needed == 0, "Needed %lu after success\n", Needed);
    HeapFree(GetProcessHeap(), 0, Buffer);

    /* a buffer that is short is */
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Actual);
    SetLastError(0xdeadbeef);
    Ret = EnumDependentServicesW(Base, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSW)Buffer, Actual - sizeof(ENUM_SERVICE_STATUSW),
                                 &Needed, &Count);
    ok(!Ret && GetLastError() == ERROR_MORE_DATA, "a short buffer: Ret %d, error %lu\n", Ret, GetLastError());
    HeapFree(GetProcessHeap(), 0, Buffer);

done:
    if (Three) { DeleteService(Three); CloseServiceHandle(Three); }
    if (Two) { DeleteService(Two); CloseServiceHandle(Two); }
    if (One) { DeleteService(One); CloseServiceHandle(One); }
    if (Base) { DeleteService(Base); CloseServiceHandle(Base); }
}

START_TEST(ServiceTriggerPrivileges)
{
    SC_HANDLE Service;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!Manager)
    {
        skip("OpenSCManagerW failed %lu\n", GetLastError());
        return;
    }

    /* an earlier run that died */
    DeleteTestService(SERVICE_NAME);
    DeleteTestService(DEP_THREE);
    DeleteTestService(DEP_TWO);
    DeleteTestService(DEP_ONE);
    DeleteTestService(DEP_BASE);

    Service = CreateTestService(SERVICE_NAME, NULL);
    ok(Service != NULL, "CreateServiceW failed %lu\n", GetLastError());
    if (Service)
    {
        TestPrivileges(Service);
        TestTriggers(Service);
        DeleteService(Service);
        CloseServiceHandle(Service);
    }

    TestLockOwner();
    TestDependents();

    CloseServiceHandle(Manager);
}
