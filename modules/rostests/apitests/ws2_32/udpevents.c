/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WSAEventSelect on UDP sockets: only the selected events are reported, FD_WRITE is
 *              signalled once a socket is bound, FD_CONNECT and FD_WRITE on every connect
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

/* The expected values were recorded on Windows 10 22H2. */

/* The events WSAEnumNetworkEvents reports after waiting briefly for the event */
static
long
Events(SOCKET Socket, WSAEVENT Event)
{
    WSANETWORKEVENTS Network;

    WaitForSingleObject(Event, 300);
    ZeroMemory(&Network, sizeof(Network));
    if (WSAEnumNetworkEvents(Socket, Event, &Network) != 0)
        return -1;
    return Network.lNetworkEvents;
}

static
void
BindSocket(SOCKET Socket, struct sockaddr_in *Name)
{
    int Length = sizeof(*Name);

    ZeroMemory(Name, sizeof(*Name));
    Name->sin_family = AF_INET;
    ok(bind(Socket, (struct sockaddr *)Name, sizeof(*Name)) == 0, "bind failed: %d\n", WSAGetLastError());
    getsockname(Socket, (struct sockaddr *)Name, &Length);
    Name->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

START_TEST(udpevents)
{
    WSADATA WsaData;
    SOCKET Socket, Sender;
    WSAEVENT Event;
    struct sockaddr_in Name, Peer;
    long Reported;
    char Buffer[8];
    int Result;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }

    ZeroMemory(&Peer, sizeof(Peer));
    Peer.sin_family = AF_INET;
    Peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Peer.sin_port = htons(9);

    /* Nothing is reported before the socket is bound */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Event = WSACreateEvent();
    ok(WSAEventSelect(Socket, Event, FD_WRITE) == 0, "WSAEventSelect failed: %d\n", WSAGetLastError());
    Reported = Events(Socket, Event);
    ok(Reported == 0, "unbound: events 0x%lx\n", Reported);
    /* and sending, which binds it, signals FD_WRITE */
    Result = sendto(Socket, "x", 1, 0, (struct sockaddr *)&Peer, sizeof(Peer));
    ok(Result == 1, "sendto returned %d, error %d\n", Result, WSAGetLastError());
    Reported = Events(Socket, Event);
    ok(Reported == FD_WRITE, "after the implicit bind: events 0x%lx\n", Reported);
    closesocket(Socket);
    WSACloseEvent(Event);

    /* A bound socket reports FD_WRITE once, and only when it is selected */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BindSocket(Socket, &Name);
    Event = WSACreateEvent();
    WSAEventSelect(Socket, Event, FD_READ);
    Reported = Events(Socket, Event);
    ok(Reported == 0, "bound, FD_READ selected: events 0x%lx\n", Reported);
    WSAEventSelect(Socket, Event, FD_READ | FD_WRITE);
    Reported = Events(Socket, Event);
    ok(Reported == FD_WRITE, "bound, FD_READ|FD_WRITE selected: events 0x%lx\n", Reported);
    WSAEventSelect(Socket, Event, FD_READ | FD_WRITE);
    Reported = Events(Socket, Event);
    ok(Reported == 0, "selected again: events 0x%lx\n", Reported);

    /* Sending does not signal it again, a datagram signals FD_READ */
    Result = sendto(Socket, "x", 1, 0, (struct sockaddr *)&Name, sizeof(Name));
    ok(Result == 1, "sendto returned %d, error %d\n", Result, WSAGetLastError());
    Sleep(300);
    Reported = Events(Socket, Event);
    ok(Reported == FD_READ, "after sending to itself: events 0x%lx\n", Reported);
    Result = recv(Socket, Buffer, sizeof(Buffer), 0);
    ok(Result == 1, "recv returned %d\n", Result);
    Reported = Events(Socket, Event);
    ok(Reported == 0, "after the receive: events 0x%lx\n", Reported);

    /* A datagram from another socket, with only FD_READ selected */
    WSAEventSelect(Socket, Event, FD_READ);
    Sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sendto(Sender, "abc", 3, 0, (struct sockaddr *)&Name, sizeof(Name));
    Sleep(300);
    Reported = Events(Socket, Event);
    ok(Reported == FD_READ, "datagram queued, FD_READ selected: events 0x%lx\n", Reported);
    closesocket(Sender);
    closesocket(Socket);
    WSACloseEvent(Event);

    /* Every connect signals FD_CONNECT, and FD_WRITE again */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BindSocket(Socket, &Name);
    Event = WSACreateEvent();
    WSAEventSelect(Socket, Event, FD_WRITE | FD_CONNECT);
    Reported = Events(Socket, Event);
    ok(Reported == FD_WRITE, "bound, FD_WRITE|FD_CONNECT selected: events 0x%lx\n", Reported);
    ok(connect(Socket, (struct sockaddr *)&Peer, sizeof(Peer)) == 0, "connect failed: %d\n", WSAGetLastError());
    Reported = Events(Socket, Event);
    ok(Reported == (FD_WRITE | FD_CONNECT), "after connect: events 0x%lx\n", Reported);
    ok(connect(Socket, (struct sockaddr *)&Peer, sizeof(Peer)) == 0, "second connect failed: %d\n", WSAGetLastError());
    Reported = Events(Socket, Event);
    ok(Reported == (FD_WRITE | FD_CONNECT), "after the second connect: events 0x%lx\n", Reported);
    closesocket(Socket);
    WSACloseEvent(Event);

    /* With FD_CONNECT alone, connect on an unbound socket reports just that */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Event = WSACreateEvent();
    WSAEventSelect(Socket, Event, FD_CONNECT);
    ok(connect(Socket, (struct sockaddr *)&Peer, sizeof(Peer)) == 0, "connect failed: %d\n", WSAGetLastError());
    Reported = Events(Socket, Event);
    ok(Reported == FD_CONNECT, "FD_CONNECT selected, after connect: events 0x%lx\n", Reported);
    closesocket(Socket);
    WSACloseEvent(Event);

    /* FD_CONNECT stays set after a connect: selecting it later reports it at once, FD_WRITE
     * is still pending as it was not selected before */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(connect(Socket, (struct sockaddr *)&Peer, sizeof(Peer)) == 0, "connect failed: %d\n", WSAGetLastError());
    Event = WSACreateEvent();
    WSAEventSelect(Socket, Event, FD_CONNECT);
    Reported = Events(Socket, Event);
    ok(Reported == FD_CONNECT, "connected earlier, FD_CONNECT selected: events 0x%lx\n", Reported);
    WSAEventSelect(Socket, Event, FD_CONNECT | FD_WRITE);
    Reported = Events(Socket, Event);
    ok(Reported == FD_WRITE, "selected again with FD_WRITE: events 0x%lx\n", Reported);
    closesocket(Socket);
    WSACloseEvent(Event);

    /* A datagram socket never reports WSAEWOULDBLOCK, however many sends are in flight, so
     * there is no FD_WRITE to wait for: Windows sends over a million 1400 byte datagrams in
     * four seconds without one */
    {
        SOCKET Receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in To;
        u_long NonBlocking = 1;
        static char Datagram[1400];
        int i, WouldBlock = 0, Sent = 0;

        Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BindSocket(Receiver, &To);
        ioctlsocket(Socket, FIONBIO, &NonBlocking);
        for (i = 0; i < 3000; i++)
        {
            Result = sendto(Socket, Datagram, sizeof(Datagram), 0, (struct sockaddr *)&To, sizeof(To));
            if (Result == (int)sizeof(Datagram))
                Sent++;
            else if (WSAGetLastError() == WSAEWOULDBLOCK)
                WouldBlock++;
        }
        ok(WouldBlock == 0 && Sent == 3000, "%d datagrams sent, %d would have blocked\n", Sent, WouldBlock);
        closesocket(Socket);
        closesocket(Receiver);
    }

    WSACleanup();
}
