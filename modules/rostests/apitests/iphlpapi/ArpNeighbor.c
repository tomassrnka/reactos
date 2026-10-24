/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ARP / neighbor table length contract (T119): valid link address
 *              lengths round-trip, out-of-range lengths are refused
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winsock2.h>
#include <iphlpapi.h>

static PMIB_IPNETTABLE GetNetTable(void)
{
    PMIB_IPNETTABLE Table = NULL;
    ULONG Size = 0, Tries;
    DWORD Error = ERROR_INSUFFICIENT_BUFFER;

    for (Tries = 0; Tries < 5 && Error == ERROR_INSUFFICIENT_BUFFER; Tries++)
    {
        free(Table);
        Table = Size ? malloc(Size) : NULL;
        Error = GetIpNetTable(Table, &Size, FALSE);
    }
    if (Error != NO_ERROR)
    {
        /* an empty table is reported as ERROR_NO_DATA */
        ok(Error == ERROR_NO_DATA, "GetIpNetTable: %lu\n", Error);
        free(Table);
        return NULL;
    }
    return Table;
}

static void CheckEntryLengths(PMIB_IPNETTABLE Table)
{
    DWORD i;

    for (i = 0; i < Table->dwNumEntries; i++)
    {
        PMIB_IPNETROW Row = &Table->table[i];

        ok(Row->dwPhysAddrLen <= MAXLEN_PHYSADDR, "entry %lu: dwPhysAddrLen %lu exceeds %u\n", i, Row->dwPhysAddrLen,
           MAXLEN_PHYSADDR);
    }
}

static void TestResolveAndTable(void)
{
    PIP_ADAPTER_INFO Adapters = NULL, A;
    PMIB_IPNETTABLE Table;
    ULONG Size = 0, Hw[2], HwLen;
    DWORD Error, i;
    IPAddr Source = 0, Gateway = 0;
    BOOL Found = FALSE;

    Error = GetAdaptersInfo(NULL, &Size);
    if (Error != ERROR_BUFFER_OVERFLOW || !(Adapters = malloc(Size)) || GetAdaptersInfo(Adapters, &Size) != NO_ERROR)
    {
        skip("no adapter information\n");
        free(Adapters);
        return;
    }

    for (A = Adapters; A; A = A->Next)
    {
        Source = inet_addr(A->IpAddressList.IpAddress.String);
        Gateway = inet_addr(A->GatewayList.IpAddress.String);
        if (Source && Source != INADDR_NONE && Gateway && Gateway != INADDR_NONE)
            break;
        Source = Gateway = 0;
    }
    free(Adapters);
    if (!Gateway)
    {
        skip("no adapter with a gateway\n");
        return;
    }

    /* A normal ARP exchange resolves, with a 6 byte Ethernet address */
    memset(Hw, 0, sizeof(Hw));
    HwLen = 6;
    Error = SendARP(Gateway, Source, Hw, &HwLen);
    ok(Error == NO_ERROR, "SendARP: %lu\n", Error);
    ok(HwLen == 6, "SendARP length %lu\n", HwLen);

    /* ... and the neighbor table query reports it, within the entry size */
    Table = GetNetTable();
    ok(Table != NULL, "no neighbor table after a successful ARP exchange\n");
    if (!Table)
        return;
    CheckEntryLengths(Table);
    for (i = 0; i < Table->dwNumEntries; i++)
    {
        if (Table->table[i].dwAddr == Gateway)
        {
            Found = TRUE;
            ok(Table->table[i].dwPhysAddrLen == 6, "gateway entry length %lu\n", Table->table[i].dwPhysAddrLen);
            ok(!memcmp(Table->table[i].bPhysAddr, Hw, 6), "gateway entry differs from the SendARP answer\n");
        }
    }
    ok(Found, "gateway missing from the neighbor table\n");

    /* repeated queries stay self-consistent */
    for (i = 0; i < 20; i++)
    {
        PMIB_IPNETTABLE Again = GetNetTable();

        if (Again)
        {
            CheckEntryLengths(Again);
            free(Again);
        }
    }
    free(Table);
}

static void TestSetEntryLengths(void)
{
    PMIB_IPNETTABLE Table = GetNetTable();
    MIB_IPNETROW Row;
    DWORD Error;

    if (!Table || !Table->dwNumEntries)
    {
        skip("no neighbor to take an interface from\n");
        free(Table);
        return;
    }

    memset(&Row, 0, sizeof(Row));
    Row.dwIndex = Table->table[0].dwIndex;
    Row.dwAddr = inet_addr("169.254.119.119");
    Row.dwType = MIB_IPNET_TYPE_STATIC;
    free(Table);

    /* valid length: accepted, listed, removable */
    Row.dwPhysAddrLen = 6;
    memcpy(Row.bPhysAddr, "\x02\x00\x00\x01\x19\x77", 6);
    Error = SetIpNetEntry(&Row);
    if (Error == ERROR_ACCESS_DENIED || Error == ERROR_NOT_SUPPORTED)
    {
        skip("SetIpNetEntry not permitted: %lu\n", Error);
        return;
    }
    ok(Error == NO_ERROR, "SetIpNetEntry (6 byte address): %lu\n", Error);
    if (Error == NO_ERROR)
    {
        DWORD i;
        BOOL Seen = FALSE;

        Table = GetNetTable();
        if (Table)
        {
            CheckEntryLengths(Table);
            for (i = 0; i < Table->dwNumEntries; i++)
                if (Table->table[i].dwAddr == Row.dwAddr)
                {
                    Seen = TRUE;
                    ok(Table->table[i].dwPhysAddrLen == 6, "static entry length %lu\n", Table->table[i].dwPhysAddrLen);
                }
            free(Table);
        }
        ok(Seen, "static entry missing from the neighbor table\n");
        Error = DeleteIpNetEntry(&Row);
        ok(Error == NO_ERROR, "DeleteIpNetEntry: %lu\n", Error);
    }

    /* invalid length (larger than a table entry's address field): refused,
       and nothing is added */
    Row.dwPhysAddrLen = MAXLEN_PHYSADDR + 1;
    Error = SetIpNetEntry(&Row);
    ok(Error != NO_ERROR, "SetIpNetEntry accepted a %u byte address\n", MAXLEN_PHYSADDR + 1);
    if (Error == NO_ERROR)
        DeleteIpNetEntry(&Row);
}

START_TEST(ArpNeighbor)
{
    TestResolveAndTable();
    TestSetEntryLengths();
}
