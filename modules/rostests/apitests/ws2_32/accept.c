/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for accept
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

static
VOID
TestNonBlockingAcceptNothingPending(VOID)
{
    SOCKET ListenSocket, s;
    struct sockaddr_in Addr;
    INT AddrLen;
    ULONG NonBlocking = 1;
    INT Error;

    ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(ListenSocket != INVALID_SOCKET, "socket failed with %d\n", WSAGetLastError());
    if (ListenSocket == INVALID_SOCKET)
        return;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Addr.sin_port = 0;

    /* A blocking accept would wait forever, so stop on any setup failure */
    Error = bind(ListenSocket, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Error == 0, "bind failed with %d\n", WSAGetLastError());
    if (Error == 0)
    {
        Error = listen(ListenSocket, SOMAXCONN);
        ok(Error == 0, "listen failed with %d\n", WSAGetLastError());
    }
    if (Error == 0)
    {
        Error = ioctlsocket(ListenSocket, FIONBIO, &NonBlocking);
        ok(Error == 0, "ioctlsocket failed with %d\n", WSAGetLastError());
    }
    if (Error != 0)
    {
        closesocket(ListenSocket);
        return;
    }

    /* No client: the call must fail with the full-width INVALID_SOCKET */
    WSASetLastError(0xdeadbeef);
    s = accept(ListenSocket, NULL, NULL);
    ok(s == INVALID_SOCKET, "accept returned 0x%Ix\n", (ULONG_PTR)s);
    ok(WSAGetLastError() == WSAEWOULDBLOCK, "WSAGetLastError() = %d\n", WSAGetLastError());
    if (s != INVALID_SOCKET)
        closesocket(s);

    WSASetLastError(0xdeadbeef);
    AddrLen = sizeof(Addr);
    s = accept(ListenSocket, (struct sockaddr *)&Addr, &AddrLen);
    ok(s == INVALID_SOCKET, "accept returned 0x%Ix\n", (ULONG_PTR)s);
    ok(WSAGetLastError() == WSAEWOULDBLOCK, "WSAGetLastError() = %d\n", WSAGetLastError());
    if (s != INVALID_SOCKET)
        closesocket(s);

    WSASetLastError(0xdeadbeef);
    s = WSAAccept(ListenSocket, NULL, NULL, NULL, 0);
    ok(s == INVALID_SOCKET, "WSAAccept returned 0x%Ix\n", (ULONG_PTR)s);
    ok(WSAGetLastError() == WSAEWOULDBLOCK, "WSAGetLastError() = %d\n", WSAGetLastError());
    if (s != INVALID_SOCKET)
        closesocket(s);

    closesocket(ListenSocket);
}

START_TEST(accept)
{
    WSADATA WsaData;
    INT Error;

    Error = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Error == 0, "WSAStartup failed with %d\n", Error);
    if (Error != 0)
        return;

    TestNonBlockingAcceptNothingPending();

    WSACleanup();
}
