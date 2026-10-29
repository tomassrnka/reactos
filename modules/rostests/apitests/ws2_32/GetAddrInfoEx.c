/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for ws2_32!GetAddrInfoExW, GetAddrInfoExCancel, GetAddrInfoExOverlappedResult and FreeAddrInfoExW
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Expected values were measured on Windows 10 22H2. The asynchronous cases
 * matter because .NET calls GetAddrInfoExW with a completion routine from
 * thread-pool threads that never wait alertably: Windows runs the routine on
 * a system worker thread, so it must not depend on an APC to the caller.
 * Set WS2_APITEST_NET=1 to add a case that resolves a real name.
 */

/* The Vista+ address info types are hidden from the 0x502 headers this module is built with */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#undef WINVER
#define WINVER 0x0600

#include "ws2_32.h"

#include <ws2tcpip.h>

/* Bound for anything that must complete "promptly" */
#define WAIT_MS 10000

typedef INT (WSAAPI *PFN_GetAddrInfoExW)(PCWSTR, PCWSTR, DWORD, LPGUID, const ADDRINFOEXW *, PADDRINFOEXW *,
                                         struct timeval *, LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
typedef INT (WSAAPI *PFN_GetAddrInfoExCancel)(LPHANDLE);
typedef INT (WSAAPI *PFN_GetAddrInfoExOverlappedResult)(LPOVERLAPPED);
typedef VOID (WSAAPI *PFN_FreeAddrInfoExW)(PADDRINFOEXW);

static PFN_GetAddrInfoExW pGetAddrInfoExW;
static PFN_GetAddrInfoExCancel pGetAddrInfoExCancel;
static PFN_GetAddrInfoExOverlappedResult pGetAddrInfoExOverlappedResult;
static PFN_FreeAddrInfoExW pFreeAddrInfoExW;

/* State recorded by the completion routine */
static HANDLE g_Done;
static volatile LONG g_Called;
static volatile DWORD g_RoutineThread;
static volatile DWORD g_RoutineError;
static volatile DWORD g_RoutineBytes;
static LPWSAOVERLAPPED volatile g_RoutineOverlapped;
static volatile LONG g_RoutineSawResult;
static PADDRINFOEXW *volatile g_ResultPtr;

static VOID CALLBACK CompletionRoutine(DWORD Error, DWORD Bytes, LPWSAOVERLAPPED Overlapped)
{
    g_RoutineThread = GetCurrentThreadId();
    g_RoutineError = Error;
    g_RoutineBytes = Bytes;
    g_RoutineOverlapped = Overlapped;
    g_RoutineSawResult = (g_ResultPtr && *g_ResultPtr != NULL);
    InterlockedIncrement(&g_Called);
    SetEvent(g_Done);
}

static void ResetRoutineState(PADDRINFOEXW *ResultPtr)
{
    g_Called = 0;
    g_RoutineThread = 0;
    g_RoutineError = 0xdeadbeef;
    g_RoutineBytes = 0xdeadbeef;
    g_RoutineOverlapped = NULL;
    g_RoutineSawResult = -1;
    g_ResultPtr = ResultPtr;
    ResetEvent(g_Done);
}

static void CheckLoopback(PADDRINFOEXW Info, int Port, int SockType, int Line)
{
    SOCKADDR_IN *In;

    ok(Info != NULL, "line %d: no result\n", Line);
    if (!Info)
        return;
    ok(Info->ai_family == AF_INET, "line %d: family %d\n", Line, Info->ai_family);
    ok(Info->ai_socktype == SockType, "line %d: socktype %d\n", Line, Info->ai_socktype);
    ok((int)Info->ai_addrlen == (int)sizeof(SOCKADDR_IN), "line %d: addrlen %d\n", Line, (int)Info->ai_addrlen);
    ok(Info->ai_next == NULL, "line %d: unexpected second entry\n", Line);
    ok(Info->ai_blob == NULL && Info->ai_bloblen == 0, "line %d: blob %p/%d\n", Line, Info->ai_blob, (int)Info->ai_bloblen);
    if (Info->ai_addr && Info->ai_addrlen >= sizeof(SOCKADDR_IN))
    {
        In = (SOCKADDR_IN *)Info->ai_addr;
        ok(In->sin_family == AF_INET, "line %d: sin_family %d\n", Line, In->sin_family);
        ok(ntohs(In->sin_port) == Port, "line %d: port %d\n", Line, ntohs(In->sin_port));
        ok(In->sin_addr.s_addr == inet_addr("127.0.0.1"), "line %d: address %08lx\n", Line, In->sin_addr.s_addr);
    }
    else
    {
        ok(0, "line %d: no address\n", Line);
    }
}

typedef struct _CALLER
{
    PCWSTR Name;
    BOOL Alertable;
    DWORD Tid;
    INT Ret;
    HANDLE Handle;
    OVERLAPPED Overlapped;
    PADDRINFOEXW Result;
    DWORD ResultAtReturnWasNull;
    DWORD WaitResult;
    ADDRINFOEXW Hints;
} CALLER;

/* Issue the call from a thread that then waits for the routine, alertably or not */
static DWORD WINAPI CallerThread(LPVOID Param)
{
    CALLER *C = Param;

    C->Tid = GetCurrentThreadId();
    memset(&C->Overlapped, 0, sizeof(C->Overlapped));
    C->Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    C->Handle = (HANDLE)(ULONG_PTR)0x77;
    ResetRoutineState(&C->Result);

    C->Ret = pGetAddrInfoExW(C->Name, NULL, NS_DNS, NULL, &C->Hints, &C->Result, NULL,
                             &C->Overlapped, CompletionRoutine, &C->Handle);
    C->ResultAtReturnWasNull = (C->Ret == WSA_IO_PENDING) ? (C->Result == NULL) : 2;

    if (C->Alertable)
    {
        /* An APC-based implementation would run the routine here, on this thread */
        DWORD Start = GetTickCount();

        while (GetTickCount() - Start < WAIT_MS && WaitForSingleObject(g_Done, 0) != WAIT_OBJECT_0)
            SleepEx(50, TRUE);
        C->WaitResult = WaitForSingleObject(g_Done, 0);
    }
    else
    {
        /* Never alertable, like a thread-pool thread */
        C->WaitResult = WaitForSingleObject(g_Done, WAIT_MS);
    }
    return 0;
}

static void TestRoutineFromThread(PCWSTR Name, BOOL Alertable, BOOL NetworkCase)
{
    CALLER C;
    HANDLE Thread;
    DWORD Wait;
    PADDRINFOEXW Result;

    memset(&C, 0, sizeof(C));
    C.Name = Name;
    C.Alertable = Alertable;
    C.Hints.ai_family = AF_INET;
    C.Hints.ai_socktype = SOCK_STREAM;

    Thread = CreateThread(NULL, 0, CallerThread, &C, 0, NULL);
    ok(Thread != NULL, "CreateThread failed\n");
    if (!Thread)
        return;
    Wait = WaitForSingleObject(Thread, NetworkCase ? 3 * WAIT_MS : 2 * WAIT_MS + 5000);
    ok(Wait == WAIT_OBJECT_0, "caller thread did not finish\n");
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateThread(Thread, 0);
        CloseHandle(Thread);
        return;
    }
    CloseHandle(Thread);

    ok(C.Ret == WSA_IO_PENDING, "%ls: GetAddrInfoExW returned %d\n", Name, C.Ret);
    ok(C.ResultAtReturnWasNull == 1, "%ls: *ppResult was not NULL at return (%lu)\n", Name, C.ResultAtReturnWasNull);
    ok(C.Handle != NULL && C.Handle != (HANDLE)(ULONG_PTR)0x77, "%ls: no cancel handle (%p)\n", Name, C.Handle);
    ok(C.Overlapped.Pointer == &C.Result, "%ls: Overlapped.Pointer = %p\n", Name, C.Overlapped.Pointer);

    ok(C.WaitResult == WAIT_OBJECT_0,
       "%ls: completion routine did not run within %d ms on a %s thread (wait %lu)\n",
       Name, NetworkCase ? 3 * WAIT_MS : WAIT_MS, Alertable ? "alertable" : "non-alertable", C.WaitResult);
    if (C.WaitResult != WAIT_OBJECT_0)
        return;

    ok(g_Called == 1, "%ls: routine ran %ld times\n", Name, g_Called);
    ok(g_RoutineThread != C.Tid, "%ls: routine ran on the calling thread (APC delivery)\n", Name);
    ok(g_RoutineOverlapped == (LPWSAOVERLAPPED)&C.Overlapped, "%ls: routine overlapped %p\n", Name, g_RoutineOverlapped);
    ok(g_RoutineBytes == 0, "%ls: routine bytes %lu\n", Name, g_RoutineBytes);
    ok(C.Overlapped.Internal == g_RoutineError, "%ls: Internal %#lx, routine error %lu\n",
       Name, (ULONG)C.Overlapped.Internal, g_RoutineError);

    Result = C.Result;
    if (NetworkCase)
    {
        /* Whatever the network says, the routine ran; a success must carry a valid chain */
        ok(g_RoutineError == 0 || Result == NULL, "%ls: error %lu with result %p\n", Name, g_RoutineError, Result);
        ok(g_RoutineError != 0 || g_RoutineSawResult == 1, "%ls: result not set when routine ran\n", Name);
        if (g_RoutineError == 0 && Result)
        {
            ok(Result->ai_family == AF_INET, "%ls: family %d\n", Name, Result->ai_family);
            ok(Result->ai_addrlen == sizeof(SOCKADDR_IN), "%ls: addrlen %d\n", Name, (int)Result->ai_addrlen);
        }
    }
    else
    {
        ok(g_RoutineError == 0, "%ls: routine error %lu\n", Name, g_RoutineError);
        ok(g_RoutineSawResult == 1, "%ls: *ppResult was not set before the routine ran\n", Name);
        CheckLoopback(Result, 0, SOCK_STREAM, __LINE__);
    }
    if (Result)
        pFreeAddrInfoExW(Result);
}

static void TestEventOnly(void)
{
    OVERLAPPED Overlapped;
    ADDRINFOEXW Hints;
    PADDRINFOEXW Result;
    HANDLE Handle, Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD Wait;
    INT Error;

    memset(&Hints, 0, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;

    memset(&Overlapped, 0xcc, sizeof(Overlapped));
    Overlapped.hEvent = Event;
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Handle = (HANDLE)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", L"8080", NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, NULL, &Handle);
    ok(Error == WSA_IO_PENDING, "GetAddrInfoExW returned %d\n", Error);
    ok(WSAGetLastError() == WSA_IO_PENDING, "last error %d\n", WSAGetLastError());
    ok(Result == NULL, "Result = %p, expected NULL while pending\n", Result);
    ok(Handle != NULL && Handle != (HANDLE)(ULONG_PTR)0x1234, "handle %p\n", Handle);
    ok(Overlapped.hEvent == Event, "hEvent changed to %p\n", Overlapped.hEvent);
    if (Error != WSA_IO_PENDING)
    {
        CloseHandle(Event);
        return;
    }
    Wait = WaitForSingleObject(Event, WAIT_MS);
    ok(Wait == WAIT_OBJECT_0, "wait returned %lu\n", Wait);
    ok(Overlapped.Internal == 0, "Internal %#lx\n", (ULONG)Overlapped.Internal);
    ok(Overlapped.Pointer == &Result, "Pointer = %p, expected %p\n", Overlapped.Pointer, &Result);
    if (pGetAddrInfoExOverlappedResult)
    {
        Error = pGetAddrInfoExOverlappedResult(&Overlapped);
        ok(Error == 0, "GetAddrInfoExOverlappedResult returned %d\n", Error);
    }
    if (Wait == WAIT_OBJECT_0)
    {
        CheckLoopback(Result, 8080, SOCK_STREAM, __LINE__);
        if (Result)
        {
            ok(HeapValidate(GetProcessHeap(), 0, Result), "result is not a valid process heap block\n");
            pFreeAddrInfoExW(Result);
        }
    }

    /* A name that does not exist: pending, then the error, and no result */
    ResetEvent(Event);
    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = Event;
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"ws2-apitest-nonexistent.invalid", NULL, NS_DNS, NULL, &Hints, &Result, NULL,
                            &Overlapped, NULL, &Handle);
    ok(Error == WSA_IO_PENDING, "GetAddrInfoExW(nonexistent) returned %d\n", Error);
    if (Error == WSA_IO_PENDING)
    {
        if (pGetAddrInfoExOverlappedResult)
        {
            Error = pGetAddrInfoExOverlappedResult(&Overlapped);
            ok(Error != 0, "result while pending/failed: %d\n", Error);
        }
        Wait = WaitForSingleObject(Event, 3 * WAIT_MS);
        ok(Wait == WAIT_OBJECT_0, "wait for nonexistent returned %lu\n", Wait);
        ok(Overlapped.Internal != 0, "nonexistent name resolved (%#lx)\n", (ULONG)Overlapped.Internal);
        ok(Result == NULL, "Result = %p for a failed lookup\n", Result);
        if (Result && Result != (PADDRINFOEXW)(ULONG_PTR)0x1234)
            pFreeAddrInfoExW(Result);
    }
    CloseHandle(Event);
}

/* Name literals and failed validation complete inline: real result, nothing signalled, no handle */
static void TestInline(void)
{
    OVERLAPPED Overlapped;
    ADDRINFOEXW Hints;
    PADDRINFOEXW Result;
    HANDLE Handle;
    INT Error;

    memset(&Hints, 0, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;

    memset(&Overlapped, 0, sizeof(Overlapped));
    ResetRoutineState(&Result);
    Result = NULL;
    Handle = (HANDLE)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, CompletionRoutine, &Handle);
    ok(Error == 0, "numeric host with routine returned %d\n", Error);
    ok(Handle == NULL, "numeric host: handle %p\n", Handle);
    CheckLoopback(Result, 80, SOCK_STREAM, __LINE__);
    if (Result)
        pFreeAddrInfoExW(Result);
    ok(WaitForSingleObject(g_Done, 500) == WAIT_TIMEOUT && g_Called == 0,
       "completion routine ran for an inline completion\n");

    /* NULL name and NULL service */
    ResetRoutineState(&Result);
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(NULL, NULL, NS_DNS, NULL, NULL, &Result, NULL, &Overlapped, CompletionRoutine, NULL);
    ok(Error == WSAHOST_NOT_FOUND, "NULL name returned %d\n", Error);
    ok(Result == NULL, "NULL name: Result = %p\n", Result);
    ok(WaitForSingleObject(g_Done, 300) == WAIT_TIMEOUT && g_Called == 0, "routine ran for NULL name\n");

    /* Unknown address family fails inline */
    Hints.ai_family = 1023;
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, CompletionRoutine, NULL);
    ok(Error == WSAEAFNOSUPPORT, "bad family returned %d\n", Error);
    ok(Result == NULL, "bad family: Result = %p\n", Result);
}

static void TestCancel(void)
{
    ADDRINFOEXW Hints;
    int i, Cancelled = 0, Raced = 0;
    DWORD Wait;
    INT Error;
    HANDLE Handle;
    OVERLAPPED Overlapped;
    PADDRINFOEXW Result;
    HANDLE Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    WCHAR Name[64];

    memset(&Hints, 0, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;

    /* Event mode: cancel straight after the call; Windows always wins this race */
    for (i = 0; i < 20; i++)
    {
        _snwprintf(Name, ARRAYSIZE(Name), L"ws2-apitest-cancel-%lu-%d.invalid", GetTickCount(), i);
        ResetEvent(Event);
        memset(&Overlapped, 0, sizeof(Overlapped));
        Overlapped.hEvent = Event;
        Result = NULL;
        Handle = NULL;
        Error = pGetAddrInfoExW(Name, NULL, NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, NULL, &Handle);
        ok(Error == WSA_IO_PENDING, "GetAddrInfoExW returned %d\n", Error);
        if (Error != WSA_IO_PENDING)
            break;
        Error = pGetAddrInfoExCancel(&Handle);
        Wait = WaitForSingleObject(Event, 3 * WAIT_MS);
        ok(Wait == WAIT_OBJECT_0, "event not signalled after cancel (%lu)\n", Wait);
        if (Error == 0)
        {
            Cancelled++;
            ok(Overlapped.Internal == WSA_E_CANCELLED, "cancelled: Internal %#lx\n", (ULONG)Overlapped.Internal);
            ok(Result == NULL, "cancelled: Result = %p\n", Result);
            if (pGetAddrInfoExOverlappedResult)
                ok(pGetAddrInfoExOverlappedResult(&Overlapped) == WSA_E_CANCELLED, "overlapped result %d\n",
                   pGetAddrInfoExOverlappedResult(&Overlapped));
        }
        else
        {
            /* The lookup finished first */
            Raced++;
            ok(Error == WSA_INVALID_HANDLE, "late cancel returned %d\n", Error);
            ok(Overlapped.Internal != WSA_E_CANCELLED, "completed lookup reports cancelled\n");
        }
        if (Result)
            pFreeAddrInfoExW(Result);
    }
    ok(Cancelled >= 1, "no lookup was ever cancelled (%d raced)\n", Raced);

    /* Cancelling twice: the second has nothing to cancel */
    _snwprintf(Name, ARRAYSIZE(Name), L"ws2-apitest-cancel2-%lu.invalid", GetTickCount());
    ResetEvent(Event);
    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = Event;
    Result = NULL;
    Error = pGetAddrInfoExW(Name, NULL, NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, NULL, &Handle);
    if (Error == WSA_IO_PENDING)
    {
        HANDLE Copy = Handle;
        pGetAddrInfoExCancel(&Handle);
        Error = pGetAddrInfoExCancel(&Copy);
        ok(Error == WSA_INVALID_HANDLE, "second cancel returned %d\n", Error);
        WaitForSingleObject(Event, 3 * WAIT_MS);
        if (Result)
            pFreeAddrInfoExW(Result);
    }

    /* Completion routine mode: the cancelled routine runs once, with WSA_E_CANCELLED */
    for (i = 0; i < 20; i++)
    {
        _snwprintf(Name, ARRAYSIZE(Name), L"ws2-apitest-cancelrt-%lu-%d.invalid", GetTickCount(), i);
        memset(&Overlapped, 0, sizeof(Overlapped));
        Result = NULL;
        Handle = NULL;
        ResetRoutineState(&Result);
        Error = pGetAddrInfoExW(Name, NULL, NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, CompletionRoutine, &Handle);
        ok(Error == WSA_IO_PENDING, "GetAddrInfoExW returned %d\n", Error);
        if (Error != WSA_IO_PENDING)
            break;
        Error = pGetAddrInfoExCancel(&Handle);
        if (Error == 0)
        {
            /* Windows runs the routine of a cancelled lookup inside the cancel call, on its thread */
            ok(g_Called == 1, "routine ran %ld times by the time cancel returned\n", g_Called);
            ok(g_RoutineThread == GetCurrentThreadId(), "cancelled routine ran on thread %lu, not the canceller\n", g_RoutineThread);
            ok(g_RoutineError == WSA_E_CANCELLED, "cancelled routine error %lu\n", g_RoutineError);
            Sleep(100);
            ok(g_Called == 1, "cancelled routine ran %ld times\n", g_Called);
            ok(Result == NULL, "cancelled: Result = %p\n", Result);
            break;
        }
        /* The lookup finished first: its routine still has to run once */
        Wait = WaitForSingleObject(g_Done, WAIT_MS);
        ok(Wait == WAIT_OBJECT_0, "routine did not run after late cancel (%lu)\n", Wait);
        if (Result)
            pFreeAddrInfoExW(Result);
        if (Wait != WAIT_OBJECT_0)
            break;
    }
    ok(i < 20, "routine-mode lookup was never cancelled\n");

    /* A handle that names nothing */
    Handle = (HANDLE)(ULONG_PTR)0x12345;
    Error = pGetAddrInfoExCancel(&Handle);
    ok(Error == WSA_INVALID_HANDLE, "cancel of a bogus handle returned %d\n", Error);
    Handle = NULL;
    Error = pGetAddrInfoExCancel(&Handle);
    ok(Error == WSA_INVALID_HANDLE, "cancel of a NULL handle returned %d\n", Error);
    Error = pGetAddrInfoExCancel(NULL);
    ok(Error == WSA_INVALID_HANDLE, "cancel(NULL) returned %d\n", Error);

    /* ... and one that already completed */
    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = Event;
    ResetEvent(Event);
    Result = NULL;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, NULL, &Handle);
    ok(Error == WSA_IO_PENDING, "GetAddrInfoExW returned %d\n", Error);
    if (Error == WSA_IO_PENDING)
    {
        WaitForSingleObject(Event, WAIT_MS);
        Sleep(50);
        Error = pGetAddrInfoExCancel(&Handle);
        ok(Error == WSA_INVALID_HANDLE, "cancel after completion returned %d\n", Error);
        if (Result)
            pFreeAddrInfoExW(Result);
    }
    CloseHandle(Event);
}

static void TestSyncAndInvalid(void)
{
    ADDRINFOEXW Hints, *Result;
    OVERLAPPED Overlapped;
    struct timeval Timeout = {5, 0};
    HANDLE Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    INT Error;

    memset(&Hints, 0, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;

    /* Synchronous: the result is returned directly */
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", L"80", NS_DNS, NULL, &Hints, &Result, NULL, NULL, NULL, NULL);
    ok(Error == 0, "sync localhost returned %d\n", Error);
    if (Error == 0)
    {
        CheckLoopback(Result, 80, SOCK_STREAM, __LINE__);
        pFreeAddrInfoExW(Result);
    }

    /* Synchronous calls take no timeout (WSAEINVAL on Windows 10) */
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_DNS, NULL, &Hints, &Result, &Timeout, NULL, NULL, NULL);
    ok(Error == WSAEINVAL, "sync with timeout returned %d\n", Error);
    ok(Result == NULL, "sync with timeout: Result = %p\n", Result);

    /* ... but an overlapped one does */
    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = Event;
    Result = NULL;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_DNS, NULL, &Hints, &Result, &Timeout, &Overlapped, NULL, NULL);
    ok(Error == WSA_IO_PENDING, "overlapped with timeout returned %d\n", Error);
    if (Error == WSA_IO_PENDING)
    {
        ok(WaitForSingleObject(Event, WAIT_MS) == WAIT_OBJECT_0, "timeout case did not complete\n");
        if (Result)
            pFreeAddrInfoExW(Result);
    }

    /* A completion routine needs an OVERLAPPED */
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_DNS, NULL, &Hints, &Result, NULL, NULL, CompletionRoutine, NULL);
    ok(Error == WSAEINVAL, "routine without OVERLAPPED returned %d\n", Error);
    ok(Result == NULL, "routine without OVERLAPPED: Result = %p\n", Result);

    /* An OVERLAPPED needs exactly one of hEvent and the routine */
    memset(&Overlapped, 0, sizeof(Overlapped));
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, NULL, NULL);
    ok(Error == WSAEINVAL, "OVERLAPPED without event or routine returned %d\n", Error);
    ok(Result == NULL, "OVERLAPPED without event or routine: Result = %p\n", Result);
    Overlapped.hEvent = Event;
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_DNS, NULL, &Hints, &Result, NULL, &Overlapped, CompletionRoutine, NULL);
    ok(Error == WSAEINVAL, "OVERLAPPED with event and routine returned %d\n", Error);
    ok(Result == NULL, "OVERLAPPED with event and routine: Result = %p\n", Result);

    /* Unsupported namespace; a numeric-only lookup of a name */
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"localhost", NULL, NS_NETBT, NULL, NULL, &Result, NULL, NULL, NULL, NULL);
    ok(Error == WSASERVICE_NOT_FOUND, "NS_NETBT returned %d\n", Error);
    ok(Result == NULL, "NS_NETBT: Result = %p\n", Result);
    Hints.ai_flags = AI_NUMERICHOST;
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"not-a-numeric-host.invalid", NULL, NS_ALL, NULL, &Hints, &Result, NULL, NULL, NULL, NULL);
    ok(Error == WSAHOST_NOT_FOUND, "AI_NUMERICHOST on a name returned %d\n", Error);
    ok(Result == NULL, "AI_NUMERICHOST on a name: Result = %p\n", Result);
    Result = NULL;
    Error = pGetAddrInfoExW(L"127.0.0.1", NULL, NS_DNS, NULL, NULL, &Result, NULL, NULL, NULL, NULL);
    ok(Error == 0, "numeric sync returned %d\n", Error);
    if (Error == 0)
    {
        ok(Result != NULL && Result->ai_family == AF_INET, "numeric sync result\n");
        pFreeAddrInfoExW(Result);
    }

    /* Freeing NULL is harmless */
    pFreeAddrInfoExW(NULL);
    CloseHandle(Event);
}

START_TEST(GetAddrInfoEx)
{
    HMODULE Ws2 = GetModuleHandleW(L"ws2_32.dll");
    BOOL Expected = is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    WCHAR Env[8];
    WSADATA Data;

    pGetAddrInfoExW = (PFN_GetAddrInfoExW)GetProcAddress(Ws2, "GetAddrInfoExW");
    pGetAddrInfoExCancel = (PFN_GetAddrInfoExCancel)GetProcAddress(Ws2, "GetAddrInfoExCancel");
    pGetAddrInfoExOverlappedResult = (PFN_GetAddrInfoExOverlappedResult)GetProcAddress(Ws2, "GetAddrInfoExOverlappedResult");
    pFreeAddrInfoExW = (PFN_FreeAddrInfoExW)GetProcAddress(Ws2, "FreeAddrInfoExW");

    if (!pGetAddrInfoExW || !pFreeAddrInfoExW || !pGetAddrInfoExCancel)
    {
        ok(!Expected, "GetAddrInfoExW/GetAddrInfoExCancel/FreeAddrInfoExW are not exported\n");
        skip("GetAddrInfoExW is not exported\n");
        return;
    }
    ok(pGetAddrInfoExOverlappedResult != NULL || !Expected, "GetAddrInfoExOverlappedResult is not exported\n");

    if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }
    g_Done = CreateEventW(NULL, TRUE, FALSE, NULL);

    TestSyncAndInvalid();
    TestInline();
    TestEventOnly();

    /* The case behind the .NET hang: a thread that never waits alertably */
    TestRoutineFromThread(L"localhost", FALSE, FALSE);
    /* Not an APC to the caller even if the caller does wait alertably */
    TestRoutineFromThread(L"localhost", TRUE, FALSE);

    TestCancel();

    if (GetEnvironmentVariableW(L"WS2_APITEST_NET", Env, ARRAYSIZE(Env)) && Env[0] == L'1')
        TestRoutineFromThread(L"example.com", FALSE, TRUE);
    else
        skip("WS2_APITEST_NET=1 not set, skipping the networked lookup\n");

    CloseHandle(g_Done);
    WSACleanup();
}
