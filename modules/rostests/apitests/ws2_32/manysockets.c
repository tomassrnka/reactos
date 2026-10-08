/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for many bound sockets at once
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <iphlpapi.h>

#define SOCKET_COUNT 5000  /* above MAX_TDI_ENTITIES (4096) so the entity list must grow */

static SOCKET Sockets[SOCKET_COUNT];

static
ULONG
CountBoundUdpPorts(VOID)
{
    PMIB_UDPTABLE Table;
    DWORD Size = 0, i, Count = 0;

    if (GetUdpTable(NULL, &Size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return 0;
    Table = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Table)
        return 0;
    if (GetUdpTable(Table, &Size, FALSE) == NO_ERROR)
    {
        for (i = 0; i < Table->dwNumEntries; i++)
        {
            if (Table->table[i].dwLocalAddr == htonl(INADDR_LOOPBACK))
                Count++;
        }
    }
    HeapFree(GetProcessHeap(), 0, Table);
    return Count;
}

static
VOID
TestManySockets(ULONG Round)
{
    struct sockaddr_in Addr;
    ULONG i, Opened = 0, Failed = 0, Listed;
    INT Error = 0;

    for (i = 0; i < SOCKET_COUNT; i++)
    {
        Sockets[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (Sockets[i] == INVALID_SOCKET)
        {
            Error = WSAGetLastError();
            Failed++;
            continue;
        }
        ZeroMemory(&Addr, sizeof(Addr));
        Addr.sin_family = AF_INET;
        Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(Sockets[i], (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR)
        {
            Error = WSAGetLastError();
            closesocket(Sockets[i]);
            Sockets[i] = INVALID_SOCKET;
            Failed++;
            continue;
        }
        Opened++;
    }
    ok(Failed == 0, "Round %lu: %lu of %u sockets failed to bind, last error %d\n", Round, Failed, SOCKET_COUNT, Error);

    /* Binding more entities than the old entity list could hold is the point of the test; the
     * kernel must not fault or corrupt pool. The UDP table listing is informational only
     * (GetUdpTable does not enumerate every bound socket on ReactOS). Run with special pool on
     * tag 'EidT' to turn the old overflow into a deterministic guard-page bugcheck. */
    Listed = CountBoundUdpPorts();
    trace("Round %lu: %lu loopback UDP endpoints listed, %lu bound\n", Round, Listed, Opened);

    for (i = 0; i < SOCKET_COUNT; i++)
    {
        if (Sockets[i] != INVALID_SOCKET)
            closesocket(Sockets[i]);
        Sockets[i] = INVALID_SOCKET;
    }
}

START_TEST(manysockets)
{
    WSADATA WsaData;
    ULONG Round;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");

    for (Round = 0; Round < 3; Round++)
        TestManySockets(Round);

    WSACleanup();
}
