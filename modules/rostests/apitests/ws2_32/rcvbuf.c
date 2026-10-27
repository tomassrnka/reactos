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
 * ERR_SOCKET_RECEIVE_BUFFER_SIZE_UNCHANGEABLE (net error -348), aborting the
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
    static const INT Sizes[] = { 4096, 8192, 32768, 65536, 131072, 262144, 1048576 };
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
         * requested (Windows may round up, never down, and never reports the
         * option as unchangeable). Compare against the saved request, not the
         * (possibly mutated) option buffer. */
        ok(ReadBack >= Requested,
           "%s: set %d but read back %d (< requested) -- this is the -348 trigger\n",
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

static
VOID
TestBoundUdp(VOID)
{
    SOCKET Sock;
    SOCKADDR_IN Addr;
    INT Result;
    const INT Requested = 1048576;
    INT Value = Requested;
    INT ReadBack = -1;
    INT Length = sizeof(ReadBack);

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

    Result = setsockopt(Sock, SOL_SOCKET, SO_RCVBUF, (PCHAR)&Value, sizeof(Value));
    ok(Result == 0, "bound-udp: setsockopt(SO_RCVBUF,%d) failed, error %d\n", Requested, WSAGetLastError());
    ok(Value == Requested, "bound-udp: setsockopt mutated the caller's buffer: %d -> %d\n", Requested, Value);

    Result = getsockopt(Sock, SOL_SOCKET, SO_RCVBUF, (PCHAR)&ReadBack, &Length);
    ok(Result == 0, "bound-udp: getsockopt(SO_RCVBUF) failed, error %d\n", WSAGetLastError());
    ok(ReadBack >= Requested,
       "bound-udp: set SO_RCVBUF %d but read back %d (< requested) -- the -UNCHANGEABLE trigger\n",
       Requested, ReadBack);

    closesocket(Sock);
}

static
VOID
TestConnectedUdp(VOID)
{
    SOCKET Sock;
    SOCKADDR_IN Addr;
    INT Result;
    const INT Requested = 1048576;
    INT Value = Requested;
    INT ReadBack = -1;
    INT Length = sizeof(ReadBack);

    /* A connected datagram socket is the shape Chrome's QUIC uses (connect to
     * the server, then size the receive buffer). Exercise that exact order. */
    Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sock != INVALID_SOCKET, "conn-udp: socket failed, error %d\n", WSAGetLastError());
    if (Sock == INVALID_SOCKET)
        return;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    Addr.sin_port = htons(443);
    Result = connect(Sock, (SOCKADDR *)&Addr, sizeof(Addr));
    ok(Result == 0, "conn-udp: connect failed, error %d\n", WSAGetLastError());

    Result = setsockopt(Sock, SOL_SOCKET, SO_RCVBUF, (PCHAR)&Value, sizeof(Value));
    ok(Result == 0, "conn-udp: setsockopt(SO_RCVBUF,%d) failed, error %d\n", Requested, WSAGetLastError());
    ok(Value == Requested, "conn-udp: setsockopt mutated the caller's buffer: %d -> %d\n", Requested, Value);

    Result = getsockopt(Sock, SOL_SOCKET, SO_RCVBUF, (PCHAR)&ReadBack, &Length);
    ok(Result == 0, "conn-udp: getsockopt(SO_RCVBUF) failed, error %d\n", WSAGetLastError());
    ok(ReadBack >= Requested,
       "conn-udp: set SO_RCVBUF %d but read back %d (< requested)\n",
       Requested, ReadBack);

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

    WSACleanup();
}
