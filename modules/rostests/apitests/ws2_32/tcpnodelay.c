/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     getsockopt(IPPROTO_TCP, TCP_NODELAY): the value setsockopt stored, as a one byte BOOLEAN
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does: success with a one byte result (also for a
 * larger buffer, the bytes after it are left alone), the value 0 by default, setsockopt reads the
 * first byte only, a buffer under one byte is WSAEFAULT, a datagram socket is WSAEINVAL, an
 * accepted socket has the value of its listener.
 */

#include "ws2_32.h"

static int GetNoDelay(SOCKET s, int BufLen, BYTE *Out, int *OutLen)
{
    int Len = BufLen, Result;

    memset(Out, 0xcc, 8);
    Result = getsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)Out, &Len);
    *OutLen = Len;
    return Result;
}

static void CheckValue(SOCKET s, int Expected, const char *What)
{
    BYTE Value[8];
    int Len, Result;

    Result = GetNoDelay(s, 1, Value, &Len);
    ok(Result == 0, "%s: getsockopt(1) failed: %d\n", What, WSAGetLastError());
    ok(Len == 1, "%s: getsockopt(1) length %d\n", What, Len);
    ok(Value[0] == Expected, "%s: getsockopt(1) value %02x, expected %d\n", What, Value[0], Expected);

    Result = GetNoDelay(s, 4, Value, &Len);
    ok(Result == 0, "%s: getsockopt(4) failed: %d\n", What, WSAGetLastError());
    ok(Len == 1, "%s: getsockopt(4) length %d\n", What, Len);
    ok(Value[0] == Expected, "%s: getsockopt(4) value %02x, expected %d\n", What, Value[0], Expected);
    ok(Value[1] == 0xcc && Value[2] == 0xcc && Value[3] == 0xcc,
       "%s: getsockopt(4) wrote past the first byte: %02x %02x %02x\n", What, Value[1], Value[2], Value[3]);
}

START_TEST(tcpnodelay)
{
    WSADATA Data;
    SOCKET s, Listener, Client, Accepted, Udp;
    struct sockaddr_in Addr;
    BYTE Value[8];
    int Len, Result, Int;
    BYTE Bytes[4];

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");

    s = socket(AF_INET, SOCK_STREAM, 0);
    ok(s != INVALID_SOCKET, "socket failed\n");

    CheckValue(s, 0, "default");

    /* A buffer under one byte, and no buffer */
    Result = GetNoDelay(s, 0, Value, &Len);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "getsockopt(0): %d, error %d\n", Result, WSAGetLastError());
    Len = 1;
    Result = getsockopt(s, IPPROTO_TCP, TCP_NODELAY, NULL, &Len);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "getsockopt(NULL): %d, error %d\n", Result, WSAGetLastError());

    Int = 1;
    Result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, sizeof(Int));
    ok(Result == 0, "setsockopt(1) failed: %d\n", WSAGetLastError());
    CheckValue(s, 1, "set to 1");

    Int = 0;
    Result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, sizeof(Int));
    ok(Result == 0, "setsockopt(0) failed: %d\n", WSAGetLastError());
    CheckValue(s, 0, "set to 0");

    /* Any non-zero value reads back as 1 */
    Int = 5;
    Result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, sizeof(Int));
    ok(Result == 0, "setsockopt(5) failed: %d\n", WSAGetLastError());
    CheckValue(s, 1, "set to 5");

    /* Only the first byte counts */
    Int = 0x100;
    Result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, sizeof(Int));
    ok(Result == 0, "setsockopt(0x100) failed: %d\n", WSAGetLastError());
    CheckValue(s, 0, "set to 0x100");

    Bytes[0] = 1;
    Result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)Bytes, 1);
    ok(Result == 0, "setsockopt(one byte) failed: %d\n", WSAGetLastError());
    CheckValue(s, 1, "set with one byte");

    Bytes[0] = 0;
    Bytes[1] = 1;
    Result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)Bytes, 2);
    ok(Result == 0, "setsockopt(two bytes) failed: %d\n", WSAGetLastError());
    CheckValue(s, 0, "set with {0, 1}");

    /* An empty value */
    Int = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, sizeof(Int));
    Result = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, 0);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "setsockopt(length 0): %d, error %d\n", Result, WSAGetLastError());
    CheckValue(s, 1, "after a failed set");

    /* A listener's value goes to the sockets it accepts */
    Listener = socket(AF_INET, SOCK_STREAM, 0);
    memset(&Addr, 0, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed\n");
    Len = sizeof(Addr);
    ok(getsockname(Listener, (struct sockaddr *)&Addr, &Len) == 0, "getsockname failed\n");
    ok(listen(Listener, 1) == 0, "listen failed\n");
    Int = 1;
    Result = setsockopt(Listener, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, sizeof(Int));
    ok(Result == 0, "setsockopt on the listener failed: %d\n", WSAGetLastError());
    CheckValue(Listener, 1, "listener");
    Client = socket(AF_INET, SOCK_STREAM, 0);
    ok(connect(Client, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "connect failed: %d\n", WSAGetLastError());
    Accepted = accept(Listener, NULL, NULL);
    ok(Accepted != INVALID_SOCKET, "accept failed\n");
    CheckValue(Client, 0, "connected client");
    CheckValue(Accepted, 1, "accepted");
    closesocket(Accepted);
    closesocket(Client);
    closesocket(Listener);

    /* Not for a datagram socket */
    Udp = socket(AF_INET, SOCK_DGRAM, 0);
    Result = GetNoDelay(Udp, 1, Value, &Len);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "UDP getsockopt: %d, error %d\n", Result, WSAGetLastError());
    Int = 1;
    Result = setsockopt(Udp, IPPROTO_TCP, TCP_NODELAY, (char *)&Int, sizeof(Int));
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "UDP setsockopt: %d, error %d\n", Result, WSAGetLastError());
    closesocket(Udp);

    closesocket(s);
    WSACleanup();
}
