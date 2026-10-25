/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Contract test: unbound raw AFD datagram sendto must not bugcheck
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

/*
 * A raw AFD datagram socket that is deliberately NEVER bound. Winsock does an
 * implicit bind before sendto, so it never exercised this path; a raw AFD
 * sendto reaches AfdPacketSocketWriteData in SOCKET_STATE_CREATED and drives
 * AFD's own implicit bind. That bind used to warm the socket without building
 * FCB->AddressFrom, yet WarmSocketForBind pre-posts a receive datagram with it,
 * and tcpip!DGReceiveDatagram dereferences it (ConnInfo->RemoteAddressLength,
 * offset 0x20) -> NULL deref -> bugcheck. Reachable unprivileged == local DoS
 * (T134). The contract: AfdSendTo must send or fail with a STATUS, never crash.
 */
static
void
TestSendToUnbound(void)
{
    NTSTATUS Status;
    HANDLE SocketHandle;
    CHAR Buffer[32];
    struct sockaddr_in addr;

    RtlZeroMemory(Buffer, sizeof(Buffer));

    Status = AfdCreateSocket(&SocketHandle, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed with %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(9); /* discard */

    /* No AfdBind on purpose: this is the implicit-bind path. Reaching the
     * assertion at all (instead of bugchecking) is the regression signal. */
    Status = AfdSendTo(SocketHandle, Buffer, sizeof(Buffer), (const struct sockaddr *)&addr, sizeof(addr));
    ok(Status == STATUS_SUCCESS, "Unbound AfdSendTo failed with %lx\n", Status);

    /* The socket is now implicitly bound: a second send must stay graceful. */
    Status = AfdSendTo(SocketHandle, Buffer, sizeof(Buffer), (const struct sockaddr *)&addr, sizeof(addr));
    ok(Status == STATUS_SUCCESS, "Second unbound AfdSendTo failed with %lx\n", Status);

    NtClose(SocketHandle);
}

START_TEST(sendtounbound)
{
    TestSendToUnbound();
}
