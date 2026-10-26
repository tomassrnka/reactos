/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for many bound sockets at once
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <iphlpapi.h>

/* Above MAX_TDI_ENTITIES (4096), so the entity list must grow where resources allow */
#define SOCKET_COUNT 5000
/* Past the old overflow point on both x86 (2048 entries) and x64 (1365 entries) */
#define MIN_BOUND 2100

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
    ULONG i, Opened = 0, Listed;
    INT Error = 0;

    for (i = 0; i < SOCKET_COUNT; i++)
        Sockets[i] = INVALID_SOCKET;

    for (i = 0; i < SOCKET_COUNT; i++)
    {
        Sockets[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (Sockets[i] == INVALID_SOCKET)
        {
            Error = WSAGetLastError();
            break;
        }
        ZeroMemory(&Addr, sizeof(Addr));
        Addr.sin_family = AF_INET;
        Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(Sockets[i], (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR)
        {
            Error = WSAGetLastError();
            closesocket(Sockets[i]);
            Sockets[i] = INVALID_SOCKET;
            break;
        }
        Opened++;
    }
    /* Running out of resources ends the round; any other error is a failure */
    ok(Opened == SOCKET_COUNT || Error == WSAENOBUFS,
       "Round %lu: stopped after %lu sockets with error %d\n", Round, Opened, Error);
    ok(Opened >= MIN_BOUND, "Round %lu: only %lu sockets bound, error %d\n", Round, Opened, Error);
    if (Opened < SOCKET_COUNT)
        trace("Round %lu: out of resources after %lu sockets\n", Round, Opened);

    /* The UDP table listing is informational: GetUdpTable does not list every bound socket */
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
