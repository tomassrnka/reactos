/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test a reconnection from the port of a connection that the server holds in TIME-WAIT
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <iphlpapi.h>

/* Moves the server's send sequence past the next initial sequence numbers of a stack whose generator
   rises slowly (lwIP: by its tick count per connection) */
#define FIRST_TRANSFER (32 * 1024)
#define CHUNK 4096

/* Every operation in this test ends within this time */
#define TIMEOUT_MS 5000

/* Wait until the socket is readable (or writable) before the deadline (GetTickCount) */
static
BOOL
WaitReady(SOCKET Socket, BOOL Write, DWORD Deadline)
{
    fd_set Set;
    struct timeval Timeout;
    LONG Left = (LONG)(Deadline - GetTickCount());

    if (Left <= 0)
        return FALSE;
    Timeout.tv_sec = Left / 1000;
    Timeout.tv_usec = (Left % 1000) * 1000;
    FD_ZERO(&Set);
    FD_SET(Socket, &Set);
    return select(0, Write ? NULL : &Set, Write ? &Set : NULL, NULL, &Timeout) == 1;
}

static
BOOL
SetNonBlocking(SOCKET Socket)
{
    u_long NonBlocking = 1;

    return ioctlsocket(Socket, FIONBIO, &NonBlocking) == 0;
}

/* Connect a non-blocking socket; return 0 or the error (WSAETIMEDOUT after the timeout) */
static
int
ConnectWithin(SOCKET Socket, const struct sockaddr_in *Addr)
{
    fd_set WriteSet, ExceptSet;
    struct timeval Timeout = { TIMEOUT_MS / 1000, (TIMEOUT_MS % 1000) * 1000 };
    int Error, Len = sizeof(Error), Result;

    if (connect(Socket, (const struct sockaddr *)Addr, sizeof(*Addr)) == 0)
        return 0;
    Error = WSAGetLastError();
    if (Error != WSAEWOULDBLOCK)
        return Error;
    FD_ZERO(&WriteSet);
    FD_SET(Socket, &WriteSet);
    FD_ZERO(&ExceptSet);
    FD_SET(Socket, &ExceptSet);
    Result = select(0, NULL, &WriteSet, &ExceptSet, &Timeout);
    if (Result == 0)
        return WSAETIMEDOUT;
    if (Result == SOCKET_ERROR)
        return WSAGetLastError();
    Error = 0;
    if (getsockopt(Socket, SOL_SOCKET, SO_ERROR, (char *)&Error, &Len) == SOCKET_ERROR)
        return WSAGetLastError();
    if (Error == 0 && !FD_ISSET(Socket, &WriteSet))
        Error = WSAECONNREFUSED;
    return Error;
}

/* Send exactly Length bytes; FALSE on an error or a timeout */
static
BOOL
SendAll(SOCKET Socket, const char *Buffer, int Length)
{
    DWORD Deadline = GetTickCount() + TIMEOUT_MS;
    int Sent = 0, Result;

    while (Sent < Length)
    {
        if (!WaitReady(Socket, TRUE, Deadline))
            return FALSE;
        Result = send(Socket, Buffer + Sent, Length - Sent, 0);
        if (Result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            continue;
        if (Result <= 0)
            return FALSE;
        Sent += Result;
    }
    return TRUE;
}

/* Receive exactly Length bytes; FALSE on an error, an early close or a timeout */
static
BOOL
ReceiveAll(SOCKET Socket, char *Buffer, int Length)
{
    DWORD Deadline = GetTickCount() + TIMEOUT_MS;
    int Received = 0, Result;

    while (Received < Length)
    {
        if (!WaitReady(Socket, FALSE, Deadline))
            return FALSE;
        Result = recv(Socket, Buffer + Received, Length - Received, 0);
        if (Result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            continue;
        if (Result <= 0)
            return FALSE;
        Received += Result;
    }
    return TRUE;
}

/* Wait for the peer's FIN: a recv that returns 0 */
static
BOOL
ReceiveFin(SOCKET Socket)
{
    DWORD Deadline = GetTickCount() + TIMEOUT_MS;
    char Byte;
    int Result;

    while (WaitReady(Socket, FALSE, Deadline))
    {
        Result = recv(Socket, &Byte, 1, 0);
        if (Result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            continue;
        return Result == 0;
    }
    return FALSE;
}

static
SOCKET
AcceptWithin(SOCKET Listener, struct sockaddr_in *Peer)
{
    DWORD Deadline = GetTickCount() + TIMEOUT_MS;
    SOCKET Socket;
    int Len;

    while (WaitReady(Listener, FALSE, Deadline))
    {
        Len = sizeof(*Peer);
        Socket = accept(Listener, (struct sockaddr *)Peer, &Len);
        if (Socket != INVALID_SOCKET || WSAGetLastError() != WSAEWOULDBLOCK)
            return Socket;
    }
    return INVALID_SOCKET;
}

/* Whether the TCP table shows the server side of the connection in TIME-WAIT */
static
BOOL
ServerInTimeWait(const struct sockaddr_in *Server, const struct sockaddr_in *Client)
{
    PMIB_TCPTABLE Table;
    DWORD Size = 0, i;
    BOOL Found = FALSE;

    if (GetTcpTable(NULL, &Size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;
    Table = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Table)
        return FALSE;
    if (GetTcpTable(Table, &Size, FALSE) == NO_ERROR)
    {
        for (i = 0; i < Table->dwNumEntries; i++)
        {
            if (Table->table[i].dwState == MIB_TCP_STATE_TIME_WAIT &&
                Table->table[i].dwLocalAddr == Server->sin_addr.s_addr &&
                Table->table[i].dwLocalPort == Server->sin_port &&
                Table->table[i].dwRemoteAddr == Client->sin_addr.s_addr &&
                Table->table[i].dwRemotePort == Client->sin_port)
            {
                Found = TRUE;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, Table);
    return Found;
}

/* First connection from a fixed client port, closed so that the server holds it in TIME-WAIT */
static
BOOL
FirstConnection(SOCKET Listener, const struct sockaddr_in *ServerAddr, struct sockaddr_in *ClientAddr, char *Buffer)
{
    SOCKET Client, Server = INVALID_SOCKET;
    struct sockaddr_in Any, Peer;
    int Len = sizeof(*ClientAddr), Offset, Result;
    BOOL Done = FALSE;

    ZeroMemory(&Any, sizeof(Any));
    Any.sin_family = AF_INET;
    Any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Client == INVALID_SOCKET || !SetNonBlocking(Client) ||
        bind(Client, (struct sockaddr *)&Any, sizeof(Any)) == SOCKET_ERROR ||
        getsockname(Client, (struct sockaddr *)ClientAddr, &Len) == SOCKET_ERROR)
    {
        skip("client setup failed: %d\n", WSAGetLastError());
        goto Cleanup;
    }
    Result = ConnectWithin(Client, ServerAddr);
    if (Result != 0 || (Server = AcceptWithin(Listener, &Peer)) == INVALID_SOCKET)
    {
        skip("first connection failed: connect %d, accept %d\n", Result, WSAGetLastError());
        goto Cleanup;
    }

    /* The server sends; small chunks, each read before the next, so no send buffer has to hold them */
    for (Offset = 0; Offset < FIRST_TRANSFER; Offset += CHUNK)
    {
        if (!SendAll(Server, Buffer + Offset, CHUNK) || !ReceiveAll(Client, Buffer + Offset, CHUNK))
        {
            ok(0, "The first transfer stopped at %d bytes: %d\n", Offset, WSAGetLastError());
            goto Cleanup;
        }
    }

    /* The server sends its FIN first; the client's FIN then moves the server into TIME-WAIT */
    Result = shutdown(Server, SD_SEND);
    ok(Result == 0, "shutdown failed: %d\n", WSAGetLastError());
    if (!ReceiveFin(Client))
    {
        ok(0, "The client did not see the server's FIN\n");
        goto Cleanup;
    }
    Result = closesocket(Client);
    Client = INVALID_SOCKET;
    ok(Result == 0, "closesocket failed: %d\n", WSAGetLastError());
    if (!ReceiveFin(Server))
    {
        ok(0, "The server did not see the client's FIN\n");
        goto Cleanup;
    }
    Done = TRUE;

Cleanup:
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    return Done;
}

static
void
Test_ReconnectFromTimeWaitPort(void)
{
    SOCKET Listener, Client = INVALID_SOCKET, Server = INVALID_SOCKET;
    struct sockaddr_in ServerAddr, ClientAddr, PeerAddr;
    int Len, Result, Try;
    char Byte = 0;
    char *Buffer;

    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, FIRST_TRANSFER);
    ok(Buffer != NULL, "HeapAlloc failed\n");
    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (!Buffer || Listener == INVALID_SOCKET)
        goto Cleanup;
    ZeroMemory(&ServerAddr, sizeof(ServerAddr));
    ServerAddr.sin_family = AF_INET;
    ServerAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Len = sizeof(ServerAddr);
    if (!SetNonBlocking(Listener) ||
        bind(Listener, (struct sockaddr *)&ServerAddr, sizeof(ServerAddr)) == SOCKET_ERROR ||
        listen(Listener, 5) == SOCKET_ERROR ||
        getsockname(Listener, (struct sockaddr *)&ServerAddr, &Len) == SOCKET_ERROR)
    {
        skip("listener setup failed: %d\n", WSAGetLastError());
        goto Cleanup;
    }

    if (!FirstConnection(Listener, &ServerAddr, &ClientAddr, Buffer))
        goto Cleanup;
    /* ReactOS does not list connections in TIME-WAIT in the table */
    trace("TIME-WAIT listed in the TCP table: %d\n", ServerInTimeWait(&ServerAddr, &ClientAddr));

    /* Second connection from the same port */
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Client != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Client == INVALID_SOCKET)
        goto Cleanup;
    Result = SetNonBlocking(Client);
    ok(Result, "ioctlsocket failed: %d\n", WSAGetLastError());
    if (!Result)
        goto Cleanup;
    /* The client's own side may need a moment to leave LAST-ACK */
    for (Try = 0; Try < 40; Try++)
    {
        Result = bind(Client, (struct sockaddr *)&ClientAddr, sizeof(ClientAddr));
        if (Result == 0 || WSAGetLastError() != WSAEADDRINUSE)
            break;
        Sleep(50);
    }
    ok(Result == 0, "bind to the first client port failed: %d\n", WSAGetLastError());
    if (Result != 0)
        goto Cleanup;

    /* ReactOS used to answer with an ACK (or a reset, inside the old receive window) until
       TIME-WAIT ended, 120 seconds later */
    Result = ConnectWithin(Client, &ServerAddr);
    ok(Result == 0, "The reconnection from the TIME-WAIT port failed: %d\n", Result);
    if (Result != 0)
        goto Cleanup;
    Server = AcceptWithin(Listener, &PeerAddr);
    ok(Server != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    if (Server == INVALID_SOCKET)
        goto Cleanup;
    ok(PeerAddr.sin_port == ClientAddr.sin_port, "accepted peer port %u, expected %u\n",
       ntohs(PeerAddr.sin_port), ntohs(ClientAddr.sin_port));

    /* The new connection carries data both ways */
    ok(SendAll(Server, "s", 1), "send on the server failed: %d\n", WSAGetLastError());
    ok(ReceiveAll(Client, &Byte, 1) && Byte == 's', "The client did not receive the byte (%d)\n", Byte);
    ok(SendAll(Client, "c", 1), "send on the client failed: %d\n", WSAGetLastError());
    ok(ReceiveAll(Server, &Byte, 1) && Byte == 'c', "The server did not receive the byte (%d)\n", Byte);

Cleanup:
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Listener != INVALID_SOCKET)
        closesocket(Listener);
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
}

START_TEST(timewait)
{
    WSADATA WsaData;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    Test_ReconnectFromTimeWaitPort();

    WSACleanup();
}
