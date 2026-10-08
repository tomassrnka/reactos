/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the listen backlog
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "ws2_32.h"
#include <mswsock.h>

#define MAX_CLIENTS 300

/* Windows Server 2008 R2 and Windows 10 queue at most 200 connections for
 * a larger backlog, SOMAXCONN included */
#define MAX_BACKLOG 200

enum { CLIENT_PENDING, CLIENT_CONNECTED, CLIENT_FAILED };

/* Connect one more client and accept it on the nonblocking listener */
static BOOL
ConnectAndAccept(SOCKET Server, const struct sockaddr_in *Addr)
{
    SOCKET Client, Accepted = INVALID_SOCKET;
    DWORD Start;

    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Client == INVALID_SOCKET)
        return FALSE;
    if (connect(Client, (const struct sockaddr *)Addr, sizeof(*Addr)) == 0)
    {
        Start = GetTickCount();
        do
        {
            Accepted = accept(Server, NULL, NULL);
            if (Accepted != INVALID_SOCKET || WSAGetLastError() != WSAEWOULDBLOCK)
                break;
            Sleep(50);
        } while (GetTickCount() - Start < 5000);
    }
    closesocket(Client);
    if (Accepted == INVALID_SOCKET)
        return FALSE;
    closesocket(Accepted);
    return TRUE;
}

/* Connect Count nonblocking clients to a listener that does not accept while
 * they connect, then count the connections that are still alive and the
 * ones the listener accepts afterwards. Connections beyond the backlog are
 * refused or reset. Windows 10 sometimes queues more than a small backlog
 * (up to 25 for a backlog of 5 in our runs), so only ReactOS is held to the
 * exact limit. */
static void
TestBacklog(int Backlog, int Count)
{
    SOCKET Server, Accepted, Clients[MAX_CLIENTS];
    int State[MAX_CLIENTS];
    struct sockaddr_in Addr;
    int AddrLen = sizeof(Addr);
    u_long NonBlocking = 1;
    int i, Error, Alive = 0, Failed = 0, Pending, AcceptCount = 0;
    int Limit = (Backlog > MAX_BACKLOG) ? MAX_BACKLOG : Backlog;
    DWORD Start;
    char Byte;
    fd_set WriteSet, ExceptSet;
    struct timeval Zero = { 0, 0 };

    ok(Count <= MAX_CLIENTS, "Count %d\n", Count);
    if (Count > MAX_CLIENTS)
        return;

    Server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Server != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Server == INVALID_SOCKET)
        return;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Error = bind(Server, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Error == 0, "bind failed: %d\n", WSAGetLastError());
    Error = getsockname(Server, (struct sockaddr *)&Addr, &AddrLen);
    ok(Error == 0, "getsockname failed: %d\n", WSAGetLastError());
    Error = listen(Server, Backlog);
    ok(Error == 0, "listen failed: %d\n", WSAGetLastError());
    Error = ioctlsocket(Server, FIONBIO, &NonBlocking);
    ok(Error == 0, "ioctlsocket failed: %d\n", WSAGetLastError());

    for (i = 0; i < Count; i++)
    {
        State[i] = CLIENT_FAILED;
        Clients[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Clients[i] == INVALID_SOCKET)
            continue;
        ioctlsocket(Clients[i], FIONBIO, &NonBlocking);
        Error = connect(Clients[i], (struct sockaddr *)&Addr, sizeof(Addr));
        if (Error == 0)
            State[i] = CLIENT_CONNECTED;
        else if (WSAGetLastError() == WSAEWOULDBLOCK)
            State[i] = CLIENT_PENDING;
    }

    /* A refused connect may be retried for a few seconds */
    Start = GetTickCount();
    do
    {
        Pending = 0;
        for (i = 0; i < Count; i++)
        {
            if (State[i] != CLIENT_PENDING)
                continue;
            FD_ZERO(&WriteSet);
            FD_ZERO(&ExceptSet);
            FD_SET(Clients[i], &WriteSet);
            FD_SET(Clients[i], &ExceptSet);
            if (select(0, NULL, &WriteSet, &ExceptSet, &Zero) > 0)
                State[i] = FD_ISSET(Clients[i], &ExceptSet) ? CLIENT_FAILED : CLIENT_CONNECTED;
            else
                Pending++;
        }
        if (Pending)
            Sleep(100);
    } while (Pending && GetTickCount() - Start < 30000);

    /* A connection the listener had no room for may also be reset */
    Sleep(1000);
    for (i = 0; i < Count; i++)
    {
        if (State[i] == CLIENT_CONNECTED &&
            recv(Clients[i], &Byte, 1, 0) == SOCKET_ERROR &&
            WSAGetLastError() == WSAEWOULDBLOCK)
        {
            Alive++;
        }
        else if (State[i] != CLIENT_PENDING)
        {
            Failed++;
        }
    }

    for (;;)
    {
        Accepted = accept(Server, NULL, NULL);
        if (Accepted == INVALID_SOCKET)
            break;
        AcceptCount++;
        closesocket(Accepted);
    }
    ok(WSAGetLastError() == WSAEWOULDBLOCK, "accept failed: %d\n", WSAGetLastError());

    trace("Backlog %d, %d clients: %d alive, %d failed, %d pending, %d accepted\n",
          Backlog, Count, Alive, Failed, Pending, AcceptCount);
    ok(Pending == 0, "Backlog %d: %d connects still pending\n", Backlog, Pending);
    if (Count > Limit)
    {
        ok(Alive >= Limit && Alive < Count,
           "Backlog %d, %d clients: %d connections alive\n", Backlog, Count, Alive);
        if (is_reactos())
        {
            ok(Alive <= Limit,
               "Backlog %d, %d clients: %d connections alive\n", Backlog, Count, Alive);
        }
    }
    else
    {
        ok(Alive == Count, "Backlog %d, %d clients: %d connections alive\n", Backlog, Count, Alive);
    }
    ok(AcceptCount == Alive, "Backlog %d: %d accepted, %d alive\n", Backlog, AcceptCount, Alive);

    /* With the queue drained, the listener takes new connections again */
    ok(ConnectAndAccept(Server, &Addr), "Backlog %d: no connection after the queue was drained\n", Backlog);

    for (i = 0; i < Count; i++)
    {
        if (Clients[i] != INVALID_SOCKET)
            closesocket(Clients[i]);
    }
    closesocket(Server);
}

/* Two AcceptEx requests posted while the queue is full take the queued
 * connection and the next one, in order: the first at once on Windows, when
 * the next connection arrives on ReactOS. Neither may wait for good. */
static void
TestAcceptExFullBacklog(void)
{
    SOCKET Server, Clients[3], AcceptSockets[2];
    struct sockaddr_in Addr, ClientAddr[3], PeerAddr;
    int AddrLen, PeerLen, Error, i;
    LPFN_ACCEPTEX pAcceptEx = NULL;
    GUID AcceptExGuid = WSAID_ACCEPTEX;
    DWORD Bytes = 0, Flags = 0;
    OVERLAPPED Overlapped[2];
    char Buffer[2][2 * (sizeof(struct sockaddr_in) + 16)];
    u_long NonBlocking = 1;
    BOOL Result;

    Server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Server != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Server == INVALID_SOCKET)
        return;
    Error = WSAIoctl(Server, SIO_GET_EXTENSION_FUNCTION_POINTER, &AcceptExGuid, sizeof(AcceptExGuid),
                     &pAcceptEx, sizeof(pAcceptEx), &Bytes, NULL, NULL);
    if (Error != 0 || !pAcceptEx)
    {
        skip("No AcceptEx: %d\n", WSAGetLastError());
        closesocket(Server);
        return;
    }

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Error = bind(Server, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Error == 0, "bind failed: %d\n", WSAGetLastError());
    AddrLen = sizeof(Addr);
    getsockname(Server, (struct sockaddr *)&Addr, &AddrLen);
    Error = listen(Server, 1);
    ok(Error == 0, "listen failed: %d\n", WSAGetLastError());

    for (i = 0; i < 3; i++)
    {
        Clients[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(Clients[i] != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    }
    for (i = 0; i < 2; i++)
    {
        AcceptSockets[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(AcceptSockets[i] != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
        ZeroMemory(&Overlapped[i], sizeof(Overlapped[i]));
        Overlapped[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        ok(Overlapped[i].hEvent != NULL, "CreateEventW failed: %lu\n", GetLastError());
    }
    if (Clients[0] == INVALID_SOCKET || Clients[1] == INVALID_SOCKET || Clients[2] == INVALID_SOCKET ||
        AcceptSockets[0] == INVALID_SOCKET || AcceptSockets[1] == INVALID_SOCKET ||
        !Overlapped[0].hEvent || !Overlapped[1].hEvent)
    {
        skip("Out of resources\n");
        goto Cleanup;
    }

    /* Fill the queue */
    Error = connect(Clients[0], (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Error == 0, "connect failed: %d\n", WSAGetLastError());
    Sleep(500);

    for (i = 0; i < 2; i++)
    {
        Result = pAcceptEx(Server, AcceptSockets[i], Buffer[i], 0,
                           sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
                           &Bytes, &Overlapped[i]);
        ok(Result || WSAGetLastError() == ERROR_IO_PENDING, "AcceptEx %d failed: %d\n", i, WSAGetLastError());
    }

    /* Further clients give ReactOS the connections it waits for */
    for (i = 1; i < 3; i++)
    {
        ioctlsocket(Clients[i], FIONBIO, &NonBlocking);
        connect(Clients[i], (struct sockaddr *)&Addr, sizeof(Addr));
        Sleep(500);
    }

    for (i = 0; i < 3; i++)
    {
        AddrLen = sizeof(ClientAddr[i]);
        getsockname(Clients[i], (struct sockaddr *)&ClientAddr[i], &AddrLen);
    }

    for (i = 0; i < 2; i++)
    {
        ok(WaitForSingleObject(Overlapped[i].hEvent, 5000) == WAIT_OBJECT_0, "AcceptEx %d did not complete\n", i);
        Result = WSAGetOverlappedResult(Server, &Overlapped[i], &Bytes, FALSE, &Flags);
        ok(Result, "AcceptEx %d result: %d\n", i, WSAGetLastError());
        if (!Result)
            continue;

        /* They took the connections in order */
        setsockopt(AcceptSockets[i], SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&Server, sizeof(Server));
        PeerLen = sizeof(PeerAddr);
        Error = getpeername(AcceptSockets[i], (struct sockaddr *)&PeerAddr, &PeerLen);
        ok(Error == 0, "getpeername failed: %d\n", WSAGetLastError());
        ok(PeerAddr.sin_port == ClientAddr[i].sin_port, "AcceptEx %d took port %u, client %d has %u\n",
           i, ntohs(PeerAddr.sin_port), i, ntohs(ClientAddr[i].sin_port));
    }

Cleanup:
    /* Closing the listener cancels what is still pending */
    closesocket(Server);
    for (i = 0; i < 2; i++)
    {
        if (Overlapped[i].hEvent)
        {
            WaitForSingleObject(Overlapped[i].hEvent, 5000);
            CloseHandle(Overlapped[i].hEvent);
        }
        if (AcceptSockets[i] != INVALID_SOCKET)
            closesocket(AcceptSockets[i]);
    }
    for (i = 0; i < 3; i++)
    {
        if (Clients[i] != INVALID_SOCKET)
            closesocket(Clients[i]);
    }
}

START_TEST(backlog)
{
    WSADATA WsaData;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    TestBacklog(1, 16);
    TestBacklog(2, 32);
    TestBacklog(5, 48);
    TestBacklog(SOMAXCONN, 40);
    TestBacklog(SOMAXCONN, 300);
    TestAcceptExFullBacklog();

    WSACleanup();
}
