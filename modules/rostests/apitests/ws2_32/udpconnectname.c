/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     getsockname after connect on a UDP socket reports the address of the
 *              interface that reaches the peer
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

/* The expected values were recorded on Windows 10 22H2: connect() sends nothing, but
 * the socket is then bound to the address of the route's interface (127.0.0.1 for
 * any 127.x.x.x peer, the network interface for every other one, multicast and
 * broadcast included), and disconnecting returns it to the wildcard address. */

static
BOOL
ConnectTo(SOCKET Socket, const char *Peer, int *Error)
{
    struct sockaddr_in Address;

    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = inet_addr(Peer);
    Address.sin_port = htons(5353);
    if (connect(Socket, (struct sockaddr *)&Address, sizeof(Address)) == 0)
        return TRUE;
    *Error = WSAGetLastError();
    return FALSE;
}

static
BOOL
LocalName(SOCKET Socket, struct sockaddr_in *Name, const char *What)
{
    int Length = sizeof(*Name);

    memset(Name, 0x55, sizeof(*Name));
    if (getsockname(Socket, (struct sockaddr *)Name, &Length) != 0)
    {
        ok(0, "%s: getsockname failed: %d\n", What, WSAGetLastError());
        return FALSE;
    }
    ok(Name->sin_family == AF_INET && Length == sizeof(*Name), "%s: family %u, length %d\n",
       What, Name->sin_family, Length);
    return TRUE;
}

static
void
CheckLoopbackPeer(const char *Peer)
{
    SOCKET Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in Name;
    int Error = 0;
    char What[64];

    sprintf(What, "connect to %s", Peer);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Socket == INVALID_SOCKET)
        return;
    ok(ConnectTo(Socket, Peer, &Error), "%s failed: %d\n", What, Error);
    if (LocalName(Socket, &Name, What))
    {
        ok(Name.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "%s: local address %s, expected 127.0.0.1\n",
           What, inet_ntoa(Name.sin_addr));
        ok(Name.sin_port != 0, "%s: local port is 0\n", What);
    }
    closesocket(Socket);
}


/* connect() outcome, with the socket optionally bound first */
static
int
ConnectError(SOCKET Socket, const char *Peer)
{
    int Error = 0;

    return ConnectTo(Socket, Peer, &Error) ? 0 : Error;
}

static
void
CheckErrors(const char *Interface)
{
    struct sockaddr_in Address, Name;
    SOCKET Socket;
    char Buffer[4];
    int Result, Error, Length;

    /* The unspecified address is no peer */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Error = ConnectError(Socket, "0.0.0.0");
    ok(Error == WSAEADDRNOTAVAIL, "connect to 0.0.0.0: error %d, expected %d\n", Error, WSAEADDRNOTAVAIL);
    Length = sizeof(Name);
    Result = getpeername(Socket, (struct sockaddr *)&Name, &Length);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAENOTCONN, "getpeername: %d, error %d\n", Result, WSAGetLastError());
    closesocket(Socket);

    /* Not connected: no peer to send to or to disconnect from */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Result = send(Socket, "x", 1, 0);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAENOTCONN, "send on an unconnected socket: %d, error %d\n", Result, WSAGetLastError());
    ZeroMemory(&Address, sizeof(Address));
    Result = connect(Socket, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEAFNOSUPPORT, "disconnect while unconnected: %d, error %d\n", Result, WSAGetLastError());

    /* Disconnecting leaves no peer */
    ok(ConnectTo(Socket, "127.0.0.1", &Error), "connect failed: %d\n", Error);
    Length = sizeof(Name);
    ok(getpeername(Socket, (struct sockaddr *)&Name, &Length) == 0, "getpeername while connected failed\n");
    Result = connect(Socket, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 0, "disconnect failed: %d\n", WSAGetLastError());
    Length = sizeof(Name);
    Result = getpeername(Socket, (struct sockaddr *)&Name, &Length);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAENOTCONN, "getpeername after disconnect: %d, error %d\n", Result, WSAGetLastError());
    Result = send(Socket, Buffer, 1, 0);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAENOTCONN, "send after disconnect: %d, error %d\n", Result, WSAGetLastError());
    closesocket(Socket);

    /* A socket bound to the loopback address reaches loopback peers only */
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) == 0, "bind failed: %d\n", WSAGetLastError());
    Error = ConnectError(Socket, "8.8.8.8");
    ok(Error == WSAENETUNREACH, "bound to 127.0.0.1, connect to 8.8.8.8: error %d, expected %d\n", Error, WSAENETUNREACH);
    Error = ConnectError(Socket, "127.0.0.1");
    ok(Error == 0, "bound to 127.0.0.1, connect to 127.0.0.1: error %d\n", Error);
    Error = ConnectError(Socket, Interface);
    ok(Error == WSAENETUNREACH, "bound to 127.0.0.1, connect to %s: error %d, expected %d\n", Interface, Error, WSAENETUNREACH);
    /* multicast, broadcast and link-local peers are accepted */
    Error = ConnectError(Socket, "224.0.0.251");
    ok(Error == 0, "bound to 127.0.0.1, connect to 224.0.0.251: error %d\n", Error);
    Error = ConnectError(Socket, "255.255.255.255");
    ok(Error == 0, "bound to 127.0.0.1, connect to 255.255.255.255: error %d\n", Error);
    Error = ConnectError(Socket, "169.254.7.7");
    ok(Error == 0, "bound to 127.0.0.1, connect to 169.254.7.7: error %d\n", Error);
    closesocket(Socket);

    /* and one bound to the interface address no loopback peer */
    Address.sin_addr.s_addr = inet_addr(Interface);
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) == 0, "bind failed: %d\n", WSAGetLastError());
    Error = ConnectError(Socket, "127.0.0.1");
    ok(Error == WSAEADDRNOTAVAIL, "bound to %s, connect to 127.0.0.1: error %d, expected %d\n", Interface, Error, WSAEADDRNOTAVAIL);
    Error = ConnectError(Socket, "127.5.5.5");
    ok(Error == WSAEADDRNOTAVAIL, "bound to %s, connect to 127.5.5.5: error %d, expected %d\n", Interface, Error, WSAEADDRNOTAVAIL);
    Error = ConnectError(Socket, "8.8.8.8");
    ok(Error == 0, "bound to %s, connect to 8.8.8.8: error %d\n", Interface, Error);
    Error = ConnectError(Socket, "224.0.0.251");
    ok(Error == 0, "bound to %s, connect to 224.0.0.251: error %d\n", Interface, Error);
    closesocket(Socket);
}

START_TEST(udpconnectname)
{
    static const char *const ExternalPeers[] = { "8.8.8.8", "255.255.255.255", "224.0.0.251", "169.254.7.7" };
    WSADATA WsaData;
    SOCKET Socket;
    struct sockaddr_in Name, Bound, Zero;
    char Interface[32] = "";
    int Error = 0, i, Length;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }

    CheckLoopbackPeer("127.0.0.1");
    CheckLoopbackPeer("127.5.5.5");

    for (i = 0; i < (int)(sizeof(ExternalPeers) / sizeof(ExternalPeers[0])); i++)
    {
        char What[64];

        sprintf(What, "connect to %s", ExternalPeers[i]);
        Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
        if (Socket == INVALID_SOCKET)
            continue;
        if (!ConnectTo(Socket, ExternalPeers[i], &Error))
        {
            skip("%s failed with %d: no route to it\n", What, Error);
            closesocket(Socket);
            continue;
        }
        if (LocalName(Socket, &Name, What))
        {
            ok(Name.sin_addr.s_addr != 0, "%s: local address is 0.0.0.0\n", What);
            ok(Name.sin_addr.s_addr != htonl(INADDR_LOOPBACK), "%s: local address is the loopback\n", What);
            ok(Name.sin_port != 0, "%s: local port is 0\n", What);
            if (!Interface[0])
                strcpy(Interface, inet_ntoa(Name.sin_addr));
            else
                ok(!strcmp(Interface, inet_ntoa(Name.sin_addr)), "%s: local address %s, expected %s\n",
                   What, inet_ntoa(Name.sin_addr), Interface);
        }
        closesocket(Socket);
    }

    if (!Interface[0])
    {
        skip("no peer outside the host is reachable\n");
        WSACleanup();
        return;
    }

    /* Connecting to the interface's own address keeps it */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Socket != INVALID_SOCKET)
    {
        ok(ConnectTo(Socket, Interface, &Error), "connect to %s failed: %d\n", Interface, Error);
        if (LocalName(Socket, &Name, "connect to the interface address"))
            ok(!strcmp(inet_ntoa(Name.sin_addr), Interface), "local address %s, expected %s\n",
               inet_ntoa(Name.sin_addr), Interface);
        closesocket(Socket);
    }

    /* A socket bound to the wildcard address and a port keeps the port, and the address follows
     * every reconnect; disconnecting returns it to the wildcard address */
    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Socket != INVALID_SOCKET)
    {
        ZeroMemory(&Bound, sizeof(Bound));
        Bound.sin_family = AF_INET;
        ok(bind(Socket, (struct sockaddr *)&Bound, sizeof(Bound)) == 0, "bind failed: %d\n", WSAGetLastError());
        Length = sizeof(Bound);
        getsockname(Socket, (struct sockaddr *)&Bound, &Length);
        ok(Bound.sin_addr.s_addr == 0 && Bound.sin_port != 0, "bound name %s:%u\n",
           inet_ntoa(Bound.sin_addr), ntohs(Bound.sin_port));

        ok(ConnectTo(Socket, "127.0.0.1", &Error), "connect to 127.0.0.1 failed: %d\n", Error);
        if (LocalName(Socket, &Name, "bound, connect to 127.0.0.1"))
        {
            ok(Name.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "local address %s, expected 127.0.0.1\n",
               inet_ntoa(Name.sin_addr));
            ok(Name.sin_port == Bound.sin_port, "local port %u, expected %u\n",
               ntohs(Name.sin_port), ntohs(Bound.sin_port));
        }

        ok(ConnectTo(Socket, "8.8.8.8", &Error), "reconnect to 8.8.8.8 failed: %d\n", Error);
        if (LocalName(Socket, &Name, "reconnect to 8.8.8.8"))
        {
            ok(!strcmp(inet_ntoa(Name.sin_addr), Interface), "local address %s, expected %s\n",
               inet_ntoa(Name.sin_addr), Interface);
            ok(Name.sin_port == Bound.sin_port, "local port %u, expected %u\n",
               ntohs(Name.sin_port), ntohs(Bound.sin_port));
        }

        ZeroMemory(&Zero, sizeof(Zero));
        ok(connect(Socket, (struct sockaddr *)&Zero, sizeof(Zero)) == 0, "disconnect failed: %d\n",
           WSAGetLastError());
        if (LocalName(Socket, &Name, "after disconnect"))
        {
            ok(Name.sin_addr.s_addr == 0, "local address %s, expected 0.0.0.0\n", inet_ntoa(Name.sin_addr));
            ok(Name.sin_port == Bound.sin_port, "local port %u, expected %u\n",
               ntohs(Name.sin_port), ntohs(Bound.sin_port));
        }
        closesocket(Socket);
    }

    CheckErrors(Interface);

    WSACleanup();
}
