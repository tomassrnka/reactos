/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the callbacks of NotifyIpInterfaceChange, NotifyUnicastIpAddressChange,
 *              NotifyRouteChange2 and the waiting of CancelMibChangeNotify2
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

/* The start of the rows of the Windows SDK, under private names: the headers of this tree do not declare them. */
typedef struct { USHORT Family; NET_LUID InterfaceLuid; ULONG InterfaceIndex; } IPIF_HEAD;
typedef struct { SOCKADDR_INET Address; NET_LUID InterfaceLuid; ULONG InterfaceIndex; } UNI_HEAD;
typedef struct { NET_LUID InterfaceLuid; ULONG InterfaceIndex; } ROUTE_HEAD;

#define NOTIF_PARAMETER 0
#define NOTIF_ADD 1
#define NOTIF_DELETE 2
#define NOTIF_INITIAL 3

typedef VOID (WINAPI *CHANGE_CALLBACK)(PVOID, PVOID, ULONG);
typedef DWORD (WINAPI *FN_Notify)(ADDRESS_FAMILY, CHANGE_CALLBACK, PVOID, BOOLEAN, HANDLE *);
typedef DWORD (WINAPI *FN_Cancel)(HANDLE);

static FN_Notify pNotifyIpInterfaceChange, pNotifyUnicastIpAddressChange, pNotifyRouteChange2;
static FN_Cancel pCancelMibChangeNotify2;

#define TEST_ADDRESS "192.0.2.77"
#define TEST_MASK "255.255.255.0"

typedef struct
{
    PVOID Expected;                 /* the context the registration was made with */
    LONG Calls;
    LONG Initial;
    LONG InitialWithRow;
    LONG BadContext;
    LONG Adds, Deletes;             /* of the test address (unicast) or of the interface (route) */
    DWORD IndexOfInterest;
    ULONG TestAddress;
    int Kind;                       /* 0 interface, 1 unicast, 2 route */
    DWORD RegisterThread;
    LONG InitialOnCaller;
    HANDLE AddSeen, DeleteSeen;
    HANDLE InCallback;              /* the callback is running (blocking test) */
    DWORD SleepMs;
    volatile LONG Finished;         /* the callback returned (blocking test) */
    HANDLE *SelfHandle;             /* the callback cancels this (self-cancel test) */
    LONG SelfResult;
    LONG Cancelled;
} STATE;

static VOID WINAPI Callback(PVOID Context, PVOID Row, ULONG Type)
{
    STATE *State = Context;
    DWORD Index = 0;
    ULONG Address = 0;

    if (!State)
        return;
    InterlockedIncrement(&State->Calls);
    if (State->Expected != State)
        InterlockedIncrement(&State->BadContext);

    if (Type == NOTIF_INITIAL)
    {
        InterlockedIncrement(&State->Initial);
        if (Row)
            InterlockedIncrement(&State->InitialWithRow);
        if (GetCurrentThreadId() == State->RegisterThread)
            InterlockedIncrement(&State->InitialOnCaller);
        return;
    }
    if (!Row)
        return;

    if (State->Kind == 0)
        Index = ((IPIF_HEAD *)Row)->InterfaceIndex;
    else if (State->Kind == 1)
    {
        Index = ((UNI_HEAD *)Row)->InterfaceIndex;
        Address = ((UNI_HEAD *)Row)->Address.Ipv4.sin_addr.s_addr;
        if (Address != State->TestAddress)
            return;
    }
    else
        Index = ((ROUTE_HEAD *)Row)->InterfaceIndex;

    if (Index != State->IndexOfInterest)
        return;

    if (Type == NOTIF_ADD)
    {
        if (InterlockedIncrement(&State->Adds) == 1 && State->AddSeen)
            SetEvent(State->AddSeen);
        if (State->InCallback)
            SetEvent(State->InCallback);
        if (State->SleepMs)
        {
            Sleep(State->SleepMs);
            InterlockedExchange(&State->Finished, 1);
        }
        if (State->SelfHandle)
            InterlockedExchange(&State->SelfResult, (LONG)pCancelMibChangeNotify2(*State->SelfHandle));
    }
    else if (Type == NOTIF_DELETE)
    {
        if (InterlockedIncrement(&State->Deletes) == 1 && State->DeleteSeen)
            SetEvent(State->DeleteSeen);
    }
}

static void test_initial(const char *Name, FN_Notify pNotify, int Kind)
{
    STATE State;
    HANDLE Handle = NULL;
    ADDRESS_FAMILY Families[] = { AF_UNSPEC, AF_INET };
    DWORD Error;
    int i;

    for (i = 0; i < 2; i++)
    {
        memset(&State, 0, sizeof(State));
        State.Expected = &State;
        State.Kind = Kind;
        State.RegisterThread = GetCurrentThreadId();
        Handle = NULL;
        Error = pNotify(Families[i], Callback, &State, TRUE, &Handle);
        ok(Error == NO_ERROR, "%s(family %u, initial): %lu\n", Name, Families[i], Error);
        if (Error != NO_ERROR)
            continue;
        ok(Handle != NULL, "%s: no handle\n", Name);
        /* made before the call returned, by the calling thread, without a row; Windows 10 makes one per
           family, so two for AF_UNSPEC */
        ok(State.Initial >= 1 && State.Initial <= 2, "%s(family %u): %ld initial notifications\n",
           Name, Families[i], State.Initial);
        ok(State.Initial == State.Calls, "%s: %ld calls, %ld initial\n", Name, State.Calls, State.Initial);
        ok(State.InitialOnCaller == State.Initial, "%s: initial notification from another thread\n", Name);
        ok(State.InitialWithRow == 0, "%s: initial notification with a row\n", Name);
        ok(State.BadContext == 0, "%s: wrong context\n", Name);
        if (Families[i] == AF_INET)
            ok(State.Initial == 1, "%s(AF_INET): %ld initial notifications\n", Name, State.Initial);

        Sleep(300);
        ok(State.Calls == State.Initial, "%s: %ld calls without a change\n", Name, State.Calls);
        Error = pCancelMibChangeNotify2(Handle);
        ok(Error == NO_ERROR, "%s: cancel: %lu\n", Name, Error);
    }

    /* without the initial notification there is no call */
    memset(&State, 0, sizeof(State));
    State.Expected = &State;
    State.Kind = Kind;
    Error = pNotify(AF_INET, Callback, &State, FALSE, &Handle);
    ok(Error == NO_ERROR, "%s(no initial): %lu\n", Name, Error);
    if (Error == NO_ERROR)
    {
        ok(State.Calls == 0, "%s: %ld calls during the registration\n", Name, State.Calls);
        Sleep(300);
        ok(State.Calls == 0, "%s: %ld calls without a change\n", Name, State.Calls);
        Error = pCancelMibChangeNotify2(Handle);
        ok(Error == NO_ERROR, "%s: cancel: %lu\n", Name, Error);
    }
}

/* The address of the interface the tests use. ReactOS has one address per interface: AddIPAddress replaces it
   and DeleteIPAddress leaves the interface without one, so it is put back afterwards. */
static DWORD SavedIndex;
static ULONG SavedAddress, SavedMask;

static void restore_address(void)
{
    PMIB_IPADDRTABLE Table;
    ULONG Size = 0, Context, Instance;
    DWORD i, Error;
    BOOL Present = FALSE;

    if (!is_reactos() || !SavedAddress)
        return;
    GetIpAddrTable(NULL, &Size, FALSE);
    Table = malloc(Size);
    Error = Table ? GetIpAddrTable(Table, &Size, FALSE) : ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; Error == NO_ERROR && i < Table->dwNumEntries; i++)
        if (Table->table[i].dwAddr == SavedAddress && Table->table[i].dwIndex == SavedIndex)
            Present = TRUE;
    free(Table);
    if (!Present)
        AddIPAddress(SavedAddress, SavedMask, SavedIndex, &Context, &Instance);
    Sleep(500);
}

/* The index of an interface with an IPv4 address, and its address. */
static BOOL find_interface(DWORD *Index)
{
    PMIB_IPADDRTABLE Table;
    ULONG Size = 0;
    DWORD i, Error;
    BOOL Found = FALSE;

    GetIpAddrTable(NULL, &Size, FALSE);
    Table = malloc(Size);
    Error = Table ? GetIpAddrTable(Table, &Size, FALSE) : ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; Error == NO_ERROR && i < Table->dwNumEntries && !Found; i++)
    {
        if (Table->table[i].dwAddr && (Table->table[i].dwAddr & 0xff) != 127)
        {
            *Index = Table->table[i].dwIndex;
            SavedIndex = *Index;
            SavedAddress = Table->table[i].dwAddr;
            SavedMask = Table->table[i].dwMask;
            Found = TRUE;
        }
    }
    free(Table);
    return Found;
}

typedef struct { HANDLE Done; HANDLE *Handle; DWORD Result; DWORD Elapsed; } CANCEL_ARGS;

static DWORD WINAPI CancelThread(PVOID Param)
{
    CANCEL_ARGS *Args = Param;
    DWORD Start = GetTickCount();

    Args->Result = pCancelMibChangeNotify2(*Args->Handle);
    Args->Elapsed = GetTickCount() - Start;
    SetEvent(Args->Done);
    return 0;
}

static void test_changes(void)
{
    STATE Unicast, Interface, Route;
    HANDLE hUnicast = NULL, hInterface = NULL, hRoute = NULL;
    DWORD Index, Error, Wait;
    ULONG Context = 0, Instance = 0;
    BOOL Added = FALSE;

    if (!is_reactos())
    {
        skip("A change of the network configuration is only made on ReactOS\n");
        return;
    }

    if (!find_interface(&Index))
    {
        skip("No interface with an address\n");
        return;
    }

    memset(&Unicast, 0, sizeof(Unicast));
    memset(&Interface, 0, sizeof(Interface));
    memset(&Route, 0, sizeof(Route));
    Unicast.Expected = &Unicast; Unicast.Kind = 1; Unicast.IndexOfInterest = Index; Unicast.TestAddress = inet_addr(TEST_ADDRESS);
    Interface.Expected = &Interface; Interface.Kind = 0; Interface.IndexOfInterest = Index;
    Route.Expected = &Route; Route.Kind = 2; Route.IndexOfInterest = Index;
    Unicast.AddSeen = CreateEventW(NULL, TRUE, FALSE, NULL);
    Unicast.DeleteSeen = CreateEventW(NULL, TRUE, FALSE, NULL);
    Route.AddSeen = CreateEventW(NULL, TRUE, FALSE, NULL);
    Route.DeleteSeen = CreateEventW(NULL, TRUE, FALSE, NULL);

    Error = pNotifyUnicastIpAddressChange(AF_INET, Callback, &Unicast, FALSE, &hUnicast);
    ok(Error == NO_ERROR, "NotifyUnicastIpAddressChange: %lu\n", Error);
    Error = pNotifyIpInterfaceChange(AF_INET, Callback, &Interface, FALSE, &hInterface);
    ok(Error == NO_ERROR, "NotifyIpInterfaceChange: %lu\n", Error);
    Error = pNotifyRouteChange2(AF_INET, Callback, &Route, FALSE, &hRoute);
    ok(Error == NO_ERROR, "NotifyRouteChange2: %lu\n", Error);
    if (!hUnicast || !hInterface || !hRoute)
        goto Done;

    Error = AddIPAddress(inet_addr(TEST_ADDRESS), inet_addr(TEST_MASK), Index, &Context, &Instance);
    if (Error != NO_ERROR)
    {
        skip("AddIPAddress failed: %lu\n", Error);
        goto Done;
    }
    Added = TRUE;

    Wait = WaitForSingleObject(Unicast.AddSeen, 10000);
    ok(Wait == WAIT_OBJECT_0, "No MibAddInstance for the address\n");
    Wait = WaitForSingleObject(Route.AddSeen, 10000);
    ok(Wait == WAIT_OBJECT_0, "No MibAddInstance for a route of the interface\n");
    ok(Unicast.BadContext == 0 && Route.BadContext == 0, "Wrong context\n");

    Error = DeleteIPAddress(Context);
    ok(Error == NO_ERROR, "DeleteIPAddress: %lu\n", Error);
    Added = FALSE;
    Wait = WaitForSingleObject(Unicast.DeleteSeen, 10000);
    ok(Wait == WAIT_OBJECT_0, "No MibDeleteInstance for the address\n");
    Wait = WaitForSingleObject(Route.DeleteSeen, 10000);
    ok(Wait == WAIT_OBJECT_0, "No MibDeleteInstance for a route of the interface\n");

    /* after the cancel there is no call: another change is not reported */
    Error = pCancelMibChangeNotify2(hUnicast);
    ok(Error == NO_ERROR, "Cancel: %lu\n", Error);
    hUnicast = NULL;
    {
        LONG Calls = Unicast.Calls;

        Error = AddIPAddress(inet_addr(TEST_ADDRESS), inet_addr(TEST_MASK), Index, &Context, &Instance);
        ok(Error == NO_ERROR, "AddIPAddress: %lu\n", Error);
        Sleep(1000);
        ok(Unicast.Calls == Calls, "Call after the cancel\n");
        if (Error == NO_ERROR)
            DeleteIPAddress(Context);
        Sleep(500);
        restore_address();
    }

Done:
    if (Added)
        DeleteIPAddress(Context);
    restore_address();
    if (hUnicast)
        pCancelMibChangeNotify2(hUnicast);
    if (hInterface)
        pCancelMibChangeNotify2(hInterface);
    if (hRoute)
        pCancelMibChangeNotify2(hRoute);
    CloseHandle(Unicast.AddSeen);
    CloseHandle(Unicast.DeleteSeen);
    CloseHandle(Route.AddSeen);
    CloseHandle(Route.DeleteSeen);
}

/* CancelMibChangeNotify2 waits for a callback that is running, and can be called from another thread */
static void test_cancel_waits(void)
{
    STATE State;
    CANCEL_ARGS Args;
    HANDLE Handle = NULL, Thread;
    DWORD Index, Error, Wait;
    ULONG Context = 0, Instance = 0;

    if (!is_reactos())
    {
        skip("A change of the network configuration is only made on ReactOS\n");
        return;
    }

    if (!find_interface(&Index))
    {
        skip("No interface with an address\n");
        return;
    }

    memset(&State, 0, sizeof(State));
    State.Expected = &State;
    State.Kind = 1;
    State.IndexOfInterest = Index;
    State.TestAddress = inet_addr(TEST_ADDRESS);
    State.InCallback = CreateEventW(NULL, TRUE, FALSE, NULL);
    State.SleepMs = 2000;

    Error = pNotifyUnicastIpAddressChange(AF_INET, Callback, &State, FALSE, &Handle);
    ok(Error == NO_ERROR, "NotifyUnicastIpAddressChange: %lu\n", Error);
    if (Error != NO_ERROR)
        goto Done;
    Error = AddIPAddress(inet_addr(TEST_ADDRESS), inet_addr(TEST_MASK), Index, &Context, &Instance);
    if (Error != NO_ERROR)
    {
        skip("AddIPAddress failed: %lu\n", Error);
        pCancelMibChangeNotify2(Handle);
        goto Done;
    }

    Wait = WaitForSingleObject(State.InCallback, 10000);
    ok(Wait == WAIT_OBJECT_0, "The callback was not called\n");
    if (Wait == WAIT_OBJECT_0)
    {
        Args.Done = CreateEventW(NULL, TRUE, FALSE, NULL);
        Args.Handle = &Handle;
        Thread = CreateThread(NULL, 0, CancelThread, &Args, 0, NULL);
        Wait = WaitForSingleObject(Args.Done, 10000);
        ok(Wait == WAIT_OBJECT_0, "Cancel did not return\n");
        ok(State.Finished == 1, "Cancel returned while the callback was running\n");
        ok(Args.Result == NO_ERROR, "Cancel: %lu\n", Args.Result);
        WaitForSingleObject(Thread, 10000);
        CloseHandle(Thread);
        CloseHandle(Args.Done);
    }
    DeleteIPAddress(Context);
    Sleep(500);
    restore_address();

Done:
    CloseHandle(State.InCallback);
}

/* Windows 10 deadlocks when a callback cancels its own registration; here it must not */
static void test_self_cancel(void)
{
    STATE State;
    HANDLE Handle = NULL;
    DWORD Index, Error;
    ULONG Context = 0, Instance = 0;
    int i;

    if (!find_interface(&Index))
    {
        skip("No interface with an address\n");
        return;
    }

    memset(&State, 0, sizeof(State));
    State.Expected = &State;
    State.Kind = 1;
    State.IndexOfInterest = Index;
    State.TestAddress = inet_addr(TEST_ADDRESS);
    State.SelfHandle = &Handle;
    State.SelfResult = -1;

    Error = pNotifyUnicastIpAddressChange(AF_INET, Callback, &State, FALSE, &Handle);
    ok(Error == NO_ERROR, "NotifyUnicastIpAddressChange: %lu\n", Error);
    if (Error != NO_ERROR)
        return;
    Error = AddIPAddress(inet_addr(TEST_ADDRESS), inet_addr(TEST_MASK), Index, &Context, &Instance);
    if (Error != NO_ERROR)
    {
        skip("AddIPAddress failed: %lu\n", Error);
        pCancelMibChangeNotify2(Handle);
        return;
    }
    for (i = 0; i < 100 && State.SelfResult == -1; i++)
        Sleep(100);
    ok(State.SelfResult == NO_ERROR, "Cancel from the callback: %ld\n", State.SelfResult);
    DeleteIPAddress(Context);
    Sleep(500);
    restore_address();
    ok(State.Deletes == 0, "Call after the cancel\n");
    /* the registration is gone: the handle is not valid any more */
    Error = pCancelMibChangeNotify2(Handle);
    ok(Error != NO_ERROR, "Cancel of a cancelled registration succeeded\n");
}

/* ---- the process and the module with live registrations ---- */

static void run_child(const char *Mode)
{
    STARTUPINFOA Startup;
    PROCESS_INFORMATION Info;
    char Path[MAX_PATH], Command[MAX_PATH + 32];
    DWORD Wait, Code = 0xdead;

    GetModuleFileNameA(NULL, Path, sizeof(Path));
    sprintf(Command, "\"%s\" NotifyChanges", Path);
    SetEnvironmentVariableA("NOTIFY_CHILD", Mode);
    memset(&Startup, 0, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    if (!CreateProcessA(NULL, Command, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Info))
    {
        SetEnvironmentVariableA("NOTIFY_CHILD", NULL);
        skip("Cannot start the child process: %lu\n", GetLastError());
        return;
    }
    Wait = WaitForSingleObject(Info.hProcess, 30000);
    SetEnvironmentVariableA("NOTIFY_CHILD", NULL);
    ok(Wait == WAIT_OBJECT_0, "The child process (%s) did not exit\n", Mode);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(Info.hProcess, 1);
    else
    {
        GetExitCodeProcess(Info.hProcess, &Code);
        ok(Code == 0, "The child process (%s) ended with %lu\n", Mode, Code);
    }
    CloseHandle(Info.hThread);
    CloseHandle(Info.hProcess);
}

/* the child: registrations that are never cancelled, then the process ends */
static void child_exit(void)
{
    static STATE State[3];
    HANDLE Handle;
    DWORD Error;
    int i;

    for (i = 0; i < 3; i++)
    {
        State[i].Expected = &State[i];
        State[i].Kind = i;
        State[i].RegisterThread = GetCurrentThreadId();
    }
    Error = pNotifyIpInterfaceChange(AF_UNSPEC, Callback, &State[0], TRUE, &Handle);
    ok(Error == NO_ERROR, "NotifyIpInterfaceChange: %lu\n", Error);
    Error = pNotifyUnicastIpAddressChange(AF_INET, Callback, &State[1], FALSE, &Handle);
    ok(Error == NO_ERROR, "NotifyUnicastIpAddressChange: %lu\n", Error);
    Error = pNotifyRouteChange2(AF_INET, Callback, &State[2], TRUE, &Handle);
    ok(Error == NO_ERROR, "NotifyRouteChange2: %lu\n", Error);
    Sleep(300);
}

/* the child: the module is released by the application while a registration lives */
static void child_unload(void)
{
    static STATE State;
    HMODULE Module = LoadLibraryW(L"iphlpapi.dll");
    FN_Notify pNotify = (FN_Notify)GetProcAddress(Module, "NotifyIpInterfaceChange");
    FN_Cancel pCancel = (FN_Cancel)GetProcAddress(Module, "CancelMibChangeNotify2");
    HANDLE Handle = NULL;
    DWORD Error;

    State.Expected = &State;
    Error = pNotify(AF_INET, Callback, &State, FALSE, &Handle);
    ok(Error == NO_ERROR, "NotifyIpInterfaceChange: %lu\n", Error);
    if (Error != NO_ERROR)
        return;

    /* drop the reference of LoadLibraryW and the one of the import: only the registration holds the DLL */
    FreeLibrary(Module);
    FreeLibrary(GetModuleHandleW(L"iphlpapi.dll"));
    Sleep(500);
    ok(GetModuleHandleW(L"iphlpapi.dll") != NULL, "The DLL was unmapped under a live registration\n");

    Error = pCancel(Handle);
    ok(Error == NO_ERROR, "Cancel: %lu\n", Error);
    Sleep(1500);
}

/* the legacy NotifyRouteChange: pending until the route table changes */
static void test_legacy_route_change(void)
{
    typedef DWORD (WINAPI *FN_Legacy)(PHANDLE, LPOVERLAPPED);
    FN_Legacy pNotifyRouteChange = (FN_Legacy)GetProcAddress(GetModuleHandleW(L"iphlpapi.dll"), "NotifyRouteChange");
    OVERLAPPED Overlapped;
    HANDLE Handle = NULL;
    DWORD Error, Index, Transferred;
    ULONG Context = 0, Instance = 0;

    ok(pNotifyRouteChange != NULL, "NotifyRouteChange is not exported\n");
    if (!pNotifyRouteChange)
        return;

    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Error = pNotifyRouteChange(&Handle, &Overlapped);
    ok(Error == ERROR_IO_PENDING, "NotifyRouteChange: %lu\n", Error);
    if (Error != ERROR_IO_PENDING)
    {
        CloseHandle(Overlapped.hEvent);
        return;
    }
    Sleep(300);
    ok(WaitForSingleObject(Overlapped.hEvent, 0) == WAIT_TIMEOUT, "Signalled without a change\n");

    if (is_reactos() && find_interface(&Index))
    {
        Error = AddIPAddress(inet_addr(TEST_ADDRESS), inet_addr(TEST_MASK), Index, &Context, &Instance);
        ok(Error == NO_ERROR, "AddIPAddress: %lu\n", Error);
        ok(WaitForSingleObject(Overlapped.hEvent, 10000) == WAIT_OBJECT_0, "Not signalled after a route change\n");
        ok(GetOverlappedResult(Handle, &Overlapped, &Transferred, FALSE), "GetOverlappedResult: %lu\n", GetLastError());
        if (Error == NO_ERROR)
            DeleteIPAddress(Context);
        Sleep(500);
        restore_address();
    }
    else
    {
        CancelIPChangeNotify(&Overlapped);
        ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "The cancelled request did not complete\n");
    }
    CloseHandle(Overlapped.hEvent);
}

START_TEST(NotifyChanges)
{
    HMODULE hIphlpapi = GetModuleHandleW(L"iphlpapi.dll");
    WSADATA Data;
    char Mode[16];

    WSAStartup(MAKEWORD(2, 2), &Data);
    pNotifyIpInterfaceChange = (FN_Notify)GetProcAddress(hIphlpapi, "NotifyIpInterfaceChange");
    pNotifyUnicastIpAddressChange = (FN_Notify)GetProcAddress(hIphlpapi, "NotifyUnicastIpAddressChange");
    pNotifyRouteChange2 = (FN_Notify)GetProcAddress(hIphlpapi, "NotifyRouteChange2");
    pCancelMibChangeNotify2 = (FN_Cancel)GetProcAddress(hIphlpapi, "CancelMibChangeNotify2");
    if (!pNotifyIpInterfaceChange && is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64"))
    {
        skip("No NT 6 exports\n");
        return;
    }
    ok(pNotifyIpInterfaceChange && pNotifyUnicastIpAddressChange && pNotifyRouteChange2 && pCancelMibChangeNotify2,
       "The notification functions are not exported\n");
    if (!pNotifyIpInterfaceChange || !pNotifyUnicastIpAddressChange || !pNotifyRouteChange2 || !pCancelMibChangeNotify2)
        return;

    if (GetEnvironmentVariableA("NOTIFY_CHILD", Mode, sizeof(Mode)))
    {
        if (!strcmp(Mode, "exit"))
            child_exit();
        else if (!strcmp(Mode, "unload"))
            child_unload();
        return;
    }

    test_initial("NotifyIpInterfaceChange", pNotifyIpInterfaceChange, 0);
    test_initial("NotifyUnicastIpAddressChange", pNotifyUnicastIpAddressChange, 1);
    test_initial("NotifyRouteChange2", pNotifyRouteChange2, 2);
    test_changes();
    test_cancel_waits();
    if (is_reactos())
        test_self_cancel();
    test_legacy_route_change();
    run_child("exit");
    if (is_reactos())
        run_child("unload");
}
