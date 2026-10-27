/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for ws2_32!GetAddrInfoExW and FreeAddrInfoExW (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* The Vista+ address info types are hidden from the 0x502 headers this module is built with */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#undef WINVER
#define WINVER 0x0600

#include "ws2_32.h"

#include <ws2tcpip.h>

typedef INT (WSAAPI *PFN_GetAddrInfoExW)(PCWSTR, PCWSTR, DWORD, LPGUID, const ADDRINFOEXW *, PADDRINFOEXW *,
                                         struct timeval *, LPOVERLAPPED, LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
typedef VOID (WSAAPI *PFN_FreeAddrInfoExW)(PADDRINFOEXW);

static volatile LONG g_Completed;
static volatile DWORD g_CompletedError = 0xdeadbeef;
static LPWSAOVERLAPPED g_CompletedOverlapped;

static VOID CALLBACK CompletionRoutine(DWORD Error, DWORD Bytes, LPWSAOVERLAPPED Overlapped)
{
    g_CompletedError = Error;
    g_CompletedOverlapped = Overlapped;
    InterlockedIncrement(&g_Completed);
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

START_TEST(GetAddrInfoEx)
{
    HMODULE Ws2 = GetModuleHandleW(L"ws2_32.dll");
    PFN_GetAddrInfoExW pGetAddrInfoExW = (PFN_GetAddrInfoExW)GetProcAddress(Ws2, "GetAddrInfoExW");
    PFN_FreeAddrInfoExW pFreeAddrInfoExW = (PFN_FreeAddrInfoExW)GetProcAddress(Ws2, "FreeAddrInfoExW");
    BOOL Expected = is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    WSADATA Data;
    ADDRINFOEXW Hints, *Result;
    OVERLAPPED Overlapped;
    HANDLE Handle;
    INT Error;
    DWORD Wait;

    if (!pGetAddrInfoExW || !pFreeAddrInfoExW)
    {
        ok(!Expected, "GetAddrInfoExW/FreeAddrInfoExW are not exported\n");
        skip("GetAddrInfoExW is not exported\n");
        return;
    }

    if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    /* Synchronous numeric lookup: no network needed */
    memset(&Hints, 0, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    Hints.ai_flags = AI_NUMERICHOST;
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"127.0.0.1", L"80", NS_ALL, NULL, &Hints, &Result, NULL, NULL, NULL, NULL);
    ok(Error == 0, "GetAddrInfoExW returned %d\n", Error);
    if (Error == 0)
    {
        CheckLoopback(Result, 80, SOCK_STREAM, __LINE__);
        pFreeAddrInfoExW(Result);
    }

    /* NS_DNS, no hints, no service */
    Result = NULL;
    Error = pGetAddrInfoExW(L"127.0.0.1", NULL, NS_DNS, NULL, NULL, &Result, NULL, NULL, NULL, &Handle);
    ok(Error == 0, "GetAddrInfoExW returned %d\n", Error);
    if (Error == 0)
    {
        ok(Result != NULL, "no result\n");
        if (Result)
        {
            ok(Result->ai_family == AF_INET, "family %d\n", Result->ai_family);
            ok(Result->ai_next == NULL || Result->ai_next->ai_family != 0, "bad chain\n");
        }
        pFreeAddrInfoExW(Result);
    }

    /* Freeing NULL is harmless */
    pFreeAddrInfoExW(NULL);

    /* A non-numeric name with AI_NUMERICHOST fails without a result */
    Hints.ai_flags = AI_NUMERICHOST;
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"not-a-numeric-host.invalid", NULL, NS_ALL, NULL, &Hints, &Result, NULL, NULL, NULL, NULL);
    ok(Error == WSAHOST_NOT_FOUND, "GetAddrInfoExW returned %d\n", Error);
    ok(Result == NULL, "Result = %p\n", Result);

    /* Asynchronous form with an event: completes (inline here), result is valid */
    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Hints.ai_flags = AI_NUMERICHOST;
    Result = NULL;
    Handle = (HANDLE)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"127.0.0.1", L"8080", NS_ALL, NULL, &Hints, &Result, NULL, &Overlapped, NULL, &Handle);
    ok(Error == 0 || Error == WSA_IO_PENDING, "GetAddrInfoExW returned %d\n", Error);
    if (Error == WSA_IO_PENDING)
    {
        Wait = WaitForSingleObject(Overlapped.hEvent, 10000);
        ok(Wait == WAIT_OBJECT_0, "wait returned %lu\n", Wait);
        ok(Overlapped.Internal == 0, "overlapped result %lx\n", (ULONG)Overlapped.Internal);
    }
    if (Error == 0 || Error == WSA_IO_PENDING)
    {
        CheckLoopback(Result, 8080, SOCK_STREAM, __LINE__);
        pFreeAddrInfoExW(Result);
    }
    CloseHandle(Overlapped.hEvent);

    /* Asynchronous form with a completion routine: runs at an alertable wait */
    memset(&Overlapped, 0, sizeof(Overlapped));
    g_Completed = 0;
    Result = NULL;
    Error = pGetAddrInfoExW(L"127.0.0.1", L"443", NS_ALL, NULL, &Hints, &Result, NULL, &Overlapped, CompletionRoutine, &Handle);
    ok(Error == 0 || Error == WSA_IO_PENDING, "GetAddrInfoExW returned %d\n", Error);
    if (Error == WSA_IO_PENDING)
    {
        Wait = SleepEx(10000, TRUE);
        ok(Wait == WAIT_IO_COMPLETION, "SleepEx returned %lu\n", Wait);
        ok(g_Completed == 1, "completion routine ran %ld times\n", g_Completed);
        ok(g_CompletedError == 0, "completion error %lu\n", g_CompletedError);
        ok(g_CompletedOverlapped == &Overlapped, "completion overlapped %p\n", g_CompletedOverlapped);
    }
    if (Error == 0 || Error == WSA_IO_PENDING)
    {
        CheckLoopback(Result, 443, SOCK_STREAM, __LINE__);
        pFreeAddrInfoExW(Result);
    }

    /* Invalid parameters */
    Result = (PADDRINFOEXW)(ULONG_PTR)0x1234;
    Error = pGetAddrInfoExW(L"127.0.0.1", NULL, NS_NETBT, NULL, NULL, &Result, NULL, NULL, NULL, NULL);
    ok(Error != 0, "NS_NETBT unexpectedly succeeded\n");
    Result = NULL;
    Error = pGetAddrInfoExW(L"127.0.0.1", NULL, NS_ALL, NULL, NULL, &Result, NULL, NULL, CompletionRoutine, NULL);
    ok(Error == WSAEINVAL, "completion routine without OVERLAPPED returned %d\n", Error);

    WSACleanup();
}
