/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SO_RCVBUF / SO_SNDBUF accept-and-read-back contract
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Windows treats SO_RCVBUF / SO_SNDBUF as advisory hints: setsockopt accepts
 * the value and getsockopt reads it back (possibly rounded up), it is never
 * reported as "unchangeable". Chrome's QUIC code relies on exactly this: it
 * sets SO_RCVBUF on a UDP socket, reads it back, and if the read-back is
 * smaller than requested it fails the socket with
 * ERR_SOCKET_RECEIVE_BUFFER_SIZE_UNCHANGEABLE (net error -162), aborting the
 * QUIC handshake. These tests pin the contract that makes that path work.
 */

#include "ws2_32.h"

static
VOID
TestBufOption(
    _In_ SOCKET Sock,
    _In_ INT Option,
    _In_ PCSTR Name)
{
    INT Result;
    INT Value;
    INT ReadBack;
    INT Length;
    /* A representative spread: below/at the old 0x2000 clamp, inside the
     * 0x2001..0xFFFE range AFD used to cap, and the large sizes Chrome's QUIC
     * asks for. */
    static const INT Sizes[] = { 0, 4096, 8192, 32768, 65536, 131072, 262144, 1048576,
                                 2097152, 4194304, 8388608, 16777216 };
    SIZE_T i;

    /* The default must read back as a sane, non-zero size without error. */
    ReadBack = -1;
    Length = sizeof(ReadBack);
    Result = getsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&ReadBack, &Length);
    ok(Result == 0, "%s: default getsockopt failed, error %d\n", Name, WSAGetLastError());
    ok(Length == sizeof(ReadBack), "%s: default length %d\n", Name, Length);
    ok(ReadBack > 0, "%s: default read back %d\n", Name, ReadBack);

    for (i = 0; i < RTL_NUMBER_OF(Sizes); i++)
    {
        INT Requested = Sizes[i];

        Value = Requested;
        Result = setsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&Value, sizeof(Value));
        ok(Result == 0, "%s: setsockopt(%d) failed, error %d\n", Name, Requested, WSAGetLastError());

        /* setsockopt must not write through the caller's (const) option buffer.
         * The old msafd SO_RCVBUF clamp did exactly that (*optval = 0x2000),
         * which both corrupts the caller and masks the read-back gap. */
        ok(Value == Requested,
           "%s: setsockopt mutated the caller's buffer: %d -> %d\n",
           Name, Requested, Value);

        ReadBack = -1;
        Length = sizeof(ReadBack);
        Result = getsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&ReadBack, &Length);
        ok(Result == 0, "%s: getsockopt after set %d failed, error %d\n", Name, Requested, WSAGetLastError());
        ok(Length == sizeof(ReadBack), "%s: length %d after set %d\n", Name, Length, Requested);

        /* The contract Chrome depends on: the read-back is at least what was
         * requested (never smaller, never reported as unchangeable: the -162 and
         * -163 trigger). The provider records the hint as given, so it must be exactly
         * the request: a rounded value would show up here. Compare against the
         * saved request, not the (possibly mutated) option buffer. */
        ok(ReadBack >= Requested,
           "%s: set %d but read back only %d (Chrome: unchangeable)\n",
           Name, Requested, ReadBack);
        ok(ReadBack == Requested,
           "%s: set %d but read back %d\n",
           Name, Requested, ReadBack);
    }
}

static
VOID
TestSocket(
    _In_ INT Type,
    _In_ INT Protocol,
    _In_ PCSTR Label)
{
    SOCKET Sock;
    CHAR Name[64];

    Sock = socket(AF_INET, Type, Protocol);
    ok(Sock != INVALID_SOCKET, "%s: socket failed, error %d\n", Label, WSAGetLastError());
    if (Sock == INVALID_SOCKET)
        return;

    sprintf(Name, "%s SO_RCVBUF", Label);
    TestBufOption(Sock, SO_RCVBUF, Name);
    sprintf(Name, "%s SO_SNDBUF", Label);
    TestBufOption(Sock, SO_SNDBUF, Name);

    closesocket(Sock);
}

/* Set and read back one buffer option of a socket that already has a transport. */
static
VOID
TestSocketBuffer(
    _In_ SOCKET Sock,
    _In_ INT Option,
    _In_ PCSTR Label)
{
    INT Result;
    const INT Requested = 1048576;
    INT Value = Requested;
    INT ReadBack = -1;
    INT Length = sizeof(ReadBack);
    PCSTR OptName = (Option == SO_RCVBUF) ? "SO_RCVBUF" : "SO_SNDBUF";

    Result = setsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&Value, sizeof(Value));
    ok(Result == 0, "%s: setsockopt(%s,%d) failed, error %d\n", Label, OptName, Requested, WSAGetLastError());
    ok(Value == Requested, "%s: setsockopt mutated the caller's buffer: %d -> %d\n", Label, Requested, Value);

    Result = getsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&ReadBack, &Length);
    ok(Result == 0, "%s: getsockopt(%s) failed, error %d\n", Label, OptName, WSAGetLastError());
    ok(ReadBack == Requested,
       "%s: set %s %d but read back %d\n", Label, OptName, Requested, ReadBack);
}

static
VOID
TestBoundUdp(VOID)
{
    SOCKET Sock;
    SOCKADDR_IN Addr;
    INT Result;

    /* The real Chrome path sets SO_RCVBUF on a *bound* UDP socket (a transport
     * object is attached). That is exactly the case AFD refuses to resize, so
     * it is the one the old code regressed on. */
    Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sock != INVALID_SOCKET, "bound-udp: socket failed, error %d\n", WSAGetLastError());
    if (Sock == INVALID_SOCKET)
        return;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_ANY);
    Addr.sin_port = 0;
    Result = bind(Sock, (SOCKADDR *)&Addr, sizeof(Addr));
    ok(Result == 0, "bound-udp: bind failed, error %d\n", WSAGetLastError());

    TestSocketBuffer(Sock, SO_RCVBUF, "bound-udp");
    TestSocketBuffer(Sock, SO_SNDBUF, "bound-udp");

    closesocket(Sock);
}

static
VOID
TestConnectedUdp(VOID)
{
    SOCKET Sock;
    SOCKADDR_IN Addr;
    INT Result;

    /* A connected datagram socket is the shape Chrome's QUIC uses (connect to
     * the server, then size the receive buffer). Exercise that exact order.
     * The peer is the loopback address: connecting a datagram socket only
     * records the peer, and this works on a machine without a route out. */
    Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sock != INVALID_SOCKET, "conn-udp: socket failed, error %d\n", WSAGetLastError());
    if (Sock == INVALID_SOCKET)
        return;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Addr.sin_port = htons(443);
    Result = connect(Sock, (SOCKADDR *)&Addr, sizeof(Addr));
    ok(Result == 0, "conn-udp: connect failed, error %d\n", WSAGetLastError());

    TestSocketBuffer(Sock, SO_RCVBUF, "conn-udp");
    TestSocketBuffer(Sock, SO_SNDBUF, "conn-udp");

    closesocket(Sock);
}

static
VOID
TestEdgeValues(
    _In_ INT Option,
    _In_ PCSTR Name)
{
    SOCKET Sock;
    INT Result;
    INT Value;
    INT ReadBack;
    INT Length;
    INT Previous;
    static const UINT Edges[] = { 0, 0xFFFFFFFF };
    SIZE_T i;

    Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sock != INVALID_SOCKET, "%s edge: socket failed, error %d\n", Name, WSAGetLastError());
    if (Sock == INVALID_SOCKET)
        return;

    /* The hint is stored as an unsigned value: 0xFFFFFFFF reads back through
     * the signed INT of getsockopt as -1. */
    for (i = 0; i < RTL_NUMBER_OF(Edges); i++)
    {
        Value = (INT)Edges[i];
        Result = setsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&Value, sizeof(Value));
        ok(Result == 0, "%s edge: setsockopt(0x%x) failed, error %d\n", Name, Edges[i], WSAGetLastError());
        ok(Value == (INT)Edges[i], "%s edge: setsockopt mutated the caller's buffer\n", Name);

        ReadBack = 0x55555555;
        Length = sizeof(ReadBack);
        Result = getsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&ReadBack, &Length);
        ok(Result == 0, "%s edge: getsockopt after set 0x%x failed, error %d\n", Name, Edges[i], WSAGetLastError());
        ok(ReadBack == (INT)Edges[i], "%s edge: set 0x%x but read back 0x%x\n", Name, Edges[i], ReadBack);
    }

    /* A length shorter than the option, or a negative one, is refused and does
     * not change the stored value. */
    Value = 12345;
    Result = setsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&Value, sizeof(Value));
    ok(Result == 0, "%s edge: setsockopt(12345) failed, error %d\n", Name, WSAGetLastError());

    Value = 54321;
    WSASetLastError(0);
    Result = setsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&Value, sizeof(Value) - 1);
    ok(Result == SOCKET_ERROR, "%s edge: short optlen: setsockopt returned %d\n", Name, Result);
    if (Result == SOCKET_ERROR)
        ok(WSAGetLastError() == WSAEFAULT, "%s edge: short optlen: error %d\n", Name, WSAGetLastError());

    WSASetLastError(0);
    Result = setsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&Value, -1);
    ok(Result == SOCKET_ERROR, "%s edge: negative optlen: setsockopt returned %d\n", Name, Result);
    if (Result == SOCKET_ERROR)
        ok(WSAGetLastError() == WSAEFAULT, "%s edge: negative optlen: error %d\n", Name, WSAGetLastError());

    Previous = -1;
    Length = sizeof(Previous);
    Result = getsockopt(Sock, SOL_SOCKET, Option, (PCHAR)&Previous, &Length);
    ok(Result == 0, "%s edge: getsockopt failed, error %d\n", Name, WSAGetLastError());
    ok(Previous == 12345, "%s edge: a refused setsockopt changed the value to %d\n", Name, Previous);

    closesocket(Sock);
}

START_TEST(rcvbuf)
{
    WSADATA wsaData;
    INT Result;

    Result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    ok(Result == 0, "WSAStartup failed, error %d\n", Result);
    if (Result != 0)
        return;

    TestSocket(SOCK_DGRAM, IPPROTO_UDP, "udp");
    TestSocket(SOCK_STREAM, IPPROTO_TCP, "tcp");
    TestBoundUdp();
    TestConnectedUdp();
    TestEdgeValues(SO_RCVBUF, "SO_RCVBUF");
    TestEdgeValues(SO_SNDBUF, "SO_SNDBUF");

    WSACleanup();
}
