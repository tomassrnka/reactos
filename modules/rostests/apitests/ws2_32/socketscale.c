/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a process can hold many sockets and that running out fails cleanly
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

/* 5000 loopback pairs plus the listener: 10001 sockets held at once */
#define SCALE_PAIRS 5000

/* Bound sockets held while exhausting the ephemeral ports; Windows 7 has 16384 */
#define EXHAUST_MAX 70000

static
BOOL
Listen(SOCKET *Listener, struct sockaddr_in *Addr)
{
    int Len = sizeof(*Addr);

    *Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*Listener == INVALID_SOCKET)
        return FALSE;
    ZeroMemory(Addr, sizeof(*Addr));
    Addr->sin_family = AF_INET;
    Addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(*Listener, (struct sockaddr *)Addr, sizeof(*Addr)) ||
        listen(*Listener, SOMAXCONN) ||
        getsockname(*Listener, (struct sockaddr *)Addr, &Len))
    {
        closesocket(*Listener);
        *Listener = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

/* One byte each way over a connected pair; a lost byte fails after 5 s */
static
BOOL
Exchange(SOCKET A, SOCKET B, char Value)
{
    char Byte = Value;
    DWORD Timeout = 5000;

    if (setsockopt(A, SOL_SOCKET, SO_RCVTIMEO, (char *)&Timeout, sizeof(Timeout)) ||
        setsockopt(B, SOL_SOCKET, SO_RCVTIMEO, (char *)&Timeout, sizeof(Timeout)))
        return FALSE;
    if (send(A, &Byte, 1, 0) != 1)
        return FALSE;
    Byte = 0;
    if (recv(B, &Byte, 1, 0) != 1 || Byte != Value)
        return FALSE;
    if (send(B, &Byte, 1, 0) != 1)
        return FALSE;
    Byte = 0;
    return recv(A, &Byte, 1, 0) == 1 && Byte == Value;
}

/* Open SCALE_PAIRS sockets, connect each to a listener and accept it, use and close them all */
static
void
TestScale(void)
{
    SOCKET Listener, *Client, *Server;
    struct sockaddr_in Addr;
    int Opened = 0, Connected = 0, Error = 0, i;
    DWORD Start;

    Client = HeapAlloc(GetProcessHeap(), 0, SCALE_PAIRS * sizeof(SOCKET));
    Server = HeapAlloc(GetProcessHeap(), 0, SCALE_PAIRS * sizeof(SOCKET));
    if (!Client || !Server)
    {
        skip("No memory for the socket tables\n");
        goto Free;
    }
    if (!Listen(&Listener, &Addr))
    {
        ok(0, "The listener failed: %d\n", WSAGetLastError());
        goto Free;
    }

    Start = GetTickCount();
    for (i = 0; i < SCALE_PAIRS; i++)
    {
        Client[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Client[i] == INVALID_SOCKET)
        {
            Error = WSAGetLastError();
            break;
        }
        Opened++;
    }
    ok(Opened == SCALE_PAIRS, "Opened %d of %d sockets, then error %d\n", Opened, SCALE_PAIRS, Error);
    trace("Opened %d sockets in %lu ms\n", Opened, GetTickCount() - Start);

    Start = GetTickCount();
    Error = 0;
    for (i = 0; i < Opened; i++)
    {
        if (connect(Client[i], (struct sockaddr *)&Addr, sizeof(Addr)))
        {
            Error = WSAGetLastError();
            break;
        }
        Server[i] = accept(Listener, NULL, NULL);
        if (Server[i] == INVALID_SOCKET)
        {
            Error = -WSAGetLastError();
            break;
        }
        Connected++;
    }
    ok(Connected == Opened, "Connected %d of %d pairs, then error %d (negative: accept)\n",
       Connected, Opened, Error);
    trace("Connected %d pairs (%d sockets) in %lu ms\n", Connected, 2 * Connected + 1, GetTickCount() - Start);

    if (Connected > 0)
    {
        ok(Exchange(Client[0], Server[0], 'a'), "The first pair does not pass data: %d\n", WSAGetLastError());
        ok(Exchange(Client[Connected / 2], Server[Connected / 2], 'b'),
           "The middle pair does not pass data: %d\n", WSAGetLastError());
        ok(Exchange(Client[Connected - 1], Server[Connected - 1], 'c'),
           "The last pair does not pass data: %d\n", WSAGetLastError());
    }

    /* The accepted side closes first, so the TIME-WAIT connections are on the
       listener's port and the run gives every ephemeral port back */
    Start = GetTickCount();
    for (i = 0; i < Opened; i++)
    {
        if (i < Connected)
            ok(closesocket(Server[i]) == 0, "closesocket of accepted socket %d failed: %d\n", i, WSAGetLastError());
        ok(closesocket(Client[i]) == 0, "closesocket of client %d failed: %d\n", i, WSAGetLastError());
    }
    trace("Closed %d sockets in %lu ms\n", Opened + Connected, GetTickCount() - Start);

    /* Everything was given back: a new pair connects */
    Client[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client[0] != INVALID_SOCKET, "socket after the close failed: %d\n", WSAGetLastError());
    if (Client[0] != INVALID_SOCKET)
    {
        Error = connect(Client[0], (struct sockaddr *)&Addr, sizeof(Addr)) ? WSAGetLastError() : 0;
        ok(Error == 0, "connect after the close failed: %d\n", Error);
        if (Error == 0)
        {
            Server[0] = accept(Listener, NULL, NULL);
            ok(Server[0] != INVALID_SOCKET, "accept after the close failed: %d\n", WSAGetLastError());
            if (Server[0] != INVALID_SOCKET)
            {
                ok(Exchange(Client[0], Server[0], 'd'), "The new pair does not pass data: %d\n", WSAGetLastError());
                closesocket(Server[0]);
            }
        }
        closesocket(Client[0]);
    }
    closesocket(Listener);

Free:
    if (Client)
        HeapFree(GetProcessHeap(), 0, Client);
    if (Server)
        HeapFree(GetProcessHeap(), 0, Server);
}

/* Hold every TCP ephemeral port: the next bind of port 0 fails with WSAENOBUFS;
 * after one is closed, the next bind gets that port */
static
void
TestPortExhaustion(void)
{
    SOCKET *Held, Socket;
    struct sockaddr_in Addr;
    int Count = 0, Error = 0, Len, i;
    USHORT Freed, Got;

    Held = HeapAlloc(GetProcessHeap(), 0, EXHAUST_MAX * sizeof(SOCKET));
    if (!Held)
    {
        skip("No memory for the socket table\n");
        return;
    }

    while (Count < EXHAUST_MAX)
    {
        Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Socket == INVALID_SOCKET)
        {
            Error = -WSAGetLastError();
            break;
        }
        ZeroMemory(&Addr, sizeof(Addr));
        Addr.sin_family = AF_INET;
        Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(Socket, (struct sockaddr *)&Addr, sizeof(Addr)))
        {
            Error = WSAGetLastError();
            closesocket(Socket);
            break;
        }
        Held[Count++] = Socket;
    }
    trace("Port exhaustion: %d ports held, then error %d (negative: socket)\n", Count, Error);
    ok(Error == WSAENOBUFS, "Running out of ephemeral ports gave %d (negative: socket), not WSAENOBUFS\n", Error);
    ok(Count >= 10000, "Only %d ephemeral ports could be held\n", Count);

    if (Count > 0 && Error > 0)
    {
        /* Free one port from the middle; it is the only one, so the next bind takes it */
        Len = sizeof(Addr);
        getsockname(Held[Count / 2], (struct sockaddr *)&Addr, &Len);
        Freed = ntohs(Addr.sin_port);
        closesocket(Held[Count / 2]);
        Held[Count / 2] = INVALID_SOCKET;
        Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(Socket != INVALID_SOCKET, "socket after freeing a port failed: %d\n", WSAGetLastError());
        ZeroMemory(&Addr, sizeof(Addr));
        Addr.sin_family = AF_INET;
        Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Error = bind(Socket, (struct sockaddr *)&Addr, sizeof(Addr)) ? WSAGetLastError() : 0;
        ok(Error == 0, "bind after freeing a port failed: %d\n", Error);
        Len = sizeof(Addr);
        getsockname(Socket, (struct sockaddr *)&Addr, &Len);
        Got = ntohs(Addr.sin_port);
        ok(Error != 0 || Got == Freed, "Freed port %u, got %u\n", Freed, Got);
        closesocket(Socket);
    }

    for (i = 0; i < Count; i++)
    {
        if (Held[i] != INVALID_SOCKET)
            closesocket(Held[i]);
    }
    HeapFree(GetProcessHeap(), 0, Held);
}

START_TEST(socketscale)
{
    WSADATA WsaData;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData))
    {
        skip("WSAStartup failed\n");
        return;
    }
    TestScale();
    TestPortExhaustion();
    /* The stack is still usable after running out */
    TestScale();
    WSACleanup();
}
