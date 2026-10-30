/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for NotifyStableUnicastIpAddressTable
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winsock2.h>
#include <iphlpapi.h>

#define SENTINEL ((PVOID)(ULONG_PTR)0x12345678)

typedef VOID (WINAPI *STABLE_CALLBACK)(PVOID, PVOID);
typedef DWORD (WINAPI *PNOTIFY_STABLE)(ADDRESS_FAMILY, PVOID *, STABLE_CALLBACK, PVOID, HANDLE *);
typedef DWORD (WINAPI *PGET_UNICAST)(ADDRESS_FAMILY, PVOID *);
typedef VOID (WINAPI *PFREE_MIB_TABLE)(PVOID);

static VOID WINAPI Callback(PVOID Context, PVOID Table)
{
}

static void test_family(PNOTIFY_STABLE pNotify, PGET_UNICAST pGet, PFREE_MIB_TABLE pFree, ADDRESS_FAMILY Family)
{
    PVOID Table = SENTINEL, Other = NULL;
    HANDLE Handle = SENTINEL;
    DWORD Error, Count;
    BOOL Loopback = FALSE;

    Error = pNotify(Family, &Table, Callback, (PVOID)(ULONG_PTR)0x77, &Handle);
    ok(Error == NO_ERROR, "[%u] NotifyStableUnicastIpAddressTable returned %lu\n", Family, Error);
    if (Error != NO_ERROR)
        return;
    ok(Table != NULL && Table != SENTINEL, "[%u] No table: %p\n", Family, Table);
    ok(Handle == NULL, "[%u] Handle is %p for a stable table\n", Family, Handle);
    if (!Table || Table == SENTINEL)
        return;

    Count = *(PULONG)Table;

    /* The same rows as GetUnicastIpAddressTable gives (nothing changes between the two calls) */
    Error = pGet(Family, &Other);
    ok(Error == NO_ERROR, "[%u] GetUnicastIpAddressTable returned %lu\n", Family, Error);
    if (Error == NO_ERROR && Other)
    {
        ok(*(PULONG)Other == Count, "[%u] %lu rows, GetUnicastIpAddressTable has %lu\n", Family, Count, *(PULONG)Other);
        pFree(Other);
    }
    if (Family != AF_INET6)
        Loopback = Count != 0;
    ok(Family == AF_INET6 || Loopback, "[%u] No address at all, not even loopback\n", Family);
    pFree(Table);
}

START_TEST(NotifyStableUnicastIpAddressTable)
{
    HMODULE hIphlpapi = GetModuleHandleW(L"iphlpapi.dll");
    PNOTIFY_STABLE pNotify;
    PGET_UNICAST pGet;
    PFREE_MIB_TABLE pFree;
    PVOID Table;
    HANDLE Handle;
    DWORD Error;

    pNotify = (PNOTIFY_STABLE)GetProcAddress(hIphlpapi, "NotifyStableUnicastIpAddressTable");
    pGet = (PGET_UNICAST)GetProcAddress(hIphlpapi, "GetUnicastIpAddressTable");
    pFree = (PFREE_MIB_TABLE)GetProcAddress(hIphlpapi, "FreeMibTable");
    if (!pNotify && is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64"))
    {
        skip("No NT 6 exports\n");
        return;
    }
    ok(pNotify != NULL, "NotifyStableUnicastIpAddressTable is not exported\n");
    if (!pNotify || !pGet || !pFree)
        return;

    /* invalid arguments */
    Table = SENTINEL;
    Handle = NULL;
    Error = pNotify(99, &Table, Callback, NULL, &Handle);
    ok(Error == ERROR_INVALID_PARAMETER, "Bad family: %lu\n", Error);
    Handle = NULL;
    Error = pNotify(AF_UNSPEC, NULL, Callback, NULL, &Handle);
    ok(Error == ERROR_INVALID_PARAMETER, "No table pointer: %lu\n", Error);
    Table = SENTINEL;
    Error = pNotify(AF_UNSPEC, &Table, Callback, NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "No handle pointer: %lu\n", Error);
    ok(Table == SENTINEL || Table == NULL, "Table written on error: %p\n", Table);

    /* a stable table needs no callback */
    Table = SENTINEL;
    Handle = SENTINEL;
    Error = pNotify(AF_UNSPEC, &Table, NULL, NULL, &Handle);
    ok(Error == NO_ERROR, "No callback: %lu\n", Error);
    if (Error == NO_ERROR && Table && Table != SENTINEL)
        pFree(Table);

    test_family(pNotify, pGet, pFree, AF_UNSPEC);
    test_family(pNotify, pGet, pFree, AF_INET);
    test_family(pNotify, pGet, pFree, AF_INET6);
}
