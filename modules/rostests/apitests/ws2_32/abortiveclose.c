/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test the abortive and graceful close of a TCP connection
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

/* Make a connected loopback pair; return FALSE on failure (after a skip) */
static
BOOL
MakePair(SOCKET *Client, SOCKET *Server)
{
    SOCKET Listener;
    struct sockaddr_in Addr;
    int Len = sizeof(Addr);

    *Client = *Server = INVALID_SOCKET;
    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Listener == INVALID_SOCKET)
    {
        skip("socket failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR ||
        listen(Listener, 1) == SOCKET_ERROR ||
        getsockname(Listener, (struct sockaddr *)&Addr, &Len) == SOCKET_ERROR)
    {
        skip("listener setup failed: %d\n", WSAGetLastError());
        closesocket(Listener);
        return FALSE;
    }

    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*Client == INVALID_SOCKET ||
        connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR)
    {
        skip("connect failed: %d\n", WSAGetLastError());
        if (*Client != INVALID_SOCKET)
            closesocket(*Client);
        closesocket(Listener);
        return FALSE;
    }

    *Server = accept(Listener, NULL, NULL);
    closesocket(Listener);
    if (*Server == INVALID_SOCKET)
    {
        skip("accept failed: %d\n", WSAGetLastError());
        closesocket(*Client);
        return FALSE;
    }
    return TRUE;
}

/* Wait until the socket is readable; return FALSE after the timeout */
static
BOOL
WaitReadable(SOCKET Socket, LONG Seconds)
{
    fd_set ReadSet;
    struct timeval Timeout = { Seconds, 0 };

    FD_ZERO(&ReadSet);
    FD_SET(Socket, &ReadSet);
    return select(0, &ReadSet, NULL, NULL, &Timeout) == 1;
}

static
void
Test_AbortiveClose(void)
{
    SOCKET Client, Server;
    struct linger Linger = { 1, 0 };
    char Buffer[16];
    int Result, Error;

    if (!MakePair(&Client, &Server))
        return;

    /* A zero linger timeout makes the close abortive: the peer gets a reset */
    Result = setsockopt(Server, SOL_SOCKET, SO_LINGER, (const char *)&Linger, sizeof(Linger));
    ok(Result == 0, "setsockopt(SO_LINGER) failed: %d\n", WSAGetLastError());
    Result = closesocket(Server);
    ok(Result == 0, "closesocket failed: %d\n", WSAGetLastError());

    if (!WaitReadable(Client, 5))
    {
        ok(0, "The client saw no event after the abortive close\n");
        closesocket(Client);
        return;
    }
    SetLastError(0xdeadbeef);
    Result = recv(Client, Buffer, sizeof(Buffer), 0);
    Error = WSAGetLastError();
    ok(Result == SOCKET_ERROR, "recv returned %d (expected an error, not a graceful close)\n", Result);
    ok(Error == WSAECONNRESET, "recv error %d, expected WSAECONNRESET\n", Error);

    SetLastError(0xdeadbeef);
    Result = send(Client, "x", 1, 0);
    Error = WSAGetLastError();
    ok(Result == SOCKET_ERROR, "send after the reset returned %d\n", Result);
    ok(Error == WSAECONNRESET, "send error %d, expected WSAECONNRESET\n", Error);

    closesocket(Client);
}

static
void
Test_GracefulClose(BOOL Shutdown)
{
    SOCKET Client, Server;
    char Buffer[16];
    int Result;

    if (!MakePair(&Client, &Server))
        return;

    /* A shutdown of both directions, or a close without linger, is graceful:
     * the peer gets the data that was sent before it, then the end of the stream.
     * After a shutdown the server keeps its handle until the client saw both. */
    Result = send(Server, "abc", 3, 0);
    ok(Result == 3, "send returned %d, error %d\n", Result, WSAGetLastError());
    if (Shutdown)
    {
        Result = shutdown(Server, SD_BOTH);
        ok(Result == 0, "shutdown failed: %d\n", WSAGetLastError());
    }
    else
    {
        Result = closesocket(Server);
        ok(Result == 0, "closesocket failed: %d\n", WSAGetLastError());
        Server = INVALID_SOCKET;
    }

    if (!WaitReadable(Client, 5))
    {
        ok(0, "The client saw no data\n");
        goto Cleanup;
    }
    ZeroMemory(Buffer, sizeof(Buffer));
    Result = recv(Client, Buffer, sizeof(Buffer), 0);
    ok(Result == 3 && !memcmp(Buffer, "abc", 3), "recv returned %d (%.3s), error %d (expected the data)\n",
       Result, Buffer, WSAGetLastError());
    if (!WaitReadable(Client, 5))
    {
        ok(0, "The client saw no event after the graceful close\n");
        goto Cleanup;
    }
    Result = recv(Client, Buffer, sizeof(Buffer), 0);
    ok(Result == 0, "recv returned %d, error %d (expected a graceful close)\n", Result, WSAGetLastError());

Cleanup:
    if (Server != INVALID_SOCKET)
    {
        Result = closesocket(Server);
        ok(Result == 0, "closesocket failed: %d\n", WSAGetLastError());
    }
    closesocket(Client);
}

static
void
Test_ConnectRefused(void)
{
    SOCKET Socket, Reserve;
    struct sockaddr_in Addr;
    int Len = sizeof(Addr), Result, Error;

    /* Hold a bound port that nothing listens on */
    Reserve = Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (Socket == INVALID_SOCKET ||
        bind(Socket, (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR ||
        getsockname(Socket, (struct sockaddr *)&Addr, &Len) == SOCKET_ERROR)
    {
        skip("socket setup failed: %d\n", WSAGetLastError());
        if (Socket != INVALID_SOCKET)
            closesocket(Socket);
        return;
    }

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Socket == INVALID_SOCKET)
    {
        closesocket(Reserve);
        return;
    }
    SetLastError(0xdeadbeef);
    Result = connect(Socket, (struct sockaddr *)&Addr, sizeof(Addr));
    Error = WSAGetLastError();
    ok(Result == SOCKET_ERROR, "connect to a closed port returned %d\n", Result);
    ok(Error == WSAECONNREFUSED, "connect error %d, expected WSAECONNREFUSED\n", Error);
    closesocket(Socket);
    closesocket(Reserve);
}

START_TEST(abortiveclose)
{
    WSADATA WsaData;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData))
    {
        skip("WSAStartup failed\n");
        return;
    }

    Test_AbortiveClose();
    Test_GracefulClose(TRUE);
    Test_GracefulClose(FALSE);
    Test_ConnectRefused();

    WSACleanup();
}
