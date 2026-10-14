/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the TCP addresses of connected and accepted sockets
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

#include <iphlpapi.h>

/* Ports whose two bytes differ, so a byte-order mistake shows */
static const USHORT TestPorts[] = { 0x1234, 0x2345, 0x3456, 0x4567, 0x5678, 0x6789, 0x789a, 0x89ab };

/* Bind to the next free test port after *Next */
static BOOL BindLoopback(SOCKET Socket, PULONG Next, PUSHORT Port)
{
    SOCKADDR_IN Address;
    ULONG i;

    for (i = 0; i < _countof(TestPorts); i++)
    {
        *Port = TestPorts[(*Next)++ % _countof(TestPorts)];
        ZeroMemory(&Address, sizeof(Address));
        Address.sin_family = AF_INET;
        Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Address.sin_port = htons(*Port);
        if (bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) == 0)
            return TRUE;
    }
    return FALSE;
}

static void CheckAddress(const SOCKADDR_IN *Address, int Length, BOOL CheckIp, USHORT Port, const char *What)
{
    ok(Length == sizeof(*Address), "%s: length %d\n", What, Length);
    ok(Address->sin_family == AF_INET, "%s: family %u\n", What, Address->sin_family);
    if (CheckIp)
        ok(Address->sin_addr.s_addr == htonl(INADDR_LOOPBACK), "%s: address 0x%08lx\n",
           What, (ULONG)ntohl(Address->sin_addr.s_addr));
    ok(ntohs(Address->sin_port) == Port, "%s: port 0x%04x, expected 0x%04x\n",
       What, ntohs(Address->sin_port), Port);
}

static void GetName(SOCKET Socket, BOOL Peer, BOOL CheckIp, USHORT Port, const char *What)
{
    SOCKADDR_IN Address;
    int Length = sizeof(Address);
    int Result;

    memset(&Address, 0x55, sizeof(Address));
    if (Peer)
        Result = getpeername(Socket, (struct sockaddr *)&Address, &Length);
    else
        Result = getsockname(Socket, (struct sockaddr *)&Address, &Length);
    ok(Result == 0, "%s failed: %d\n", What, WSAGetLastError());
    if (Result == 0)
        CheckAddress(&Address, Length, CheckIp, Port, What);
}

/* The TCP table must list both ends of the connection with the same ports */
static void CheckTcpTable(USHORT LocalPort, USHORT RemotePort, const char *What)
{
    PMIB_TCPTABLE Table = NULL;
    ULONG Size = 0, i;
    DWORD Error;
    BOOL Found = FALSE;

    /* ReactOS's iphlpapi reads the TCP rows of an address file only when
     * its entity instance matches an ARP interface, so whether a row shows
     * depends on instance numbering there */
    if (is_reactos())
    {
        skip("%s: the TCP table is incomplete on ReactOS\n", What);
        return;
    }

    /* Connections may open between the size query and the copy */
    for (;;)
    {
        Error = GetTcpTable(Table, &Size, FALSE);
        if (Error != ERROR_INSUFFICIENT_BUFFER)
            break;
        HeapFree(GetProcessHeap(), 0, Table);
        Size += 16 * sizeof(MIB_TCPROW);
        Table = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Table)
        {
            skip("%s: no memory for the TCP table\n", What);
            return;
        }
    }
    ok(Error == NO_ERROR, "%s: GetTcpTable returned %lu\n", What, Error);
    if (Error == NO_ERROR && Table)
    {
        for (i = 0; i < Table->dwNumEntries; i++)
        {
            if (Table->table[i].dwLocalAddr == htonl(INADDR_LOOPBACK) &&
                (USHORT)Table->table[i].dwLocalPort == htons(LocalPort) &&
                (USHORT)Table->table[i].dwRemotePort == htons(RemotePort) &&
                Table->table[i].dwState == MIB_TCP_STATE_ESTAB)
            {
                Found = TRUE;
                break;
            }
        }
    }
    ok(Found, "%s: no established row for local port 0x%04x, remote port 0x%04x\n", What, LocalPort, RemotePort);
    HeapFree(GetProcessHeap(), 0, Table);
}

static void Test_TcpAddresses(BOOL BindClient, BOOL ListenAny)
{
    SOCKET Server, Client = INVALID_SOCKET, Accepted;
    SOCKADDR_IN Address;
    int Length, Error = 0;
    ULONG Next, Try;
    USHORT ServerPort, ClientPort = 0;

    /* Start at a different port in each run: a connection of an earlier
     * run between the same two ports may still be in TIME_WAIT */
    Next = GetTickCount() + GetCurrentProcessId();

    Server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Server != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Server == INVALID_SOCKET)
        return;

    if (ListenAny)
    {
        /* Listen on every address with a port from the system: the
         * port getsockname reports must be the one that takes connections */
        ZeroMemory(&Address, sizeof(Address));
        Address.sin_family = AF_INET;
        ok(bind(Server, (struct sockaddr *)&Address, sizeof(Address)) == 0, "bind failed: %d\n", WSAGetLastError());
        ok(listen(Server, 1) == 0, "listen failed: %d\n", WSAGetLastError());
        Length = sizeof(Address);
        ok(getsockname(Server, (struct sockaddr *)&Address, &Length) == 0,
           "listening getsockname failed: %d\n", WSAGetLastError());
        ServerPort = ntohs(Address.sin_port);
        trace("The system gave the listening socket port 0x%04x\n", ServerPort);
    }
    else
    {
        if (!BindLoopback(Server, &Next, &ServerPort))
        {
            skip("No test port free for the server: %d\n", WSAGetLastError());
            goto Cleanup;
        }
        ok(listen(Server, 1) == 0, "listen failed: %d\n", WSAGetLastError());
        GetName(Server, FALSE, TRUE, ServerPort, "listening getsockname");
    }

    for (Try = 0; Try < _countof(TestPorts); Try++)
    {
        Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(Client != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
        if (Client == INVALID_SOCKET)
            goto Cleanup;

        if (BindClient)
        {
            if (!BindLoopback(Client, &Next, &ClientPort))
            {
                skip("No test port free for the client: %d\n", WSAGetLastError());
                goto Cleanup;
            }
            GetName(Client, FALSE, TRUE, ClientPort, "bound client getsockname");
        }

        ZeroMemory(&Address, sizeof(Address));
        Address.sin_family = AF_INET;
        Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Address.sin_port = htons(ServerPort);
        if (connect(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0)
            break;
        Error = WSAGetLastError();
        closesocket(Client);
        Client = INVALID_SOCKET;
        if (Error != WSAEADDRINUSE || !BindClient)
            break;
    }
    ok(Client != INVALID_SOCKET, "connect failed: %d\n", Error);
    if (Client == INVALID_SOCKET)
        goto Cleanup;

    if (!BindClient)
    {
        /* The client got a port from the system: learn it from the client side */
        Length = sizeof(Address);
        ok(getsockname(Client, (struct sockaddr *)&Address, &Length) == 0,
           "client getsockname failed: %d\n", WSAGetLastError());
        ClientPort = ntohs(Address.sin_port);
        trace("The system gave the client port 0x%04x\n", ClientPort);
    }

    Length = sizeof(Address);
    memset(&Address, 0x55, sizeof(Address));
    Accepted = accept(Server, (struct sockaddr *)&Address, &Length);
    ok(Accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    if (Accepted == INVALID_SOCKET)
        goto Cleanup;
    CheckAddress(&Address, Length, TRUE, ClientPort, "accept");

    GetName(Accepted, TRUE, TRUE, ClientPort, "accepted getpeername");
    /* The local address of a socket accepted on a listener bound to every
     * address is not the point here: check only its port then */
    GetName(Accepted, FALSE, !ListenAny, ServerPort, "accepted getsockname");
    GetName(Client, TRUE, TRUE, ServerPort, "client getpeername");
    if (BindClient)
        GetName(Client, FALSE, TRUE, ClientPort, "connected client getsockname");
    CheckTcpTable(ClientPort, ServerPort, "client row");
    CheckTcpTable(ServerPort, ClientPort, "accepted row");

    closesocket(Accepted);
Cleanup:
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    closesocket(Server);
}

/* A nonblocking connect that select or WSAEventSelect reported as finished
 * must leave a connected socket: getpeername and getsockname succeed at once.
 * PreDelay lets the connection complete before the application waits, the way
 * a busy event loop sees it */
static void Test_NonBlockingConnect(BOOL UseEvents, DWORD PreDelay, const char *What)
{
    SOCKET Server, Client, Accepted;
    SOCKADDR_IN Address;
    int Length, Result, Iteration;
    USHORT ServerPort;
    ULONG NonBlocking = 1;

    Server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Server != INVALID_SOCKET, "%s: socket failed: %d\n", What, WSAGetLastError());
    if (Server == INVALID_SOCKET)
        return;
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Server, (struct sockaddr *)&Address, sizeof(Address)) == 0, "%s: bind failed: %d\n", What, WSAGetLastError());
    ok(listen(Server, 8) == 0, "%s: listen failed: %d\n", What, WSAGetLastError());
    Length = sizeof(Address);
    ok(getsockname(Server, (struct sockaddr *)&Address, &Length) == 0, "%s: getsockname failed: %d\n", What, WSAGetLastError());
    ServerPort = ntohs(Address.sin_port);

    for (Iteration = 0; Iteration < 25; Iteration++)
    {
        BOOL Ready = FALSE;

        Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(Client != INVALID_SOCKET, "%s: socket failed: %d\n", What, WSAGetLastError());
        if (Client == INVALID_SOCKET)
            break;
        ok(ioctlsocket(Client, FIONBIO, &NonBlocking) == 0, "%s: FIONBIO failed: %d\n", What, WSAGetLastError());

        ZeroMemory(&Address, sizeof(Address));
        Address.sin_family = AF_INET;
        Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Address.sin_port = htons(ServerPort);
        Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
        ok(Result == 0 || WSAGetLastError() == WSAEWOULDBLOCK, "%s: connect returned %d, error %d\n", What, Result, WSAGetLastError());

        if (PreDelay)
            Sleep(PreDelay);

        if (UseEvents)
        {
            WSAEVENT Event = WSACreateEvent();
            WSANETWORKEVENTS Events;

            ok(WSAEventSelect(Client, Event, FD_CONNECT) == 0, "%s: WSAEventSelect failed: %d\n", What, WSAGetLastError());
            Result = WSAWaitForMultipleEvents(1, &Event, FALSE, 10000, FALSE);
            ok(Result == WSA_WAIT_EVENT_0, "%s: wait returned %d\n", What, Result);
            if (Result == WSA_WAIT_EVENT_0)
            {
                ZeroMemory(&Events, sizeof(Events));
                ok(WSAEnumNetworkEvents(Client, Event, &Events) == 0, "%s: WSAEnumNetworkEvents failed: %d\n", What, WSAGetLastError());
                ok(Events.lNetworkEvents & FD_CONNECT, "%s: no FD_CONNECT, events 0x%lx\n", What, Events.lNetworkEvents);
                ok(Events.iErrorCode[FD_CONNECT_BIT] == 0, "%s: FD_CONNECT error %d\n", What, Events.iErrorCode[FD_CONNECT_BIT]);
                Ready = (Events.lNetworkEvents & FD_CONNECT) && Events.iErrorCode[FD_CONNECT_BIT] == 0;
            }
            WSAEventSelect(Client, NULL, 0);
            WSACloseEvent(Event);
        }
        else
        {
            fd_set Write, Except;
            struct timeval Timeout = { 10, 0 };

            FD_ZERO(&Write);
            FD_ZERO(&Except);
            FD_SET(Client, &Write);
            FD_SET(Client, &Except);
            Result = select(0, NULL, &Write, &Except, &Timeout);
            ok(Result == 1, "%s: select returned %d\n", What, Result);
            ok(FD_ISSET(Client, &Write), "%s: not writable after connect\n", What);
            Ready = FD_ISSET(Client, &Write) != 0;
        }

        if (Ready)
        {
            char Byte = 'x';

            Length = sizeof(Address);
            memset(&Address, 0x55, sizeof(Address));
            Result = getpeername(Client, (struct sockaddr *)&Address, &Length);
            ok(Result == 0, "%s %d: getpeername failed: %d\n", What, Iteration, WSAGetLastError());
            if (Result == 0)
                CheckAddress(&Address, Length, TRUE, ServerPort, What);

            Length = sizeof(Address);
            Result = getsockname(Client, (struct sockaddr *)&Address, &Length);
            ok(Result == 0, "%s %d: getsockname failed: %d\n", What, Iteration, WSAGetLastError());

            Result = send(Client, &Byte, 1, 0);
            ok(Result == 1, "%s %d: send returned %d, error %d\n", What, Iteration, Result, WSAGetLastError());
        }

        if (Ready)
        {
            fd_set Read;
            struct timeval Timeout = { 10, 0 };

            /* Only a connection that was reported finished is queued for sure */
            FD_ZERO(&Read);
            FD_SET(Server, &Read);
            Result = select(0, &Read, NULL, NULL, &Timeout);
            ok(Result == 1, "%s: no connection to accept, select returned %d\n", What, Result);
            if (Result == 1)
            {
                Accepted = accept(Server, NULL, NULL);
                ok(Accepted != INVALID_SOCKET, "%s: accept failed: %d\n", What, WSAGetLastError());
                if (Accepted != INVALID_SOCKET)
                    closesocket(Accepted);
            }
        }
        closesocket(Client);
    }
    closesocket(Server);
}

START_TEST(getpeername)
{
    WSADATA WsaData;
    int Result;

    Result = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Result == 0, "WSAStartup failed: %d\n", Result);
    if (Result != 0)
        return;

    Test_TcpAddresses(TRUE, FALSE);
    Test_TcpAddresses(FALSE, FALSE);
    Test_TcpAddresses(FALSE, TRUE);
    Test_NonBlockingConnect(FALSE, 0, "select");
    Test_NonBlockingConnect(FALSE, 100, "select after delay");
    Test_NonBlockingConnect(TRUE, 0, "event");
    Test_NonBlockingConnect(TRUE, 100, "event after delay");

    WSACleanup();
}
