/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SO_ERROR after a refused connection: a blocking connect reports the error itself and leaves
 *              SO_ERROR at 0, a non-blocking connect reports it through select/WSAEventSelect and SO_ERROR
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

static int GetSoError(SOCKET Socket)
{
    int Error = 0xdeadbeef, Length = sizeof(Error);

    ok(getsockopt(Socket, SOL_SOCKET, SO_ERROR, (char *)&Error, &Length) == 0, "SO_ERROR failed\n");
    return Error;
}

/* Binds a loopback socket to an ephemeral port and closes it: nothing listens there */
static BOOL GetClosedPort(struct sockaddr_in *Address)
{
    SOCKET Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int Length = sizeof(*Address);
    BOOL Ok;

    ZeroMemory(Address, sizeof(*Address));
    Address->sin_family = AF_INET;
    Address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Ok = Socket != INVALID_SOCKET &&
         bind(Socket, (struct sockaddr *)Address, sizeof(*Address)) == 0 &&
         getsockname(Socket, (struct sockaddr *)Address, &Length) == 0;
    if (Socket != INVALID_SOCKET)
        closesocket(Socket);
    return Ok;
}

START_TEST(connecterror)
{
    WSADATA WsaData;
    struct sockaddr_in Address;
    SOCKET Socket;
    ULONG NonBlocking = 1;
    fd_set Write, Except;
    struct timeval Timeout = { 10, 0 };
    WSAEVENT Event;
    WSANETWORKEVENTS Events;
    int Result;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    if (!GetClosedPort(&Address))
    {
        skip("no closed port\n");
        WSACleanup();
        return;
    }

    /* Blocking: the error comes from connect, SO_ERROR stays 0 */
    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed\n");
    ok_int(GetSoError(Socket), 0);
    Result = connect(Socket, (struct sockaddr *)&Address, sizeof(Address));
    ok_int(Result, SOCKET_ERROR);
    ok_int(WSAGetLastError(), WSAECONNREFUSED);
    ok_int(GetSoError(Socket), 0);
    ok_int(GetSoError(Socket), 0);
    closesocket(Socket);

    /* Non-blocking with select: reported in the except set and by SO_ERROR, which does not clear */
    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok_int(ioctlsocket(Socket, FIONBIO, &NonBlocking), 0);
    Result = connect(Socket, (struct sockaddr *)&Address, sizeof(Address));
    ok_int(Result, SOCKET_ERROR);
    ok_int(WSAGetLastError(), WSAEWOULDBLOCK);
    FD_ZERO(&Write);
    FD_ZERO(&Except);
    FD_SET(Socket, &Write);
    FD_SET(Socket, &Except);
    Result = select(0, NULL, &Write, &Except, &Timeout);
    ok_int(Result, 1);
    ok(!FD_ISSET(Socket, &Write), "refused socket is writable\n");
    ok(FD_ISSET(Socket, &Except), "refused socket is not in the except set\n");
    ok_int(GetSoError(Socket), WSAECONNREFUSED);
    ok_int(GetSoError(Socket), WSAECONNREFUSED);
    closesocket(Socket);

    /* Non-blocking with WSAEventSelect */
    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Event = WSACreateEvent();
    ok_int(WSAEventSelect(Socket, Event, FD_CONNECT), 0);
    Result = connect(Socket, (struct sockaddr *)&Address, sizeof(Address));
    ok_int(Result, SOCKET_ERROR);
    ok_int(WSAGetLastError(), WSAEWOULDBLOCK);
    ok_int(WaitForSingleObject(Event, 10000), WAIT_OBJECT_0);
    ZeroMemory(&Events, sizeof(Events));
    ok_int(WSAEnumNetworkEvents(Socket, Event, &Events), 0);
    ok(Events.lNetworkEvents & FD_CONNECT, "no FD_CONNECT, events %lx\n", Events.lNetworkEvents);
    ok_int(Events.iErrorCode[FD_CONNECT_BIT], WSAECONNREFUSED);
    ok_int(GetSoError(Socket), WSAECONNREFUSED);
    WSACloseEvent(Event);
    closesocket(Socket);

    WSACleanup();
}
