/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:     Test for WSAEnumNameSpaceProviders{A,W} and for the query sets
 *              WSALookupServiceBeginW accepts (NLA / NS_ALL with no class id)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <nspapi.h>

/*
 * The enumeration must return an array of WSANAMESPACE_INFO structures, then
 * the identifier strings they point to (callers index it as info[i]). Once the
 * NLA namespace provider was registered next to the DNS one, a layout that put
 * each string right behind its own structure made info[1] land inside the first
 * string, and callers crashed while printing lpszIdentifier. The required size
 * must also be reported (WSAEFAULT) before anything is written, and cover the
 * packed array. Expected values come from Windows 10 22H2.
 */

static const GUID NlaProviderId = { 0x6642243a, 0x3ba8, 0x4aa6,
                                    { 0xba, 0xa5, 0x2e, 0x0b, 0xd7, 0x1f, 0xdd, 0x83 } };
static const GUID TcpipProviderId = { 0x22059d40, 0x7e9e, 0x11cf,
                                      { 0xae, 0x5a, 0x00, 0xaa, 0x00, 0xa7, 0x11, 0x2b } };

static void Test_EnumA(INT *pCount)
{
    LPWSANAMESPACE_INFOA Info;
    DWORD Len, Need, Expected;
    INT Ret, Count, i, j;
    BOOL SawDns = FALSE, SawNla = FALSE;
    PBYTE Buf, End;

    *pCount = 0;

    SetLastError(0xdeadbeef);
    Ret = WSAEnumNameSpaceProvidersA(NULL, NULL);
    ok(Ret == SOCKET_ERROR, "NULL length: Ret = %d\n", Ret);
    ok(WSAGetLastError() == WSAEFAULT, "NULL length: error %d\n", WSAGetLastError());

    Len = 0;
    WSASetLastError(0);
    Ret = WSAEnumNameSpaceProvidersA(&Len, NULL);
    ok(Ret == SOCKET_ERROR, "size query: Ret = %d\n", Ret);
    ok(WSAGetLastError() == WSAEFAULT, "size query: error %d\n", WSAGetLastError());
    ok(Len > 0, "size query: Len = %lu\n", Len);
    if (Len == 0)
        return;
    Need = Len;

    Buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Need + 64);
    if (!Buf)
    {
        skip("out of memory\n");
        return;
    }
    Info = (LPWSANAMESPACE_INFOA)Buf;

    /* One byte short: fail and report the same size */
    Len = Need - 1;
    WSASetLastError(0);
    Ret = WSAEnumNameSpaceProvidersA(&Len, Info);
    ok(Ret == SOCKET_ERROR, "short buffer: Ret = %d\n", Ret);
    ok(WSAGetLastError() == WSAEFAULT, "short buffer: error %d\n", WSAGetLastError());
    ok(Len == Need, "short buffer: Len = %lu, expected %lu\n", Len, Need);

    /* Exactly enough */
    Len = Need;
    WSASetLastError(0);
    Ret = WSAEnumNameSpaceProvidersA(&Len, Info);
    ok(Ret > 0, "exact buffer: Ret = %d, error %d\n", Ret, WSAGetLastError());
    if (Ret <= 0)
    {
        HeapFree(GetProcessHeap(), 0, Buf);
        return;
    }
    Count = Ret;
    *pCount = Count;

    /* The array plus every identifier including its terminator must fit in the size the API asked for */
    Expected = Count * sizeof(WSANAMESPACE_INFOA);
    End = Buf + Need;
    for (i = 0; i < Count; i++)
    {
        PBYTE Id = (PBYTE)Info[i].lpszIdentifier;

        ok(Id != NULL, "A[%d]: identifier is NULL\n", i);
        if (!Id)
            continue;
        ok(Id >= Buf + Count * sizeof(WSANAMESPACE_INFOA) && Id < End,
           "A[%d]: identifier %p outside the string area [%p, %p)\n", i, Id,
           Buf + Count * sizeof(WSANAMESPACE_INFOA), End);
        if (Id < Buf + Count * sizeof(WSANAMESPACE_INFOA) || Id >= End)
            continue;

        /* NUL-terminated inside the buffer, not empty */
        ok(memchr(Id, 0, End - Id) != NULL, "A[%d]: identifier not terminated\n", i);
        if (!memchr(Id, 0, End - Id))
            continue;
        ok(Id[0] != 0, "A[%d]: empty identifier\n", i);
        Expected += strlen((char *)Id) + 1;

        ok(Info[i].dwNameSpace != 0, "A[%d]: dwNameSpace is 0\n", i);
        ok(Info[i].fActive == TRUE || Info[i].fActive == FALSE, "A[%d]: fActive %d\n", i, Info[i].fActive);

        if (Info[i].dwNameSpace == NS_DNS && IsEqualGUID(&Info[i].NSProviderId, &TcpipProviderId))
            SawDns = TRUE;
        if (Info[i].dwNameSpace == NS_NLA && IsEqualGUID(&Info[i].NSProviderId, &NlaProviderId))
            SawNla = TRUE;

        for (j = 0; j < i; j++)
        {
            ok(!IsEqualGUID(&Info[i].NSProviderId, &Info[j].NSProviderId),
               "A[%d] and A[%d] have the same provider id\n", i, j);
        }
    }
    /* Windows 10 reports slightly more than it writes (732 for 7 providers, 450 used) */
    ok(Expected <= Need, "A: array + strings = %lu bytes, WSAEnumNameSpaceProvidersA asked for only %lu\n", Expected, Need);
    ok(SawDns, "A: no NS_DNS (Tcpip) provider\n");
    ok(SawNla, "A: no NS_NLA provider\n");

    /* A larger buffer works and returns the same count */
    HeapFree(GetProcessHeap(), 0, Buf);
    Buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Need * 2);
    if (Buf)
    {
        Len = Need * 2;
        Ret = WSAEnumNameSpaceProvidersA(&Len, (LPWSANAMESPACE_INFOA)Buf);
        ok(Ret == Count, "large buffer: Ret = %d, expected %d\n", Ret, Count);
        HeapFree(GetProcessHeap(), 0, Buf);
    }
}

static void Test_EnumW(INT CountA)
{
    LPWSANAMESPACE_INFOW Info;
    DWORD Len, Need, Expected;
    INT Ret, Count, i;
    PBYTE Buf, End;

    Len = 0;
    WSASetLastError(0);
    Ret = WSAEnumNameSpaceProvidersW(&Len, NULL);
    ok(Ret == SOCKET_ERROR, "W size query: Ret = %d\n", Ret);
    ok(WSAGetLastError() == WSAEFAULT, "W size query: error %d\n", WSAGetLastError());
    ok(Len > 0, "W size query: Len = %lu\n", Len);
    if (Len == 0)
        return;
    Need = Len;

    Buf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Need + 64);
    if (!Buf)
    {
        skip("out of memory\n");
        return;
    }
    Info = (LPWSANAMESPACE_INFOW)Buf;

    Len = Need - 1;
    WSASetLastError(0);
    Ret = WSAEnumNameSpaceProvidersW(&Len, Info);
    ok(Ret == SOCKET_ERROR, "W short buffer: Ret = %d\n", Ret);
    ok(WSAGetLastError() == WSAEFAULT, "W short buffer: error %d\n", WSAGetLastError());
    ok(Len == Need, "W short buffer: Len = %lu, expected %lu\n", Len, Need);

    Len = Need;
    Ret = WSAEnumNameSpaceProvidersW(&Len, Info);
    ok(Ret > 0, "W exact buffer: Ret = %d, error %d\n", Ret, WSAGetLastError());
    if (Ret <= 0)
    {
        HeapFree(GetProcessHeap(), 0, Buf);
        return;
    }
    Count = Ret;
    ok(Count == CountA, "W count %d differs from A count %d\n", Count, CountA);

    Expected = Count * sizeof(WSANAMESPACE_INFOW);
    End = Buf + Need;
    for (i = 0; i < Count; i++)
    {
        PBYTE Id = (PBYTE)Info[i].lpszIdentifier;
        SIZE_T Max;

        ok(Id != NULL, "W[%d]: identifier is NULL\n", i);
        if (!Id)
            continue;
        ok(Id >= Buf + Count * sizeof(WSANAMESPACE_INFOW) && Id + sizeof(WCHAR) <= End,
           "W[%d]: identifier %p outside the string area\n", i, Id);
        if (Id < Buf + Count * sizeof(WSANAMESPACE_INFOW) || Id + sizeof(WCHAR) > End)
            continue;
        ok(((ULONG_PTR)Id % sizeof(WCHAR)) == 0, "W[%d]: identifier %p misaligned\n", i, Id);

        Max = (End - Id) / sizeof(WCHAR);
        ok(wcsnlen(Info[i].lpszIdentifier, Max) < Max, "W[%d]: identifier not terminated\n", i);
        if (wcsnlen(Info[i].lpszIdentifier, Max) >= Max)
            continue;
        ok(Info[i].lpszIdentifier[0] != 0, "W[%d]: empty identifier\n", i);
        Expected += (wcslen(Info[i].lpszIdentifier) + 1) * sizeof(WCHAR);
    }
    ok(Expected <= Need, "W: array + strings = %lu bytes, WSAEnumNameSpaceProvidersW asked for only %lu\n", Expected, Need);

    HeapFree(GetProcessHeap(), 0, Buf);
}

/* Begin a lookup, drain it, and check that it ends with WSA_E_NO_MORE */
static void DrainLookup(const char *Name, LPWSAQUERYSETW Query, DWORD Flags)
{
    HANDLE Lookup = (HANDLE)(ULONG_PTR)0xdeadbeef;
    INT Ret, i;
    DWORD Size, Err = 0;
    PBYTE Buf = HeapAlloc(GetProcessHeap(), 0, 16384);

    if (!Buf)
    {
        skip("out of memory\n");
        return;
    }

    WSASetLastError(0);
    Ret = WSALookupServiceBeginW(Query, Flags, &Lookup);
    ok(Ret == 0, "%s: WSALookupServiceBeginW failed, error %d\n", Name, WSAGetLastError());
    if (Ret != 0)
    {
        HeapFree(GetProcessHeap(), 0, Buf);
        return;
    }
    ok(Lookup != (HANDLE)(ULONG_PTR)0xdeadbeef && Lookup != NULL, "%s: handle not filled\n", Name);

    for (i = 0; i < 64; i++)
    {
        Size = 16384;
        ZeroMemory(Buf, sizeof(WSAQUERYSETW));
        WSASetLastError(0);
        Ret = WSALookupServiceNextW(Lookup, 0, &Size, (LPWSAQUERYSETW)Buf);
        if (Ret == SOCKET_ERROR)
        {
            Err = WSAGetLastError();
            break;
        }
    }
    ok(Ret == SOCKET_ERROR, "%s: the lookup never ended\n", Name);
    ok(Err == WSA_E_NO_MORE, "%s: lookup ended with error %lu, expected WSA_E_NO_MORE (%d)\n",
       Name, Err, WSA_E_NO_MORE);

    Ret = WSALookupServiceEnd(Lookup);
    ok(Ret == 0, "%s: WSALookupServiceEnd failed, error %d\n", Name, WSAGetLastError());
    HeapFree(GetProcessHeap(), 0, Buf);
}

static void Test_Lookup(void)
{
    WSAQUERYSETW Query;
    HANDLE Lookup;
    INT Ret;
    DWORD Err;

    /* Invalid parameters */
    Lookup = (HANDLE)(ULONG_PTR)0xdeadbeef;
    WSASetLastError(0);
    Ret = WSALookupServiceBeginW(NULL, 0, &Lookup);
    ok(Ret == SOCKET_ERROR, "NULL query: Ret = %d\n", Ret);
    ok(WSAGetLastError() == WSAEFAULT, "NULL query: error %d\n", WSAGetLastError());

    ZeroMemory(&Query, sizeof(Query));
    WSASetLastError(0);
    Ret = WSALookupServiceBeginW(&Query, 0, NULL);
    ok(Ret == SOCKET_ERROR, "NULL handle: Ret = %d\n", Ret);
    ok(WSAGetLastError() == WSAEFAULT, "NULL handle: error %d\n", WSAGetLastError());

    /* An all-zero query set (dwSize 0) must fail; Windows 10 gives ERROR_INVALID_PARAMETER.
     * The NLA provider accepting NULL class ids made this succeed (sock.c WSALookupServiceBeginW). */
    Lookup = (HANDLE)(ULONG_PTR)0xdeadbeef;
    WSASetLastError(0);
    Ret = WSALookupServiceBeginW(&Query, 0, &Lookup);
    Err = WSAGetLastError();
    ok(Ret == SOCKET_ERROR, "zeroed query set: WSALookupServiceBeginW should have failed\n");
    ok(Err == ERROR_INVALID_PARAMETER || Err == WSAEINVAL, "zeroed query set: error %lu\n", Err);
    if (Ret == 0)
        WSALookupServiceEnd(Lookup);

    Query.dwSize = sizeof(Query) - 1;
    Lookup = (HANDLE)(ULONG_PTR)0xdeadbeef;
    WSASetLastError(0);
    Ret = WSALookupServiceBeginW(&Query, LUP_RETURN_ALL | LUP_DEEP, &Lookup);
    Err = WSAGetLastError();
    ok(Ret == SOCKET_ERROR, "short dwSize: WSALookupServiceBeginW should have failed\n");
    ok(Err == ERROR_INVALID_PARAMETER || Err == WSAEINVAL, "short dwSize: error %lu\n", Err);
    if (Ret == 0)
        WSALookupServiceEnd(Lookup);

    /* A DNS query needs a service class */
    Query.dwSize = sizeof(Query);
    Query.dwNameSpace = NS_DNS;
    Lookup = (HANDLE)(ULONG_PTR)0xdeadbeef;
    Ret = WSALookupServiceBeginW(&Query, LUP_RETURN_ALL | LUP_DEEP, &Lookup);
    ok(Ret == SOCKET_ERROR, "NS_DNS without class id should have failed\n");
    if (Ret == 0)
        WSALookupServiceEnd(Lookup);

    /* The standard network list queries have no class id and must work */
    ZeroMemory(&Query, sizeof(Query));
    Query.dwSize = sizeof(Query);
    Query.dwNameSpace = NS_NLA;
    DrainLookup("NS_NLA", &Query, LUP_RETURN_ALL | LUP_DEEP);

    Query.dwNameSpace = NS_ALL;
    DrainLookup("NS_ALL", &Query, LUP_RETURN_ALL | LUP_DEEP);

    /* A larger dwSize is accepted */
    Query.dwSize = sizeof(Query) + 8;
    DrainLookup("dwSize+8", &Query, LUP_RETURN_ALL | LUP_DEEP);
}

START_TEST(WSAEnumNameSpaceProviders)
{
    WSADATA WsaData;
    INT Started, Count;

    Started = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Started == 0, "WSAStartup failed: %d\n", Started);
    if (Started != 0)
        return;

    Test_EnumA(&Count);
    Test_EnumW(Count);
    Test_Lookup();

    WSACleanup();
}
