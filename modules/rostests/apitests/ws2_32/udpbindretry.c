/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a UDP socket can be bound again after a failed bind
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "ws2_32.h"

/* A fixed port, so that an address a failed bind left open is seen again */
#define TEST_PORT 38817

static
int
BindLoopback(SOCKET Socket)
{
    struct sockaddr_in Address;

    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = htons(TEST_PORT);
    if (bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) == SOCKET_ERROR)
        return WSAGetLastError();
    return 0;
}

/* Bind to TEST_PORT; a bind that fails for lack of memory is retried once.
 * Returns TRUE when the socket is bound */
static
BOOL
BindWithRetry(SOCKET Socket, const char *Name, int *Failures)
{
    int Error;

    Error = BindLoopback(Socket);
    if (Error == WSAENOBUFS)
    {
        (*Failures)++;
        trace("%s: the first bind failed with WSAENOBUFS, binding again\n", Name);
        Error = BindLoopback(Socket);
        ok(Error == 0, "%s: bind after a failed bind returned %d\n", Name, Error);
    }
    else
    {
        ok(Error == 0, "%s: bind returned %d\n", Name, Error);
    }
    return Error == 0;
}

/* Send one datagram to TEST_PORT and check that Receiver gets it */
static
void
CheckReceive(SOCKET Receiver, const char *Name)
{
    SOCKET Sender;
    struct sockaddr_in To;
    fd_set ReadSet;
    struct timeval Timeout;
    char Buffer[64];
    int Received = -1;

    Sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sender != INVALID_SOCKET, "%s: socket failed: %d\n", Name, WSAGetLastError());
    if (Sender == INVALID_SOCKET)
        return;

    ZeroMemory(&To, sizeof(To));
    To.sin_family = AF_INET;
    To.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    To.sin_port = htons(TEST_PORT);
    if (sendto(Sender, Name, (int)strlen(Name), 0, (struct sockaddr *)&To, sizeof(To)) != (int)strlen(Name))
    {
        ok(0, "%s: sendto failed: %d\n", Name, WSAGetLastError());
        closesocket(Sender);
        return;
    }

    FD_ZERO(&ReadSet);
    FD_SET(Receiver, &ReadSet);
    Timeout.tv_sec = 3;
    Timeout.tv_usec = 0;
    if (select(0, &ReadSet, NULL, NULL, &Timeout) == 1)
        Received = recv(Receiver, Buffer, sizeof(Buffer), 0);
    ok(Received == (int)strlen(Name) && !memcmp(Buffer, Name, Received),
       "%s: the bound socket did not receive the datagram: %d, error %d\n", Name, Received, WSAGetLastError());

    closesocket(Sender);
}

START_TEST(udpbindretry)
{
    WSADATA WsaData;
    SOCKET First, Second;
    int Failures = 0;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }

    /* A failed bind must leave the socket and the port as they were: the
     * socket can be bound again, and receives */
    First = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(First != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (First != INVALID_SOCKET)
    {
        if (BindWithRetry(First, "first", &Failures))
            CheckReceive(First, "first");
        closesocket(First);
    }

    /* Closing the socket must release the port */
    Second = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Second != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Second != INVALID_SOCKET)
    {
        if (BindWithRetry(Second, "second", &Failures))
            CheckReceive(Second, "second");
        closesocket(Second);
    }

    /* Only an allocation failure makes the first bind fail; without one,
     * only the ordinary path ran */
    if (!Failures)
        skip("No bind failed, so the bind after a failed bind was not tested\n");
    else
        trace("%d binds failed and were retried\n", Failures);

    WSACleanup();
}
