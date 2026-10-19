/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the Vista IP Helper table getters and ConvertLengthToIpv4Mask
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winsock2.h>
#include <iphlpapi.h>

#define SENTINEL ((PVOID)(ULONG_PTR)0x12345678)

typedef DWORD (WINAPI *PGET_TABLE)(PVOID *);
typedef DWORD (WINAPI *PGET_TABLE_FAMILY)(ULONG, PVOID *);
typedef DWORD (WINAPI *PGET_TABLE_LEVEL)(ULONG, PVOID *);
typedef VOID (WINAPI *PFREE_MIB_TABLE)(PVOID);

static PFREE_MIB_TABLE pFreeMibTable;

/* Either the table exists, or the call failed and left no table behind. It never succeeds with the output untouched. */
static void check_table_result(const char *name, DWORD ret, PVOID table)
{
    if (ret == NO_ERROR)
    {
        ok(table != NULL && table != SENTINEL, "%s: success but table is %p\n", name, table);
        if (table && table != SENTINEL && pFreeMibTable)
            pFreeMibTable(table);
    }
    else
    {
        ok(ret != ERROR_INVALID_PARAMETER, "%s: valid arguments rejected\n", name);
        ok(table == NULL, "%s: error %lu but table is %p\n", name, ret, table);
    }
}

static void test_family_table(HMODULE hIphlpapi, const char *name)
{
    PGET_TABLE_FAMILY pGet = (PGET_TABLE_FAMILY)GetProcAddress(hIphlpapi, name);
    static const ULONG families[] = { AF_UNSPEC, AF_INET, AF_INET6 };
    PVOID table;
    DWORD ret;
    int i;

    if (!pGet)
    {
        skip("%s is not available\n", name);
        return;
    }

    for (i = 0; i < 3; i++)
    {
        table = SENTINEL;
        ret = pGet(families[i], &table);
        check_table_result(name, ret, table);
    }

    table = SENTINEL;
    ret = pGet(AF_IPX, &table);
    ok(ret == ERROR_INVALID_PARAMETER, "%s(AF_IPX): got %lu\n", name, ret);

    ret = pGet(AF_INET, NULL);
    ok(ret == ERROR_INVALID_PARAMETER, "%s(NULL table): got %lu\n", name, ret);
}

START_TEST(IpTables2)
{
    HMODULE hIphlpapi = GetModuleHandleW(L"iphlpapi.dll");
    PGET_TABLE pGetIfTable2;
    PGET_TABLE_LEVEL pGetIfTable2Ex;
    DWORD (WINAPI *pConvertLengthToIpv4Mask)(ULONG, PULONG);
    DWORD (WINAPI *pGetIfEntry2)(PVOID);
    DWORD (WINAPI *pGetUnicastIpAddressEntry)(PVOID);
    PVOID table;
    ULONG mask, i;
    DWORD ret;
    BYTE row[512];

    if (!hIphlpapi)
        hIphlpapi = LoadLibraryW(L"iphlpapi.dll");

    pFreeMibTable = (PFREE_MIB_TABLE)GetProcAddress(hIphlpapi, "FreeMibTable");
    pGetIfTable2 = (PGET_TABLE)GetProcAddress(hIphlpapi, "GetIfTable2");
    pGetIfTable2Ex = (PGET_TABLE_LEVEL)GetProcAddress(hIphlpapi, "GetIfTable2Ex");
    pConvertLengthToIpv4Mask = (PVOID)GetProcAddress(hIphlpapi, "ConvertLengthToIpv4Mask");
    pGetIfEntry2 = (PVOID)GetProcAddress(hIphlpapi, "GetIfEntry2");
    pGetUnicastIpAddressEntry = (PVOID)GetProcAddress(hIphlpapi, "GetUnicastIpAddressEntry");

    if (!pGetIfTable2 || !pFreeMibTable)
    {
        skip("The Vista IP Helper functions are not available\n");
        return;
    }

    table = SENTINEL;
    ret = pGetIfTable2(&table);
    check_table_result("GetIfTable2", ret, table);

    if (pGetIfTable2Ex)
    {
        /* level 2 is rejected by Windows Server 2008 R2 and accepted by Windows 10 */
        for (i = 0; i <= 1; i++)
        {
            table = SENTINEL;
            ret = pGetIfTable2Ex(i, &table);
            check_table_result("GetIfTable2Ex", ret, table);
        }
        table = SENTINEL;
        ret = pGetIfTable2Ex(3, &table);
        ok(ret == ERROR_INVALID_PARAMETER, "GetIfTable2Ex(3): got %lu\n", ret);
    }

    test_family_table(hIphlpapi, "GetUnicastIpAddressTable");
    test_family_table(hIphlpapi, "GetIpNetTable2");
    test_family_table(hIphlpapi, "GetIpForwardTable2");

    if (pGetIfEntry2)
    {
        memset(row, 0, sizeof(row));
        ret = pGetIfEntry2(row);
        ok(ret == ERROR_INVALID_PARAMETER, "GetIfEntry2(empty row): got %lu\n", ret);
        ret = pGetIfEntry2(NULL);
        ok(ret == ERROR_INVALID_PARAMETER, "GetIfEntry2(NULL): got %lu\n", ret);
    }

    if (pGetUnicastIpAddressEntry)
    {
        memset(row, 0, sizeof(row));
        ret = pGetUnicastIpAddressEntry(row);
        ok(ret == ERROR_INVALID_PARAMETER, "GetUnicastIpAddressEntry(empty row): got %lu\n", ret);
        ret = pGetUnicastIpAddressEntry(NULL);
        ok(ret == ERROR_INVALID_PARAMETER, "GetUnicastIpAddressEntry(NULL): got %lu\n", ret);
    }

    if (!pConvertLengthToIpv4Mask)
    {
        skip("ConvertLengthToIpv4Mask is not available\n");
        return;
    }

    for (i = 0; i <= 32; i++)
    {
        mask = 0xdeadbeef;
        ret = pConvertLengthToIpv4Mask(i, &mask);
        ok(ret == NO_ERROR, "ConvertLengthToIpv4Mask(%lu): got %lu\n", i, ret);
        ok(mask == (i ? htonl(0xffffffffUL << (32 - i)) : 0),
           "ConvertLengthToIpv4Mask(%lu): mask %08lx\n", i, mask);
    }

    mask = 0xdeadbeef;
    ret = pConvertLengthToIpv4Mask(33, &mask);
    ok(ret == ERROR_INVALID_PARAMETER, "ConvertLengthToIpv4Mask(33): got %lu\n", ret);
    ok(mask == INADDR_NONE, "ConvertLengthToIpv4Mask(33): mask %08lx\n", mask);
}
