/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Layout of the structures the service control manager returns with pointer members:
 *              EnumServicesStatus(Ex), EnumDependentServices, QueryServiceConfig2 and
 *              QueryServiceLockStatus, as seen by 32-bit callers on a 64-bit system
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Everything but the part that is guarded by is_reactos() only reads, so the test can be run on Windows.
 */

#include "precomp.h"

#define MAX_NAMES 2048

static BOOL InBuffer(const void *p, const void *Buffer, DWORD Size)
{
    return (const BYTE *)p >= (const BYTE *)Buffer && (const BYTE *)p < (const BYTE *)Buffer + Size;
}

static BOOL StrOk(BOOL Unicode, const void *p, SIZE_T *Len)
{
    SIZE_T i;

    for (i = 0; i < 1024; i++)
    {
        if (Unicode ? ((const WCHAR *)p)[i] == 0 : ((const CHAR *)p)[i] == 0)
        {
            *Len = i;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL StrEq(BOOL Unicode, const void *a, const void *b)
{
    return Unicode ? !lstrcmpiW(a, b) : !lstrcmpiA(a, b);
}

/* ------------------------------------------------------------------ enumerations */

typedef struct
{
    BOOL Process;
    BOOL Unicode;
} ENUM_KIND;

static DWORD EntrySize(const ENUM_KIND *Kind)
{
    return Kind->Process ? sizeof(ENUM_SERVICE_STATUS_PROCESSW) : sizeof(ENUM_SERVICE_STATUSW);
}

static BOOL CallEnum(const ENUM_KIND *Kind, SC_HANDLE Manager, DWORD State, BYTE *Buffer, DWORD Size,
                     DWORD *Needed, DWORD *Count, DWORD *Resume)
{
    if (Kind->Process)
    {
        if (Kind->Unicode)
            return EnumServicesStatusExW(Manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, State,
                                         Buffer, Size, Needed, Count, Resume, NULL);
        return EnumServicesStatusExA(Manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, State,
                                     Buffer, Size, Needed, Count, Resume, NULL);
    }
    if (Kind->Unicode)
        return EnumServicesStatusW(Manager, SERVICE_WIN32, State, (LPENUM_SERVICE_STATUSW)Buffer,
                                   Size, Needed, Count, Resume);
    return EnumServicesStatusA(Manager, SERVICE_WIN32, State, (LPENUM_SERVICE_STATUSA)Buffer,
                               Size, Needed, Count, Resume);
}

/* The name and display name members sit at the same offsets in the A and W structures */
static void GetEntry(const ENUM_KIND *Kind, const BYTE *Buffer, DWORD Index,
                     const void **Name, const void **Display, SERVICE_STATUS *Status)
{
    if (Kind->Process)
    {
        const ENUM_SERVICE_STATUS_PROCESSW *e = (const ENUM_SERVICE_STATUS_PROCESSW *)Buffer + Index;
        *Name = e->lpServiceName;
        *Display = e->lpDisplayName;
        memcpy(Status, &e->ServiceStatusProcess, sizeof(*Status));
    }
    else
    {
        const ENUM_SERVICE_STATUSW *e = (const ENUM_SERVICE_STATUSW *)Buffer + Index;
        *Name = e->lpServiceName;
        *Display = e->lpDisplayName;
        *Status = e->ServiceStatus;
    }
}

static void CheckEntries(const ENUM_KIND *Kind, const BYTE *Buffer, DWORD Size, DWORD Count)
{
    DWORD i, StringsStart = Count * EntrySize(Kind);

    for (i = 0; i < Count; i++)
    {
        const void *Name, *Display;
        SERVICE_STATUS Status;
        SIZE_T Len;

        GetEntry(Kind, Buffer, i, &Name, &Display, &Status);
        ok(Name && InBuffer(Name, Buffer, Size) && (const BYTE *)Name >= Buffer + StringsStart,
           "entry %lu: lpServiceName %p (buffer %p+%lu, strings from +%lu)\n", i, Name, Buffer, Size, StringsStart);
        ok(Display && InBuffer(Display, Buffer, Size) && (const BYTE *)Display >= Buffer + StringsStart,
           "entry %lu: lpDisplayName %p (buffer %p+%lu, strings from +%lu)\n", i, Display, Buffer, Size, StringsStart);
        if (!Name || !Display || !InBuffer(Name, Buffer, Size) || !InBuffer(Display, Buffer, Size))
            return;
        ok(StrOk(Kind->Unicode, Name, &Len) && Len > 0, "entry %lu: bad service name\n", i);
        ok(StrOk(Kind->Unicode, Display, &Len) && Len > 0, "entry %lu: bad display name\n", i);
        ok(Status.dwServiceType != 0, "entry %lu: dwServiceType 0x%lx\n", i, Status.dwServiceType);
        ok(Status.dwCurrentState >= SERVICE_STOPPED && Status.dwCurrentState <= SERVICE_PAUSED,
           "entry %lu: dwCurrentState %lu\n", i, Status.dwCurrentState);
    }
}

static const void *FindEntry(const ENUM_KIND *Kind, const BYTE *Buffer, DWORD Count, PCWSTR Name,
                             const void **Display, SERVICE_STATUS *Status)
{
    DWORD i;

    for (i = 0; i < Count; i++)
    {
        const void *EntryName;
        CHAR NameA[64];

        GetEntry(Kind, Buffer, i, &EntryName, Display, Status);
        WideCharToMultiByte(CP_ACP, 0, Name, -1, NameA, sizeof(NameA), NULL, NULL);
        if (EntryName && StrEq(Kind->Unicode, EntryName, Kind->Unicode ? (const void *)Name : (const void *)NameA))
            return EntryName;
    }
    return NULL;
}

static void TestEnum(SC_HANDLE Manager, const ENUM_KIND *Kind)
{
    const char *Label = Kind->Process ? (Kind->Unicode ? "EnumServicesStatusExW" : "EnumServicesStatusExA")
                                      : (Kind->Unicode ? "EnumServicesStatusW" : "EnumServicesStatusA");
    DWORD Needed, Count, Resume, FullCount, Size, Total, Page, Pages;
    BYTE *Full, *Buf;
    const void *Display;
    SERVICE_STATUS Status;
    BOOL Ret;
    const void *FirstNames[MAX_NAMES];
    BYTE SmallBuf[sizeof(ENUM_SERVICE_STATUS_PROCESSW)];
    DWORD i;

    /* The size query */
    Needed = Count = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = CallEnum(Kind, Manager, SERVICE_STATE_ALL, NULL, 0, &Needed, &Count, NULL);
    ok(!Ret && GetLastError() == ERROR_MORE_DATA, "%s: Ret %d error %lu\n", Label, Ret, GetLastError());
    ok(Count == 0, "%s: Count %lu\n", Label, Count);
    ok(Needed > EntrySize(Kind), "%s: Needed %lu\n", Label, Needed);

    /* A buffer of exactly that size returns everything */
    Size = Needed;
    Full = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Full)
    {
        skip("No memory\n");
        return;
    }
    Count = Resume = 0;
    SetLastError(0xdeadbeef);
    Ret = CallEnum(Kind, Manager, SERVICE_STATE_ALL, Full, Size, &Needed, &Count, &Resume);
    ok(Ret, "%s: failed with the size it asked for, error %lu\n", Label, GetLastError());
    ok(Count > 1, "%s: Count %lu\n", Label, Count);
    ok(Count <= MAX_NAMES, "%s: Count %lu\n", Label, Count);
    ok(Needed == 0, "%s: Needed %lu\n", Label, Needed);
    ok(Resume == 0, "%s: Resume %lu\n", Label, Resume);
    if (!Ret || Count < 2 || Count > MAX_NAMES)
    {
        HeapFree(GetProcessHeap(), 0, Full);
        return;
    }
    FullCount = Count;
    CheckEntries(Kind, Full, Size, Count);

    /* A buffer that cannot hold the first entry returns none of them and asks for all */
    Count = 0xdeadbeef;
    Resume = 0;
    Needed = 0;
    SetLastError(0xdeadbeef);
    Ret = CallEnum(Kind, Manager, SERVICE_STATE_ALL, SmallBuf, sizeof(SmallBuf), &Needed, &Count, &Resume);
    ok(!Ret && GetLastError() == ERROR_MORE_DATA, "%s: small buffer: Ret %d error %lu\n", Label, Ret, GetLastError());
    ok(Count == 0, "%s: small buffer: Count %lu\n", Label, Count);
    ok(Needed == Size, "%s: small buffer: Needed %lu, expected %lu\n", Label, Needed, Size);
    ok(Resume == 0, "%s: small buffer: Resume %lu\n", Label, Resume);

    /* A well-known service, compared with the single-service APIs */
    if (FindEntry(Kind, Full, FullCount, L"EventLog", &Display, &Status))
    {
        WCHAR DisplayW[256];
        DWORD Length = ARRAYSIZE(DisplayW);

        ok(GetServiceDisplayNameW(Manager, L"EventLog", DisplayW, &Length), "GetServiceDisplayNameW %lu\n", GetLastError());
        if (Kind->Unicode)
            ok(!lstrcmpW(Display, DisplayW), "display name %s, expected %s\n",
               wine_dbgstr_w(Display), wine_dbgstr_w(DisplayW));
        else
        {
            CHAR DisplayA[256];
            WideCharToMultiByte(CP_ACP, 0, DisplayW, -1, DisplayA, sizeof(DisplayA), NULL, NULL);
            ok(!lstrcmpA(Display, DisplayA), "display name %s, expected %s\n", (PCSTR)Display, DisplayA);
        }
        ok(Status.dwServiceType & SERVICE_WIN32, "EventLog: dwServiceType 0x%lx\n", Status.dwServiceType);
    }
    else
        ok(0, "%s: EventLog is missing\n", Label);

    /* Pages through a small buffer with the resume handle: the same services in the same order */
    for (i = 0; i < FullCount; i++)
    {
        SERVICE_STATUS Unused;
        const void *Unused2;
        GetEntry(Kind, Full, i, &FirstNames[i], &Unused2, &Unused);
    }
    /* about a third of the list, but at least one of the longest entries */
    Page = Size / 3 > 1024 ? Size / 3 : 1024;
    Buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Page);
    if (!Buf)
    {
        HeapFree(GetProcessHeap(), 0, Full);
        skip("No memory\n");
        return;
    }
    Total = Pages = 0;
    Resume = 0;
    for (;;)
    {
        Count = Needed = 0;
        SetLastError(0xdeadbeef);
        Ret = CallEnum(Kind, Manager, SERVICE_STATE_ALL, Buf, Page, &Needed, &Count, &Resume);
        if (!Ret)
        {
            ok(GetLastError() == ERROR_MORE_DATA, "%s: error %lu\n", Label, GetLastError());
            if (GetLastError() != ERROR_MORE_DATA)
                break;
            ok(Needed > 0, "%s: MORE_DATA with Needed %lu\n", Label, Needed);
        }
        ok(Count > 0, "%s: page %lu has no entries\n", Label, Pages);
        CheckEntries(Kind, Buf, Page, Count);
        for (i = 0; i < Count && Total + i < FullCount; i++)
        {
            const void *PageName, *Unused2;
            SERVICE_STATUS Unused;

            GetEntry(Kind, Buf, i, &PageName, &Unused2, &Unused);
            ok(StrEq(Kind->Unicode, PageName, FirstNames[Total + i]), "%s: entry %lu of page %lu differs\n", Label, i, Pages);
        }
        Total += Count;
        Pages++;
        if (Ret || Count == 0 || Pages > 1000)
            break;
    }
    ok(Total == FullCount, "%s: %lu services in %lu pages, %lu at once\n", Label, Total, Pages, FullCount);
    ok(Pages > 1 || Page >= Size, "%s: only %lu page of %lu bytes\n", Label, Pages, Page);
    HeapFree(GetProcessHeap(), 0, Buf);
    HeapFree(GetProcessHeap(), 0, Full);
}

static void TestEnumAll(void)
{
    ENUM_KIND Kinds[] = { {FALSE, TRUE}, {FALSE, FALSE}, {TRUE, TRUE}, {TRUE, FALSE} };
    SC_HANDLE Manager;
    int i;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    ok(Manager != NULL, "OpenSCManagerW failed %lu\n", GetLastError());
    if (!Manager)
        return;
    for (i = 0; i < ARRAYSIZE(Kinds); i++)
        TestEnum(Manager, &Kinds[i]);
    CloseServiceHandle(Manager);
}

/* ------------------------------------------------------------------ dependents */

/*
 * The native 64-bit SCM sizes an element of the dependents as 44 bytes and writes 48, so a buffer of
 * exactly the size it reports is 4 bytes short for a single dependent and the last string is cut. The
 * 32-bit callers get the size of their own layout, which is exact.
 */
static DWORD DependentsSlack(void)
{
    return (is_reactos() && sizeof(PVOID) == 8) ? 8 : 0;
}

static BOOL CallDeps(BOOL Unicode, SC_HANDLE Service, BYTE *Buffer, DWORD Size, DWORD *Needed, DWORD *Count)
{
    if (Unicode)
        return EnumDependentServicesW(Service, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSW)Buffer, Size, Needed, Count);
    return EnumDependentServicesA(Service, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSA)Buffer, Size, Needed, Count);
}

static BOOL TestDependentsOf(SC_HANDLE Manager, PCWSTR Parent, BOOL Unicode, DWORD *CountOut)
{
    ENUM_KIND Kind = { FALSE, Unicode };
    SC_HANDLE Service;
    DWORD Needed = 0, Count = 0, Size, Needed2 = 0, i;
    BYTE *Buffer;
    BOOL Ret;

    Service = OpenServiceW(Manager, Parent, SERVICE_ENUMERATE_DEPENDENTS);
    if (!Service)
        return FALSE;

    Needed = Count = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = CallDeps(Unicode, Service, NULL, 0, &Needed, &Count);
    if (Ret)
    {
        ok(Count == 0, "%ls: no buffer but Count %lu\n", Parent, Count);
        CloseServiceHandle(Service);
        *CountOut = 0;
        return TRUE;
    }
    ok(GetLastError() == ERROR_MORE_DATA, "%ls: error %lu\n", Parent, GetLastError());
    ok(Needed > EntrySize(&Kind), "%ls: Needed %lu\n", Parent, Needed);

    /* A buffer that holds one entry and not the rest reports the same size */
    Size = EntrySize(&Kind);
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    Needed2 = Count = 0;
    SetLastError(0xdeadbeef);
    Ret = CallDeps(Unicode, Service, Buffer, Size, &Needed2, &Count);
    ok(!Ret && GetLastError() == ERROR_MORE_DATA, "%ls: small buffer: Ret %d error %lu\n", Parent, Ret, GetLastError());
    ok(Needed2 == Needed, "%ls: Needed %lu with a small buffer, %lu with none\n", Parent, Needed2, Needed);
    HeapFree(GetProcessHeap(), 0, Buffer);

    Size = Needed + DependentsSlack();
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    Count = 0;
    SetLastError(0xdeadbeef);
    Ret = CallDeps(Unicode, Service, Buffer, Size, &Needed2, &Count);
    ok(Ret, "%ls: failed with the size it asked for, error %lu\n", Parent, GetLastError());
    if (Ret)
    {
        ok(Count > 0, "%ls: Count %lu\n", Parent, Count);
        CheckEntries(&Kind, Buffer, Size, Count);
        /* every dependent is a service that can be opened by the name it was reported with */
        for (i = 0; i < Count; i++)
        {
            const void *Name, *Display;
            SERVICE_STATUS Status;
            SC_HANDLE Dependent;

            GetEntry(&Kind, Buffer, i, &Name, &Display, &Status);
            if (!Name || !InBuffer(Name, Buffer, Size))
                break;
            Dependent = Unicode ? OpenServiceW(Manager, Name, SERVICE_QUERY_STATUS)
                                : OpenServiceA(Manager, Name, SERVICE_QUERY_STATUS);
            ok(Dependent != NULL, "%ls: dependent %lu cannot be opened, error %lu\n", Parent, i, GetLastError());
            if (Dependent)
                CloseServiceHandle(Dependent);
        }
    }
    *CountOut = Count;
    HeapFree(GetProcessHeap(), 0, Buffer);
    CloseServiceHandle(Service);
    return TRUE;
}

static void TestDependents(void)
{
    static const PCWSTR Parents[] = { L"RpcSs", L"EventLog", L"PlugPlay", L"Tcpip", L"Afd", L"NSI", L"LanmanWorkstation" };
    SC_HANDLE Manager;
    DWORD CountW, CountA;
    int i, Found = 0;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    ok(Manager != NULL, "OpenSCManagerW failed %lu\n", GetLastError());
    if (!Manager)
        return;
    for (i = 0; i < ARRAYSIZE(Parents) && Found < 2; i++)
    {
        CountW = CountA = 0xdeadbeef;
        if (!TestDependentsOf(Manager, Parents[i], TRUE, &CountW) || CountW == 0)
            continue;
        ok(TestDependentsOf(Manager, Parents[i], FALSE, &CountA), "%ls: A call\n", Parents[i]);
        ok(CountA == CountW, "%ls: %lu dependents (A), %lu (W)\n", Parents[i], CountA, CountW);
        Found++;
    }
    if (!Found)
        skip("No service with dependents found\n");
    CloseServiceHandle(Manager);
}

/* ------------------------------------------------------------------ QueryServiceConfig2 */

typedef struct
{
    DWORD Descriptions;
    DWORD Failures;
} CONFIG2_STATS;

static BYTE *QueryConfig2(BOOL Unicode, SC_HANDLE Service, DWORD Level, DWORD *Size)
{
    DWORD Needed = 0xdeadbeef;
    BYTE *Buffer;
    BOOL Ret;
    BOOL (WINAPI *Query)(SC_HANDLE, DWORD, LPBYTE, DWORD, LPDWORD) = Unicode ? QueryServiceConfig2W : QueryServiceConfig2A;

    SetLastError(0xdeadbeef);
    Ret = Query(Service, Level, NULL, 0, &Needed);
    /* Windows has no string for a description that is a resource that is not there (15100) */
    if (!Ret && GetLastError() == 15100)
        return NULL;
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "level %lu: Ret %d error %lu\n", Level, Ret, GetLastError());
    if (Ret || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return NULL;
    *Size = Needed;
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *Size);
    if (!Buffer)
        return NULL;
    /* the size it asked for is enough */
    Ret = Query(Service, Level, Buffer, *Size, &Needed);
    ok(Ret, "level %lu: failed with the size it asked for (%lu), error %lu\n", Level, *Size, GetLastError());
    if (!Ret)
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return NULL;
    }
    return Buffer;
}

static void CheckDescription(PCWSTR Name, SC_HANDLE Service, CONFIG2_STATS *Stats)
{
    DWORD SizeW, SizeA;
    BYTE *BufW, *BufA;
    SERVICE_DESCRIPTIONW *DescW;
    SERVICE_DESCRIPTIONA *DescA;
    SIZE_T Len;

    BufW = QueryConfig2(TRUE, Service, SERVICE_CONFIG_DESCRIPTION, &SizeW);
    BufA = QueryConfig2(FALSE, Service, SERVICE_CONFIG_DESCRIPTION, &SizeA);
    if (BufW && BufA)
    {
        DescW = (SERVICE_DESCRIPTIONW *)BufW;
        DescA = (SERVICE_DESCRIPTIONA *)BufA;
        ok(SizeW >= sizeof(*DescW), "%ls: size %lu\n", Name, SizeW);
        ok((DescW->lpDescription == NULL) == (DescA->lpDescription == NULL), "%ls: description W %p, A %p\n",
           Name, DescW->lpDescription, DescA->lpDescription);
        if (DescW->lpDescription)
        {
            Stats->Descriptions++;
            ok(InBuffer(DescW->lpDescription, BufW, SizeW), "%ls: lpDescription %p (buffer %p+%lu)\n",
               Name, DescW->lpDescription, BufW, SizeW);
            ok(InBuffer(DescA->lpDescription, BufA, SizeA), "%ls: lpDescription (A) %p (buffer %p+%lu)\n",
               Name, DescA->lpDescription, BufA, SizeA);
            if (InBuffer(DescW->lpDescription, BufW, SizeW))
                ok(StrOk(TRUE, DescW->lpDescription, &Len) && Len > 0, "%ls: bad description\n", Name);
            if (InBuffer(DescA->lpDescription, BufA, SizeA))
                ok(StrOk(FALSE, DescA->lpDescription, &Len) && Len > 0, "%ls: bad description (A)\n", Name);
        }
    }
    HeapFree(GetProcessHeap(), 0, BufW);
    HeapFree(GetProcessHeap(), 0, BufA);
}

static void CheckFailureActions(PCWSTR Name, SC_HANDLE Service, CONFIG2_STATS *Stats)
{
    DWORD SizeW, SizeA, i;
    BYTE *BufW, *BufA;
    SERVICE_FAILURE_ACTIONSW *FaW;
    SERVICE_FAILURE_ACTIONSA *FaA;

    BufW = QueryConfig2(TRUE, Service, SERVICE_CONFIG_FAILURE_ACTIONS, &SizeW);
    BufA = QueryConfig2(FALSE, Service, SERVICE_CONFIG_FAILURE_ACTIONS, &SizeA);
    if (BufW && BufA)
    {
        FaW = (SERVICE_FAILURE_ACTIONSW *)BufW;
        FaA = (SERVICE_FAILURE_ACTIONSA *)BufA;
        ok(SizeW >= sizeof(*FaW), "%ls: size %lu\n", Name, SizeW);
        ok(FaW->cActions == FaA->cActions, "%ls: %lu actions (W), %lu (A)\n", Name, FaW->cActions, FaA->cActions);
        ok(FaW->dwResetPeriod == FaA->dwResetPeriod, "%ls: reset period %lu (W), %lu (A)\n",
           Name, FaW->dwResetPeriod, FaA->dwResetPeriod);
        ok(FaW->cActions < 64, "%ls: %lu actions\n", Name, FaW->cActions);
        if (FaW->cActions > 0 && FaW->cActions < 64)
        {
            Stats->Failures++;
            ok(FaW->lpsaActions && InBuffer(FaW->lpsaActions, BufW, SizeW) &&
               InBuffer(FaW->lpsaActions + FaW->cActions - 1, BufW, SizeW),
               "%ls: lpsaActions %p (buffer %p+%lu)\n", Name, FaW->lpsaActions, BufW, SizeW);
            ok(FaA->lpsaActions && InBuffer(FaA->lpsaActions, BufA, SizeA) &&
               InBuffer(FaA->lpsaActions + FaA->cActions - 1, BufA, SizeA),
               "%ls: lpsaActions (A) %p (buffer %p+%lu)\n", Name, FaA->lpsaActions, BufA, SizeA);
            if (FaW->lpsaActions && InBuffer(FaW->lpsaActions + FaW->cActions - 1, BufW, SizeW) &&
                FaA->lpsaActions && InBuffer(FaA->lpsaActions + FaA->cActions - 1, BufA, SizeA))
            {
                for (i = 0; i < FaW->cActions; i++)
                {
                    ok(FaW->lpsaActions[i].Type <= 4 /* SC_ACTION_OWN_RESTART */, "%ls: action %lu type %d\n",
                       Name, i, FaW->lpsaActions[i].Type);
                    ok(FaW->lpsaActions[i].Type == FaA->lpsaActions[i].Type &&
                       FaW->lpsaActions[i].Delay == FaA->lpsaActions[i].Delay, "%ls: action %lu differs\n", Name, i);
                }
            }
        }
        ok(!FaW->lpRebootMsg || InBuffer(FaW->lpRebootMsg, BufW, SizeW), "%ls: lpRebootMsg %p\n", Name, FaW->lpRebootMsg);
        ok(!FaW->lpCommand || InBuffer(FaW->lpCommand, BufW, SizeW), "%ls: lpCommand %p\n", Name, FaW->lpCommand);
        ok(!FaA->lpRebootMsg || InBuffer(FaA->lpRebootMsg, BufA, SizeA), "%ls: lpRebootMsg (A) %p\n", Name, FaA->lpRebootMsg);
        ok(!FaA->lpCommand || InBuffer(FaA->lpCommand, BufA, SizeA), "%ls: lpCommand (A) %p\n", Name, FaA->lpCommand);
    }
    HeapFree(GetProcessHeap(), 0, BufW);
    HeapFree(GetProcessHeap(), 0, BufA);
}

static void TestConfig2Existing(void)
{
    ENUM_KIND Kind = { FALSE, TRUE };
    SC_HANDLE Manager, Service;
    BYTE *Buffer = NULL;
    DWORD Needed = 0, Count = 0, i;
    CONFIG2_STATS Stats = { 0, 0 };

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    ok(Manager != NULL, "OpenSCManagerW failed %lu\n", GetLastError());
    if (!Manager)
        return;
    EnumServicesStatusW(Manager, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &Needed, &Count, NULL);
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Needed);
    if (Buffer && EnumServicesStatusW(Manager, SERVICE_WIN32, SERVICE_STATE_ALL,
                                      (LPENUM_SERVICE_STATUSW)Buffer, Needed, &Needed, &Count, NULL))
    {
        for (i = 0; i < Count; i++)
        {
            const void *Name, *Display;
            SERVICE_STATUS Status;

            GetEntry(&Kind, Buffer, i, &Name, &Display, &Status);
            Service = OpenServiceW(Manager, Name, SERVICE_QUERY_CONFIG);
            if (!Service)
                continue;
            CheckDescription(Name, Service, &Stats);
            CheckFailureActions(Name, Service, &Stats);
            CloseServiceHandle(Service);
        }
    }
    trace("%lu services, %lu with a description, %lu with failure actions\n", Count, Stats.Descriptions, Stats.Failures);
    HeapFree(GetProcessHeap(), 0, Buffer);
    CloseServiceHandle(Manager);
}

/* ------------------------------------------------------------------ lock status */

static void CheckLockStatus(SC_HANDLE Manager, BOOL ExpectLocked)
{
    DWORD NeededW = 0xdeadbeef, NeededA = 0xdeadbeef;
    BYTE *BufW, *BufA;
    LPQUERY_SERVICE_LOCK_STATUSW LockW;
    LPQUERY_SERVICE_LOCK_STATUSA LockA;
    BOOL Ret;
    SIZE_T Len;

    SetLastError(0xdeadbeef);
    Ret = QueryServiceLockStatusW(Manager, NULL, 0, &NeededW);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "W: Ret %d error %lu\n", Ret, GetLastError());
    ok(NeededW >= sizeof(*LockW), "W: Needed %lu\n", NeededW);
    SetLastError(0xdeadbeef);
    Ret = QueryServiceLockStatusA(Manager, NULL, 0, &NeededA);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "A: Ret %d error %lu\n", Ret, GetLastError());
    ok(NeededA >= sizeof(*LockA), "A: Needed %lu\n", NeededA);
    if (NeededW < sizeof(*LockW) || NeededA < sizeof(*LockA) || NeededW > 4096 || NeededA > 4096)
        return;

    BufW = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NeededW);
    BufA = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NeededA);
    if (!BufW || !BufA)
    {
        skip("No memory\n");
        goto done;
    }
    Ret = QueryServiceLockStatusW(Manager, (LPQUERY_SERVICE_LOCK_STATUSW)BufW, NeededW, &NeededW);
    ok(Ret, "W: failed with the size it asked for (%lu), error %lu\n", NeededW, GetLastError());
    Ret = QueryServiceLockStatusA(Manager, (LPQUERY_SERVICE_LOCK_STATUSA)BufA, NeededA, &NeededA);
    ok(Ret, "A: failed with the size it asked for (%lu), error %lu\n", NeededA, GetLastError());
    if (!Ret)
        goto done;

    LockW = (LPQUERY_SERVICE_LOCK_STATUSW)BufW;
    LockA = (LPQUERY_SERVICE_LOCK_STATUSA)BufA;
    ok(LockW->fIsLocked == (DWORD)ExpectLocked, "W: fIsLocked %lu\n", LockW->fIsLocked);
    ok(LockA->fIsLocked == (DWORD)ExpectLocked, "A: fIsLocked %lu\n", LockA->fIsLocked);
    if (ExpectLocked && is_reactos() && !LockW->lpLockOwner && !LockA->lpLockOwner)
    {
        /* the SCM does not look the owner up yet */
        skip("No lock owner name\n");
        goto checked;
    }
    ok(LockW->lpLockOwner != NULL && InBuffer(LockW->lpLockOwner, BufW, NeededW + 0x1000) &&
       (BYTE *)LockW->lpLockOwner >= BufW + sizeof(*LockW), "W: lpLockOwner %p (buffer %p)\n", LockW->lpLockOwner, BufW);
    ok(LockA->lpLockOwner != NULL && InBuffer(LockA->lpLockOwner, BufA, NeededA + 0x1000) &&
       (BYTE *)LockA->lpLockOwner >= BufA + sizeof(*LockA), "A: lpLockOwner %p (buffer %p)\n", LockA->lpLockOwner, BufA);
    if (LockW->lpLockOwner && InBuffer(LockW->lpLockOwner, BufW, NeededW))
    {
        ok(StrOk(TRUE, LockW->lpLockOwner, &Len), "W: unterminated owner\n");
        ok(ExpectLocked ? Len > 0 : Len == 0, "W: owner %s\n", wine_dbgstr_w(LockW->lpLockOwner));
        if (LockA->lpLockOwner && InBuffer(LockA->lpLockOwner, BufA, NeededA))
        {
            WCHAR OwnerA[256];
            MultiByteToWideChar(CP_ACP, 0, LockA->lpLockOwner, -1, OwnerA, ARRAYSIZE(OwnerA));
            ok(!lstrcmpW(OwnerA, LockW->lpLockOwner), "owner %s (A), %s (W)\n",
               wine_dbgstr_w(OwnerA), wine_dbgstr_w(LockW->lpLockOwner));
        }
    }
checked:
    if (ExpectLocked)
        ok(LockW->dwLockDuration < 600, "dwLockDuration %lu\n", LockW->dwLockDuration);

done:
    HeapFree(GetProcessHeap(), 0, BufW);
    HeapFree(GetProcessHeap(), 0, BufA);
}

static void TestLockStatus(void)
{
    SC_HANDLE Manager, Locker;
    SC_LOCK Lock;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_QUERY_LOCK_STATUS | SC_MANAGER_CONNECT);
    ok(Manager != NULL, "OpenSCManagerW failed %lu\n", GetLastError());
    if (!Manager)
        return;
    CheckLockStatus(Manager, FALSE);

    Locker = OpenSCManagerW(NULL, NULL, SC_MANAGER_LOCK | SC_MANAGER_CONNECT);
    ok(Locker != NULL, "OpenSCManagerW(SC_MANAGER_LOCK) failed %lu\n", GetLastError());
    if (Locker)
    {
        Lock = LockServiceDatabase(Locker);
        ok(Lock != NULL, "LockServiceDatabase failed %lu\n", GetLastError());
        if (Lock)
        {
            CheckLockStatus(Manager, TRUE);
            ok(UnlockServiceDatabase(Lock), "UnlockServiceDatabase failed %lu\n", GetLastError());
        }
        CloseServiceHandle(Locker);
    }
    CheckLockStatus(Manager, FALSE);
    CloseServiceHandle(Manager);
}

/* ------------------------------------------------------------------ a service of our own (ReactOS only) */

#define OWN_SERVICE     L"advapi32_apitest_StructLayout"
#define OWN_DEPENDENT   L"advapi32_apitest_StructLayoutDep"
#define OWN_DESCRIPTION L"Description of the struct layout test service"
#define OWN_COMMAND     L"cmd.exe /c exit"
#define OWN_REBOOT_MSG  L"Rebooting for the struct layout test"

static void DeleteOwnService(SC_HANDLE Manager, PCWSTR Name)
{
    SC_HANDLE Service = OpenServiceW(Manager, Name, DELETE);

    if (Service)
    {
        DeleteService(Service);
        CloseServiceHandle(Service);
    }
}

static void TestOwnService(void)
{
    SC_HANDLE Manager, Service = NULL, Dependent = NULL;
    SERVICE_DESCRIPTIONW Description = { (LPWSTR)OWN_DESCRIPTION };
    SC_ACTION Actions[3] = { { SC_ACTION_RESTART, 1000 }, { SC_ACTION_RUN_COMMAND, 2000 }, { SC_ACTION_NONE, 3000 } };
    SERVICE_FAILURE_ACTIONSW Failure = { 86400, (LPWSTR)OWN_REBOOT_MSG, (LPWSTR)OWN_COMMAND, 3, Actions };
    BYTE *Buffer;
    DWORD Size, Count, Needed, i;
    BOOL Found = FALSE;

    if (!is_reactos())
    {
        skip("Does not create services outside ReactOS\n");
        return;
    }
    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    ok(Manager != NULL, "OpenSCManagerW failed %lu\n", GetLastError());
    if (!Manager)
        return;
    DeleteOwnService(Manager, OWN_DEPENDENT);
    DeleteOwnService(Manager, OWN_SERVICE);

    Service = CreateServiceW(Manager, OWN_SERVICE, L"Struct layout test service", SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                             L"C:\\ReactOS\\system32\\cmd.exe", NULL, NULL, NULL, NULL, NULL);
    ok(Service != NULL, "CreateServiceW failed %lu\n", GetLastError());
    if (!Service)
        goto done;
    ok(ChangeServiceConfig2W(Service, SERVICE_CONFIG_DESCRIPTION, &Description), "description: %lu\n", GetLastError());
    ok(ChangeServiceConfig2W(Service, SERVICE_CONFIG_FAILURE_ACTIONS, &Failure), "failure actions: %lu\n", GetLastError());

    /* the description and the failure actions, W and A */
    Buffer = QueryConfig2(TRUE, Service, SERVICE_CONFIG_DESCRIPTION, &Size);
    if (Buffer)
    {
        SERVICE_DESCRIPTIONW *Desc = (SERVICE_DESCRIPTIONW *)Buffer;
        ok(Size == sizeof(*Desc) + sizeof(OWN_DESCRIPTION), "Needed %lu, expected %lu\n", Size,
           (DWORD)(sizeof(*Desc) + sizeof(OWN_DESCRIPTION)));
        ok(Desc->lpDescription && InBuffer(Desc->lpDescription, Buffer, Size) && !lstrcmpW(Desc->lpDescription, OWN_DESCRIPTION),
           "lpDescription %p\n", Desc->lpDescription);
        HeapFree(GetProcessHeap(), 0, Buffer);
    }
    Buffer = QueryConfig2(FALSE, Service, SERVICE_CONFIG_DESCRIPTION, &Size);
    if (Buffer)
    {
        SERVICE_DESCRIPTIONA *Desc = (SERVICE_DESCRIPTIONA *)Buffer;
        ok(Desc->lpDescription && InBuffer(Desc->lpDescription, Buffer, Size) &&
           !lstrcmpA(Desc->lpDescription, "Description of the struct layout test service"),
           "lpDescription (A) %p\n", Desc->lpDescription);
        HeapFree(GetProcessHeap(), 0, Buffer);
    }
    Buffer = QueryConfig2(TRUE, Service, SERVICE_CONFIG_FAILURE_ACTIONS, &Size);
    if (Buffer)
    {
        SERVICE_FAILURE_ACTIONSW *Fa = (SERVICE_FAILURE_ACTIONSW *)Buffer;
        ok(Size == sizeof(*Fa) + sizeof(Actions) + sizeof(OWN_COMMAND) + sizeof(OWN_REBOOT_MSG), "Needed %lu\n", Size);
        ok(Fa->dwResetPeriod == 86400, "dwResetPeriod %lu\n", Fa->dwResetPeriod);
        ok(Fa->cActions == 3, "cActions %lu\n", Fa->cActions);
        ok(Fa->lpRebootMsg && InBuffer(Fa->lpRebootMsg, Buffer, Size) && !lstrcmpW(Fa->lpRebootMsg, OWN_REBOOT_MSG),
           "lpRebootMsg %p\n", Fa->lpRebootMsg);
        ok(Fa->lpCommand && InBuffer(Fa->lpCommand, Buffer, Size) && !lstrcmpW(Fa->lpCommand, OWN_COMMAND),
           "lpCommand %p\n", Fa->lpCommand);
        ok(Fa->lpsaActions && InBuffer(Fa->lpsaActions, Buffer, Size), "lpsaActions %p\n", Fa->lpsaActions);
        if (Fa->lpsaActions && Fa->cActions == 3 && InBuffer(Fa->lpsaActions + 2, Buffer, Size))
        {
            for (i = 0; i < 3; i++)
                ok(Fa->lpsaActions[i].Type == Actions[i].Type && Fa->lpsaActions[i].Delay == Actions[i].Delay,
                   "action %lu: type %d delay %lu\n", i, Fa->lpsaActions[i].Type, Fa->lpsaActions[i].Delay);
        }
        HeapFree(GetProcessHeap(), 0, Buffer);
    }
    Buffer = QueryConfig2(FALSE, Service, SERVICE_CONFIG_FAILURE_ACTIONS, &Size);
    if (Buffer)
    {
        SERVICE_FAILURE_ACTIONSA *Fa = (SERVICE_FAILURE_ACTIONSA *)Buffer;
        ok(Fa->cActions == 3, "cActions (A) %lu\n", Fa->cActions);
        ok(Fa->lpRebootMsg && InBuffer(Fa->lpRebootMsg, Buffer, Size) &&
           !lstrcmpA(Fa->lpRebootMsg, "Rebooting for the struct layout test"), "lpRebootMsg (A) %p\n", Fa->lpRebootMsg);
        ok(Fa->lpCommand && InBuffer(Fa->lpCommand, Buffer, Size) && !lstrcmpA(Fa->lpCommand, "cmd.exe /c exit"),
           "lpCommand (A) %p\n", Fa->lpCommand);
        ok(Fa->lpsaActions && Fa->cActions == 3 && InBuffer(Fa->lpsaActions + 2, Buffer, Size) &&
           Fa->lpsaActions[1].Type == SC_ACTION_RUN_COMMAND && Fa->lpsaActions[2].Delay == 3000,
           "lpsaActions (A) %p\n", Fa->lpsaActions);
        HeapFree(GetProcessHeap(), 0, Buffer);
    }

    /* a service that depends on it is the one and only dependent */
    Dependent = CreateServiceW(Manager, OWN_DEPENDENT, L"Struct layout test dependent", SERVICE_ALL_ACCESS,
                               SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                               L"C:\\ReactOS\\system32\\cmd.exe", NULL, NULL, OWN_SERVICE L"\0", NULL, NULL);
    ok(Dependent != NULL, "CreateServiceW(dependent) failed %lu\n", GetLastError());
    if (Dependent)
    {
        for (i = 0; i < 2; i++)
        {
            Needed = Count = 0;
            SetLastError(0xdeadbeef);
            ok(!(i ? EnumDependentServicesA(Service, SERVICE_STATE_ALL, NULL, 0, &Needed, &Count)
                   : EnumDependentServicesW(Service, SERVICE_STATE_ALL, NULL, 0, &Needed, &Count)) &&
               GetLastError() == ERROR_MORE_DATA, "%lu: error %lu\n", i, GetLastError());
            /* the status, two pointers and two strings, which are counted in WCHARs for both A and W */
            ok(Needed == sizeof(SERVICE_STATUS) + 2 * sizeof(PVOID) + sizeof(OWN_DEPENDENT) +
                         sizeof(L"Struct layout test dependent"),
               "%lu: Needed %lu\n", i, Needed);
            Size = Needed + DependentsSlack();
            Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
            if (Buffer && (i ? EnumDependentServicesA(Service, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSA)Buffer, Size, &Needed, &Count)
                             : EnumDependentServicesW(Service, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSW)Buffer, Size, &Needed, &Count)))
            {
                ENUM_KIND Kind = { FALSE, i == 0 };
                const void *Name, *Display;
                SERVICE_STATUS Status;

                ok(Count == 1, "%lu: Count %lu\n", i, Count);
                CheckEntries(&Kind, Buffer, Size, Count);
                GetEntry(&Kind, Buffer, 0, &Name, &Display, &Status);
                ok(i ? !lstrcmpA(Name, "advapi32_apitest_StructLayoutDep") : !lstrcmpW(Name, OWN_DEPENDENT),
                   "%lu: name %p\n", i, Name);
                ok(i ? !lstrcmpA(Display, "Struct layout test dependent") : !lstrcmpW(Display, L"Struct layout test dependent"),
                   "%lu: display name %p\n", i, Display);
                ok(Status.dwCurrentState == SERVICE_STOPPED, "%lu: state %lu\n", i, Status.dwCurrentState);
                Found = TRUE;
            }
            HeapFree(GetProcessHeap(), 0, Buffer);
        }
        ok(Found, "no dependent returned\n");
    }

done:
    if (Dependent)
    {
        DeleteService(Dependent);
        CloseServiceHandle(Dependent);
    }
    if (Service)
    {
        DeleteService(Service);
        CloseServiceHandle(Service);
    }
    CloseServiceHandle(Manager);
}

START_TEST(ServiceStructLayout)
{
    TestEnumAll();
    TestDependents();
    TestConfig2Existing();
    TestLockStatus();
    TestOwnService();
}
