/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the TCP addresses of connected and accepted sockets
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

#include <iphlpapi.h>
#include <mswsock.h>
#include <winuser.h>

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

/* A ConnectEx socket takes part in asynchronous notification only after
 * SO_UPDATE_CONNECT_CONTEXT: asking for the peer address first must not stand
 * in for that option */
static void Test_ConnectExOrder(void)
{
    GUID Guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX pConnectEx = NULL;
    SOCKET Server, Client, Accepted;
    SOCKADDR_IN Address;
    OVERLAPPED Overlapped;
    WSAEVENT Event;
    WSANETWORKEVENTS Events;
    fd_set Read;
    struct timeval Timeout = { 10, 0 };
    DWORD Bytes = 0;
    int Length, Result;
    USHORT ServerPort;

    Server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Server != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Server == INVALID_SOCKET)
        return;
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Server, (struct sockaddr *)&Address, sizeof(Address)) == 0, "bind failed: %d\n", WSAGetLastError());
    ok(listen(Server, 1) == 0, "listen failed: %d\n", WSAGetLastError());
    Length = sizeof(Address);
    ok(getsockname(Server, (struct sockaddr *)&Address, &Length) == 0, "getsockname failed: %d\n", WSAGetLastError());
    ServerPort = ntohs(Address.sin_port);

    Client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(Client != INVALID_SOCKET, "WSASocket failed: %d\n", WSAGetLastError());
    if (Client == INVALID_SOCKET)
    {
        closesocket(Server);
        return;
    }
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0, "client bind failed: %d\n", WSAGetLastError());
    Result = WSAIoctl(Client, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid), &pConnectEx, sizeof(pConnectEx), &Bytes, NULL, NULL);
    ok(Result == 0, "getting ConnectEx failed: %d\n", WSAGetLastError());
    if (Result != 0)
        goto Cleanup;

    Event = WSACreateEvent();
    ok(WSAEventSelect(Client, Event, FD_CONNECT | FD_WRITE) == 0, "WSAEventSelect failed: %d\n", WSAGetLastError());

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = WSACreateEvent();
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = htons(ServerPort);
    Result = pConnectEx(Client, (struct sockaddr *)&Address, sizeof(Address), NULL, 0, NULL, &Overlapped);
    ok(Result || WSAGetLastError() == ERROR_IO_PENDING, "ConnectEx failed: %d\n", WSAGetLastError());
    ok(WSAGetOverlappedResult(Client, &Overlapped, &Bytes, TRUE, &(DWORD){ 0 }), "ConnectEx did not complete: %d\n", WSAGetLastError());

    /* Windows refuses this before the option; what ReactOS answers is not the point */
    Length = sizeof(Address);
    getpeername(Client, (struct sockaddr *)&Address, &Length);

    ok(setsockopt(Client, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) == 0, "SO_UPDATE_CONNECT_CONTEXT failed: %d\n", WSAGetLastError());
    Length = sizeof(Address);
    memset(&Address, 0x55, sizeof(Address));
    Result = getpeername(Client, (struct sockaddr *)&Address, &Length);
    ok(Result == 0, "getpeername after the update failed: %d\n", WSAGetLastError());
    if (Result == 0)
        CheckAddress(&Address, Length, TRUE, ServerPort, "ConnectEx getpeername");

    Result = WSAWaitForMultipleEvents(1, &Event, FALSE, 5000, FALSE);
    ok(Result == WSA_WAIT_EVENT_0, "no network event after ConnectEx and the update: %d\n", Result);
    if (Result == WSA_WAIT_EVENT_0)
    {
        ZeroMemory(&Events, sizeof(Events));
        ok(WSAEnumNetworkEvents(Client, Event, &Events) == 0, "WSAEnumNetworkEvents failed: %d\n", WSAGetLastError());
        ok(Events.lNetworkEvents & (FD_CONNECT | FD_WRITE), "events 0x%lx\n", Events.lNetworkEvents);
    }
    WSAEventSelect(Client, NULL, 0);
    WSACloseEvent(Event);
    WSACloseEvent(Overlapped.hEvent);

    FD_ZERO(&Read);
    FD_SET(Server, &Read);
    if (select(0, &Read, NULL, NULL, &Timeout) == 1)
    {
        Accepted = accept(Server, NULL, NULL);
        ok(Accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
        if (Accepted != INVALID_SOCKET)
            closesocket(Accepted);
    }
Cleanup:
    closesocket(Client);
    closesocket(Server);
}

#define WM_SOCKET (WM_USER + 1)

static ULONG ConnectMessages;
static ULONG ConnectErrors;

/* Drain the queue for Milliseconds, or until StopAt FD_CONNECT messages came.
 * The wait is alertable, as in a program that also has overlapped I/O */
static void PumpMessages(DWORD Milliseconds, ULONG StopAt)
{
    DWORD End = GetTickCount() + Milliseconds;
    MSG Msg;

    do
    {
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            if (Msg.message == WM_SOCKET && WSAGETSELECTEVENT(Msg.lParam) == FD_CONNECT)
            {
                ConnectMessages++;
                if (WSAGETSELECTERROR(Msg.lParam))
                    ConnectErrors++;
            }
            DispatchMessageW(&Msg);
        }
        if (StopAt && ConnectMessages >= StopAt)
            return;
        SleepEx(10, TRUE);
    } while ((LONG)(End - GetTickCount()) > 0);
}

static void CheckConnectTime(SOCKET Socket, const char *What)
{
    DWORD Seconds = 0x55555555;
    int Length = sizeof(Seconds);

    ok(getsockopt(Socket, SOL_SOCKET, SO_CONNECT_TIME, (char *)&Seconds, &Length) == 0,
       "%s: SO_CONNECT_TIME failed: %d\n", What, WSAGetLastError());
    ok(Seconds != 0xFFFFFFFF && Seconds < 30, "%s: SO_CONNECT_TIME is %lu\n", What, Seconds);
}

static SOCKET Listener(PUSHORT Port, int Backlog, BOOL Conditional, const char *What)
{
    SOCKET Server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKADDR_IN Address;
    int Length = sizeof(Address);

    ok(Server != INVALID_SOCKET, "%s: socket failed: %d\n", What, WSAGetLastError());
    if (Server == INVALID_SOCKET)
        return INVALID_SOCKET;
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Server, (struct sockaddr *)&Address, sizeof(Address)) == 0, "%s: bind failed: %d\n", What, WSAGetLastError());
    if (Conditional)
    {
        BOOL On = TRUE;

        if (setsockopt(Server, SOL_SOCKET, SO_CONDITIONAL_ACCEPT, (const char *)&On, sizeof(On)) != 0)
        {
            skip("%s: SO_CONDITIONAL_ACCEPT failed: %d\n", What, WSAGetLastError());
            closesocket(Server);
            return INVALID_SOCKET;
        }
    }
    ok(listen(Server, Backlog) == 0, "%s: listen failed: %d\n", What, WSAGetLastError());
    ok(getsockname(Server, (struct sockaddr *)&Address, &Length) == 0, "%s: getsockname failed: %d\n", What, WSAGetLastError());
    *Port = ntohs(Address.sin_port);
    return Server;
}

static void AcceptAndClose(SOCKET Server)
{
    fd_set Read;
    struct timeval Timeout = { 10, 0 };

    FD_ZERO(&Read);
    FD_SET(Server, &Read);
    if (select(0, &Read, NULL, NULL, &Timeout) == 1)
    {
        SOCKET Accepted = accept(Server, NULL, NULL);

        ok(Accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
        if (Accepted != INVALID_SOCKET)
            closesocket(Accepted);
    }
}

/* SO_UPDATE_CONNECT_CONTEXT belongs to ConnectEx. On a socket that connect()
 * connected it must not post FD_CONNECT again, and it must not forget when the
 * connection was made. Adopt asks for the peer address before the first
 * message is read: the user-mode state of the socket then comes from AFD.
 * How many messages a delayed connection posts on its own differs between
 * systems, so the test counts the messages the option adds */
static void Test_UpdateAfterConnect(BOOL Adopt, const char *What)
{
    SOCKET Server, Client;
    SOCKADDR_IN Address;
    HWND Window;
    ULONG Before;
    USHORT ServerPort;
    int Length, Result;

    Window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    ok(Window != NULL, "%s: CreateWindowEx failed: %lu\n", What, GetLastError());
    Server = Listener(&ServerPort, 4, FALSE, What);
    if (!Window || Server == INVALID_SOCKET)
        goto Cleanup;

    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client != INVALID_SOCKET, "%s: socket failed: %d\n", What, WSAGetLastError());
    if (Client == INVALID_SOCKET)
        goto Cleanup;

    ConnectMessages = ConnectErrors = 0;
    ok(WSAAsyncSelect(Client, Window, WM_SOCKET, FD_CONNECT) == 0, "%s: WSAAsyncSelect failed: %d\n", What, WSAGetLastError());
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = htons(ServerPort);
    Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0 || WSAGetLastError() == WSAEWOULDBLOCK, "%s: connect returned %d, error %d\n", What, Result, WSAGetLastError());

    if (Adopt)
    {
        fd_set Write;
        struct timeval Timeout = { 10, 0 };

        Sleep(200);
        FD_ZERO(&Write);
        FD_SET(Client, &Write);
        ok(select(0, NULL, &Write, NULL, &Timeout) == 1, "%s: select did not report the connection\n", What);
        Length = sizeof(Address);
        ok(getpeername(Client, (struct sockaddr *)&Address, &Length) == 0, "%s: getpeername failed: %d\n", What, WSAGetLastError());
        CheckConnectTime(Client, What);
    }
    PumpMessages(10000, 1);
    PumpMessages(500, 0);
    Before = ConnectMessages;
    ok(Before >= 1, "%s: no FD_CONNECT message\n", What);
    trace("%s: %lu FD_CONNECT messages before the update\n", What, Before);

    Result = setsockopt(Client, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    trace("%s: SO_UPDATE_CONNECT_CONTEXT returned %d, error %d\n", What, Result, Result ? WSAGetLastError() : 0);
    PumpMessages(1000, 0);
    ok(ConnectMessages == Before, "%s: the update posted %lu more FD_CONNECT messages\n", What, ConnectMessages - Before);
    ok(ConnectErrors == 0, "%s: %lu FD_CONNECT messages with an error\n", What, ConnectErrors);
    CheckConnectTime(Client, What);

    WSAAsyncSelect(Client, Window, 0, 0);
    AcceptAndClose(Server);
    closesocket(Client);
Cleanup:
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    if (Window)
        DestroyWindow(Window);
}

/* Wait for a ConnectEx. When the event is set the result may still read
 * pending for a moment on ReactOS, so ask again for a short time. A ConnectEx
 * still pending after CancelAfter milliseconds is cancelled (0: never) */
static BOOL WaitForConnectEx(SOCKET Socket, LPWSAOVERLAPPED Overlapped, DWORD CancelAfter, const char *What)
{
    DWORD Bytes, Flags, Try;
    int Result;

    if (CancelAfter)
    {
        Result = WSAWaitForMultipleEvents(1, &Overlapped->hEvent, FALSE, CancelAfter, FALSE);
        if (Result == WSA_WAIT_TIMEOUT)
        {
            trace("%s: ConnectEx still pending, cancelling it\n", What);
            CancelIo((HANDLE)Socket);
        }
    }
    Result = WSAWaitForMultipleEvents(1, &Overlapped->hEvent, FALSE, 10000, FALSE);
    ok(Result == WSA_WAIT_EVENT_0, "%s: ConnectEx did not complete: %d\n", What, Result);
    if (Result != WSA_WAIT_EVENT_0)
        return FALSE;
    for (Try = 0; Try < 200; Try++)
    {
        if (WSAGetOverlappedResult(Socket, Overlapped, &Bytes, FALSE, &Flags))
            return TRUE;
        if (WSAGetLastError() != WSA_IO_INCOMPLETE && WSAGetLastError() != ERROR_IO_PENDING)
            break;
        Sleep(10);
    }
    return FALSE;
}

static void ConnectExAsyncSelect(BOOL PeerFirst, PULONG Connects, const char *What)
{
    GUID Guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX pConnectEx = NULL;
    SOCKET Server, Client = INVALID_SOCKET;
    SOCKADDR_IN Address;
    OVERLAPPED Overlapped;
    HWND Window;
    DWORD Bytes = 0;
    USHORT ServerPort;
    int Length, Result;

    Window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    ok(Window != NULL, "%s: CreateWindowEx failed: %lu\n", What, GetLastError());
    Server = Listener(&ServerPort, 4, FALSE, What);
    if (!Window || Server == INVALID_SOCKET)
        goto Cleanup;

    Client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(Client != INVALID_SOCKET, "%s: WSASocket failed: %d\n", What, WSAGetLastError());
    if (Client == INVALID_SOCKET)
        goto Cleanup;
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0, "%s: bind failed: %d\n", What, WSAGetLastError());
    Result = WSAIoctl(Client, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid), &pConnectEx, sizeof(pConnectEx), &Bytes, NULL, NULL);
    ok(Result == 0, "%s: getting ConnectEx failed: %d\n", What, WSAGetLastError());
    if (Result != 0)
        goto Cleanup;

    ConnectMessages = ConnectErrors = 0;
    ok(WSAAsyncSelect(Client, Window, WM_SOCKET, FD_CONNECT) == 0, "%s: WSAAsyncSelect failed: %d\n", What, WSAGetLastError());
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = WSACreateEvent();
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = htons(ServerPort);
    Result = pConnectEx(Client, (struct sockaddr *)&Address, sizeof(Address), NULL, 0, NULL, &Overlapped);
    ok(Result || WSAGetLastError() == ERROR_IO_PENDING, "%s: ConnectEx failed: %d\n", What, WSAGetLastError());
    Result = WaitForConnectEx(Client, &Overlapped, 0, What);
    WSACloseEvent(Overlapped.hEvent);
    if (!Result)
    {
        /* ReactOS: a ConnectEx sometimes stays pending although its event is set */
        skip("%s: ConnectEx did not finish: %d\n", What, WSAGetLastError());
        *Connects = (ULONG)-1;
        goto Cleanup;
    }

    if (PeerFirst)
    {
        /* Windows refuses this before the option; what ReactOS answers is not the point */
        Length = sizeof(Address);
        getpeername(Client, (struct sockaddr *)&Address, &Length);
    }
    ok(setsockopt(Client, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) == 0, "%s: SO_UPDATE_CONNECT_CONTEXT failed: %d\n", What, WSAGetLastError());
    Length = sizeof(Address);
    ok(getpeername(Client, (struct sockaddr *)&Address, &Length) == 0, "%s: getpeername after the update failed: %d\n", What, WSAGetLastError());
    PumpMessages(1500, 0);
    ok(ConnectErrors == 0, "%s: %lu FD_CONNECT messages with an error\n", What, ConnectErrors);
    CheckConnectTime(Client, What);
    *Connects = ConnectMessages;
    trace("%s: %lu FD_CONNECT messages\n", What, ConnectMessages);

    WSAAsyncSelect(Client, Window, 0, 0);
    AcceptAndClose(Server);
Cleanup:
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    if (Window)
        DestroyWindow(Window);
}

/* Asking for the peer address of a ConnectEx socket must not stand in for
 * SO_UPDATE_CONNECT_CONTEXT: the FD_CONNECT messages are the same with and
 * without it. How many a system posts (Windows none, ReactOS one) is not the
 * point, and neither is how often FD_WRITE comes */
static void Test_ConnectExAsyncSelect(void)
{
    ULONG Connects[2] = { (ULONG)-1, (ULONG)-1 };

    ConnectExAsyncSelect(FALSE, &Connects[0], "ConnectEx async");
    ConnectExAsyncSelect(TRUE, &Connects[1], "ConnectEx async, getpeername first");
    if (Connects[0] != (ULONG)-1 && Connects[1] != (ULONG)-1)
        ok(Connects[0] == Connects[1], "FD_CONNECT messages: %lu without, %lu with getpeername first\n", Connects[0], Connects[1]);
}

/* A ConnectEx that fails or is cancelled must not leave the socket marked: when connect()
 * later connects the same socket, SO_UPDATE_CONNECT_CONTEXT is a no-op and
 * posts no FD_CONNECT again */
static void Test_UpdateAfterFailedConnectEx(void)
{
    GUID Guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX pConnectEx = NULL;
    SOCKET Server, Dead, Client = INVALID_SOCKET;
    SOCKADDR_IN Address;
    OVERLAPPED Overlapped;
    HWND Window;
    DWORD Bytes = 0;
    ULONG Before;
    USHORT ServerPort, DeadPort;
    int Length, Result;
    const char *What = "update after a failed ConnectEx";

    Window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    ok(Window != NULL, "%s: CreateWindowEx failed: %lu\n", What, GetLastError());
    Server = Listener(&ServerPort, 4, FALSE, What);
    Dead = Listener(&DeadPort, 4, FALSE, What);
    if (!Window || Server == INVALID_SOCKET || Dead == INVALID_SOCKET)
        goto Cleanup;
    /* Nothing listens on this port any more */
    closesocket(Dead);
    Dead = INVALID_SOCKET;

    Client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(Client != INVALID_SOCKET, "%s: WSASocket failed: %d\n", What, WSAGetLastError());
    if (Client == INVALID_SOCKET)
        goto Cleanup;
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0, "%s: bind failed: %d\n", What, WSAGetLastError());
    Result = WSAIoctl(Client, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid), &pConnectEx, sizeof(pConnectEx), &Bytes, NULL, NULL);
    ok(Result == 0, "%s: getting ConnectEx failed: %d\n", What, WSAGetLastError());
    if (Result != 0)
        goto Cleanup;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = WSACreateEvent();
    Address.sin_port = htons(DeadPort);
    Result = pConnectEx(Client, (struct sockaddr *)&Address, sizeof(Address), NULL, 0, NULL, &Overlapped);
    ok(Result || WSAGetLastError() == ERROR_IO_PENDING, "%s: ConnectEx returned %d, error %d\n", What, Result, WSAGetLastError());
    ok(!WaitForConnectEx(Client, &Overlapped, 3000, What), "%s: ConnectEx to a closed port succeeded\n", What);
    WSACloseEvent(Overlapped.hEvent);

    ConnectMessages = ConnectErrors = 0;
    ok(WSAAsyncSelect(Client, Window, WM_SOCKET, FD_CONNECT) == 0, "%s: WSAAsyncSelect failed: %d\n", What, WSAGetLastError());
    Address.sin_port = htons(ServerPort);
    Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
    if (Result != 0 && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        skip("%s: connect on the failed socket returned error %d\n", What, WSAGetLastError());
        goto Cleanup;
    }
    PumpMessages(10000, 1);
    PumpMessages(500, 0);
    Before = ConnectMessages;
    Length = sizeof(Address);
    if (getpeername(Client, (struct sockaddr *)&Address, &Length) != 0)
    {
        /* ReactOS: a ConnectEx to a closed port never completes, and the
         * socket cannot connect after it was cancelled */
        skip("%s: the socket did not connect after the failed ConnectEx: %d\n", What, WSAGetLastError());
        goto Cleanup;
    }
    ok(Before >= 1, "%s: no FD_CONNECT message\n", What);

    Result = setsockopt(Client, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    trace("%s: SO_UPDATE_CONNECT_CONTEXT returned %d, error %d\n", What, Result, Result ? WSAGetLastError() : 0);
    PumpMessages(1000, 0);
    ok(ConnectMessages == Before, "%s: the update posted %lu more FD_CONNECT messages\n", What, ConnectMessages - Before);
    CheckConnectTime(Client, What);

    WSAAsyncSelect(Client, Window, 0, 0);
    AcceptAndClose(Server);
Cleanup:
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Dead != INVALID_SOCKET)
        closesocket(Dead);
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    if (Window)
        DestroyWindow(Window);
}

static int CALLBACK AcceptAll(LPWSABUF CallerId, LPWSABUF CallerData, LPQOS SQos, LPQOS GQos, LPWSABUF CalleeId, LPWSABUF CalleeData, GROUP *Group, DWORD_PTR Context)
{
    return CF_ACCEPT;
}

/* ConnectEx pending on a listener that holds the connection back (conditional accept), then (TryConnect)
 * connect() on the same socket: the connect is refused with Error and the
 * ConnectEx goes on. When it completes, the FD_CONNECT messages must be the
 * same as without the refused connect */
static void PendingConnectEx(BOOL TryConnect, PULONG Connects, PINT Error, const char *What)
{
    GUID Guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX pConnectEx = NULL;
    SOCKET Server, Client = INVALID_SOCKET;
    SOCKADDR_IN Address;
    OVERLAPPED Overlapped;
    HWND Window;
    DWORD Bytes = 0, Start;
    USHORT ServerPort;
    int Length, Result;

    Overlapped.hEvent = NULL;
    *Connects = (ULONG)-1;
    *Error = 0;
    Window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    ok(Window != NULL, "%s: CreateWindowEx failed: %lu\n", What, GetLastError());
    Server = Listener(&ServerPort, 4, TRUE, What);
    if (!Window || Server == INVALID_SOCKET)
        goto Cleanup;

    Client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(Client != INVALID_SOCKET, "%s: WSASocket failed: %d\n", What, WSAGetLastError());
    if (Client == INVALID_SOCKET)
        goto Cleanup;
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0, "%s: bind failed: %d\n", What, WSAGetLastError());
    Result = WSAIoctl(Client, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid), &pConnectEx, sizeof(pConnectEx), &Bytes, NULL, NULL);
    ok(Result == 0, "%s: getting ConnectEx failed: %d\n", What, WSAGetLastError());
    if (Result != 0)
        goto Cleanup;

    ConnectMessages = ConnectErrors = 0;
    ok(WSAAsyncSelect(Client, Window, WM_SOCKET, FD_CONNECT) == 0, "%s: WSAAsyncSelect failed: %d\n", What, WSAGetLastError());
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = WSACreateEvent();
    Address.sin_port = htons(ServerPort);
    Result = pConnectEx(Client, (struct sockaddr *)&Address, sizeof(Address), NULL, 0, NULL, &Overlapped);
    ok(Result || WSAGetLastError() == ERROR_IO_PENDING, "%s: ConnectEx returned %d, error %d\n", What, Result, WSAGetLastError());
    if (WSAWaitForMultipleEvents(1, &Overlapped.hEvent, FALSE, 1500, FALSE) == WSA_WAIT_EVENT_0)
    {
        skip("%s: the ConnectEx did not stay pending\n", What);
        goto Cleanup;
    }

    if (TryConnect)
    {
        Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
        *Error = Result == 0 ? 0 : WSAGetLastError();
        trace("%s: connect during the ConnectEx returned %d, error %d\n", What, Result, *Error);
    }

    /* Let the ConnectEx finish: take every connection off the listener */
    Start = GetTickCount();
    while (WSAWaitForMultipleEvents(1, &Overlapped.hEvent, FALSE, 0, FALSE) != WSA_WAIT_EVENT_0 && GetTickCount() - Start < 40000)
    {
        fd_set Read;
        struct timeval Timeout = { 1, 0 };

        FD_ZERO(&Read);
        FD_SET(Server, &Read);
        if (select(0, &Read, NULL, NULL, &Timeout) == 1)
        {
            SOCKET Accepted = WSAAccept(Server, NULL, NULL, AcceptAll, 0);

            if (Accepted != INVALID_SOCKET)
                closesocket(Accepted);
        }
    }
    if (!WaitForConnectEx(Client, &Overlapped, 0, What))
    {
        skip("%s: the pending ConnectEx did not finish: %d\n", What, WSAGetLastError());
        goto Cleanup;
    }
    Length = sizeof(Address);
    getpeername(Client, (struct sockaddr *)&Address, &Length);
    ok(setsockopt(Client, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) == 0, "%s: SO_UPDATE_CONNECT_CONTEXT failed: %d\n", What, WSAGetLastError());
    Length = sizeof(Address);
    ok(getpeername(Client, (struct sockaddr *)&Address, &Length) == 0, "%s: getpeername after the update failed: %d\n", What, WSAGetLastError());
    PumpMessages(1500, 0);
    *Connects = ConnectMessages;
    trace("%s: %lu FD_CONNECT messages\n", What, ConnectMessages);
    WSAAsyncSelect(Client, Window, 0, 0);

Cleanup:
    if (Overlapped.hEvent)
        WSACloseEvent(Overlapped.hEvent);
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    if (Window)
        DestroyWindow(Window);
}

/* A connect() on a socket with a ConnectEx in flight is refused and must not
 * take the ConnectEx's mark: the update still re-arms what it re-armed
 * without that connect() */
static void Test_ConnectDuringConnectEx(void)
{
    ULONG Connects[2];
    int Error[2];

    PendingConnectEx(FALSE, &Connects[0], &Error[0], "ConnectEx pending");
    PendingConnectEx(TRUE, &Connects[1], &Error[1], "ConnectEx pending, connect refused");
    if (Connects[0] != (ULONG)-1 && Connects[1] != (ULONG)-1)
        ok(Connects[0] == Connects[1], "FD_CONNECT messages: %lu without, %lu with a connect during the ConnectEx\n", Connects[0], Connects[1]);
    if (Connects[1] != (ULONG)-1)
        ok(Error[1] == WSAEINVAL, "connect during a pending ConnectEx: error %d, expected %d\n", Error[1], WSAEINVAL);
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
    Test_ConnectExOrder();
    Test_UpdateAfterConnect(FALSE, "update after connect");
    Test_UpdateAfterConnect(TRUE, "update after getpeername");
    Test_ConnectExAsyncSelect();
    Test_UpdateAfterFailedConnectEx();
    Test_ConnectDuringConnectEx();

    WSACleanup();
}
