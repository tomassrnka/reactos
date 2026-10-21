/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Defensive/contract tests for the AFD T53 hardening
 *              (window-size SET_INFO use-after-free, METHOD_NEITHER length
 *              validation, transport-address bounds). These assert that
 *              malformed/oversized input is rejected and that the window
 *              buffer is never freed while a transport is attached. They are
 *              deliberately NOT weaponized race/UAF exploits.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

static
NTSTATUS
AfdRawIoctl(
    _In_ HANDLE Socket,
    _In_ ULONG Code,
    _In_opt_ PVOID In,
    _In_ ULONG InLen,
    _In_opt_ PVOID Out,
    _In_ ULONG OutLen)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Event;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    IoStatus.Status = STATUS_PENDING;
    Status = NtDeviceIoControlFile(Socket, Event, NULL, NULL, &IoStatus,
                                   Code, In, InLen, Out, OutLen);
    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Event, FALSE, NULL);
        Status = IoStatus.Status;
    }

    NtClose(Event);
    return Status;
}

/* #5: AFD_ENUM_NETWORK_EVENTS with an input buffer far smaller than
 * AFD_ENUM_NETWORK_EVENTS_INFO must be rejected. Before the fix the native
 * path copied only InputBufferLength bytes into a pool block and then wrote
 * PollEvents + the whole EventStatus array past it (NonPagedPool overflow). */
static
void
TestEnumShortBuffer(void)
{
    NTSTATUS Status;
    HANDLE Socket;
    UCHAR Tiny[4] = { 0, 0, 0, 0 };

    Status = AfdCreateSocket(&Socket, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed: %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    Status = AfdRawIoctl(Socket, IOCTL_AFD_ENUM_NETWORK_EVENTS,
                         Tiny, sizeof(Tiny), Tiny, sizeof(Tiny));
    ok(Status != STATUS_SUCCESS,
       "ENUM_NETWORK_EVENTS with a %u-byte buffer must be rejected, got %lx\n",
       (ULONG)sizeof(Tiny), Status);

    NtClose(Socket);
}

/* #5/#6: SET_INFO and GET_INFO (METHOD_NEITHER) with an input smaller than
 * AFD_INFO must be rejected, not read/write past the captured pool copy. */
static
void
TestInfoShortBuffer(void)
{
    NTSTATUS Status;
    HANDLE Socket;
    UCHAR Tiny[4] = { 0, 0, 0, 0 };

    Status = AfdCreateSocket(&Socket, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed: %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    Status = AfdRawIoctl(Socket, IOCTL_AFD_SET_INFO, Tiny, sizeof(Tiny), NULL, 0);
    ok(Status != STATUS_SUCCESS,
       "SET_INFO with a %u-byte buffer must be rejected, got %lx\n",
       (ULONG)sizeof(Tiny), Status);

    Status = AfdRawIoctl(Socket, IOCTL_AFD_GET_INFO,
                         Tiny, sizeof(Tiny), Tiny, sizeof(Tiny));
    ok(Status != STATUS_SUCCESS,
       "GET_INFO with a %u-byte buffer must be rejected, got %lx\n",
       (ULONG)sizeof(Tiny), Status);

    NtClose(Socket);
}

/* #7: BIND whose embedded TRANSPORT_ADDRESS claims an AddressLength that runs
 * past the supplied InputBufferLength must be rejected. Before the fix
 * TaCopyTransportAddress trusted the self-described length and over-read the
 * adjacent pool into FCB->LocalAddress (info leak via getsockname). */
static
void
TestBindOverlongAddress(void)
{
    NTSTATUS Status;
    HANDLE Socket;
    UCHAR Buffer[FIELD_OFFSET(AFD_BIND_DATA, Address.Address[0].Address) + 4];
    PAFD_BIND_DATA Bind = (PAFD_BIND_DATA)Buffer;

    Status = AfdCreateSocket(&Socket, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed: %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    RtlZeroMemory(Buffer, sizeof(Buffer));
    Bind->ShareType = AFD_SHARE_UNIQUE;
    Bind->Address.TAAddressCount = 1;
    Bind->Address.Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    /* Lie: claim a 0x200-byte address while the buffer holds only 4 bytes. */
    Bind->Address.Address[0].AddressLength = 0x200;

    Status = AfdRawIoctl(Socket, IOCTL_AFD_BIND,
                         Buffer, sizeof(Buffer), Buffer, sizeof(Buffer));
    ok(Status != STATUS_SUCCESS,
       "BIND with an overlong AddressLength must be rejected, got %lx\n", Status);

    NtClose(Socket);
}

/* #8 (UAF): bind a datagram socket (a datagram receive goes in flight,
 * referencing Recv.Window), then resize the receive/send window. The fix
 * refuses to free the live buffer, so the SET succeeds as a no-op (the size is
 * unchanged) and nothing is corrupted. This exercises the former UAF path as a
 * safe contract check rather than winning a write-after-free race. */
static
void
TestWindowResizeOnLiveSocketIsNoOp(void)
{
    NTSTATUS Status;
    HANDLE Socket;
    struct sockaddr_in Addr;
    ULONG OrigRecv = 0, OrigSend = 0, Recv = 0, Send = 0;

    Status = AfdCreateSocket(&Socket, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed: %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    RtlZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    Addr.sin_port = htons(0);

    Status = AfdBind(Socket, (const struct sockaddr *)&Addr, sizeof(Addr));
    ok(Status == STATUS_SUCCESS, "AfdBind failed: %lx\n", Status);

    Status = AfdGetInformation(Socket, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &OrigRecv, NULL);
    ok(Status == STATUS_SUCCESS, "get recv size failed: %lx\n", Status);
    Status = AfdGetInformation(Socket, AFD_INFO_SEND_WINDOW_SIZE, NULL, &OrigSend, NULL);
    ok(Status == STATUS_SUCCESS, "get send size failed: %lx\n", Status);

    /* The former UAF trigger: free/realloc the window while the transport holds
     * it. The fix makes this a no-op on a bound socket. */
    Recv = OrigRecv + 0x1000;
    Status = AfdSetInformation(Socket, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &Recv, NULL);
    ok(Status == STATUS_SUCCESS, "set recv size failed: %lx\n", Status);
    Send = OrigSend + 0x1000;
    Status = AfdSetInformation(Socket, AFD_INFO_SEND_WINDOW_SIZE, NULL, &Send, NULL);
    ok(Status == STATUS_SUCCESS, "set send size failed: %lx\n", Status);

    /* Size must be unchanged: the live buffer was not freed/reallocated. */
    Status = AfdGetInformation(Socket, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &Recv, NULL);
    ok(Status == STATUS_SUCCESS, "re-get recv size failed: %lx\n", Status);
    ok(Recv == OrigRecv, "recv window was resized on a live socket: %lu -> %lu\n", OrigRecv, Recv);
    Status = AfdGetInformation(Socket, AFD_INFO_SEND_WINDOW_SIZE, NULL, &Send, NULL);
    ok(Status == STATUS_SUCCESS, "re-get send size failed: %lx\n", Status);
    ok(Send == OrigSend, "send window was resized on a live socket: %lu -> %lu\n", OrigSend, Send);

    NtClose(Socket);
}

/* Valid operations must still work after the hardening. */
static
void
TestValidOpsStillWork(void)
{
    NTSTATUS Status;
    HANDLE Socket;
    ULONG Size = 0;
    BOOLEAN Blocking = TRUE;

    Status = AfdCreateSocket(&Socket, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Status == STATUS_SUCCESS, "AfdCreateSocket failed: %lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    Status = AfdGetInformation(Socket, AFD_INFO_RECEIVE_WINDOW_SIZE, NULL, &Size, NULL);
    ok(Status == STATUS_SUCCESS, "get recv window failed: %lx\n", Status);
    ok(Size != 0, "recv window size unexpectedly 0\n");

    Status = AfdSetInformation(Socket, AFD_INFO_BLOCKING_MODE, &Blocking, NULL, NULL);
    ok(Status == STATUS_SUCCESS, "set blocking mode failed: %lx\n", Status);

    NtClose(Socket);
}

START_TEST(defensive)
{
    TestEnumShortBuffer();
    TestInfoShortBuffer();
    TestBindOverlongAddress();
    TestWindowResizeOnLiveSocketIsNoOp();
    TestValidOpsStillWork();
}
