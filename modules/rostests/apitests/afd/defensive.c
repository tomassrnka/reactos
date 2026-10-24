/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Defensive/contract tests for the AFD T53 hardening
 *              (window-size SET_INFO use-after-free, METHOD_NEITHER length
 *              validation, transport-address bounds). These assert that
 *              malformed/oversized input is rejected and that the window
 *              buffer is never freed while a transport is attached. They are
 *              deliberately NOT weaponized race/UAF exploits.
 *              Also covers the T120 request validation: short/empty
 *              recv/send/listen/disconnect requests, sends with no WSABUFs
 *              or an unusable datagram destination are rejected, while
 *              ordinary loopback TCP/UDP I/O keeps working.
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

/* ---- T120: AFD send/recv/listen/disconnect request validation ----
 *
 * Contract: a request whose input buffer is missing or smaller than the
 * structure the handler reads, a send with no WSABUFs, or a datagram send
 * with an unusable destination, is rejected with a failure status and leaves
 * the socket usable. Ordinary socket I/O keeps working. */

static
NTSTATUS
AfdRawIoctlInfo(
    _In_ HANDLE Socket,
    _In_ ULONG Code,
    _In_opt_ PVOID In,
    _In_ ULONG InLen,
    _Out_ PULONG_PTR Information)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Event;

    *Information = 0;
    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    IoStatus.Status = STATUS_PENDING;
    IoStatus.Information = 0;
    Status = NtDeviceIoControlFile(Socket, Event, NULL, NULL, &IoStatus,
                                   Code, In, InLen, NULL, 0);
    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Event, FALSE, NULL);
        Status = IoStatus.Status;
    }
    *Information = IoStatus.Information;
    NtClose(Event);
    return Status;
}

/* Connected TCP pair over loopback; the sockets are plain winsock handles. */
static
BOOL
MakeTcpPair(SOCKET *Client, SOCKET *Server, SOCKET *Listener)
{
    struct sockaddr_in Addr;
    int Len = sizeof(Addr);

    *Client = *Server = *Listener = INVALID_SOCKET;
    *Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*Listener == INVALID_SOCKET || *Client == INVALID_SOCKET)
        return FALSE;

    RtlZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(*Listener, (struct sockaddr *)&Addr, sizeof(Addr)) != 0 ||
        getsockname(*Listener, (struct sockaddr *)&Addr, &Len) != 0 ||
        listen(*Listener, 1) != 0 ||
        connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr)) != 0)
        return FALSE;
    *Server = accept(*Listener, NULL, NULL);
    return *Server != INVALID_SOCKET;
}

static
void
TestShortRequestsRejected(void)
{
    static const ULONG Codes[] =
    {
        IOCTL_AFD_RECV, IOCTL_AFD_SEND, IOCTL_AFD_DISCONNECT
    };
    SOCKET Client, Server, Listener;
    NTSTATUS Status;
    ULONG_PTR Info;
    UCHAR Tiny[4] = { 0 };
    char Byte;
    UINT i;

    if (!MakeTcpPair(&Client, &Server, &Listener))
    {
        skip("loopback TCP pair unavailable\n");
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(Codes); i++)
    {
        Status = AfdRawIoctlInfo((HANDLE)Client, Codes[i], NULL, 0, &Info);
        ok(Status == STATUS_INVALID_PARAMETER,
           "ioctl %lx with no input buffer: got %lx\n", Codes[i], Status);

        Status = AfdRawIoctlInfo((HANDLE)Client, Codes[i], Tiny, sizeof(Tiny), &Info);
        ok(Status == STATUS_INVALID_PARAMETER,
           "ioctl %lx with a %u-byte input buffer: got %lx\n",
           Codes[i], (ULONG)sizeof(Tiny), Status);
    }

    /* Listen on the (bound) listener with a short request */
    Status = AfdRawIoctlInfo((HANDLE)Listener, IOCTL_AFD_START_LISTEN, Tiny, 1, &Info);
    ok(Status == STATUS_INVALID_PARAMETER,
       "LISTEN with a 1-byte input buffer: got %lx\n", Status);
    Status = AfdRawIoctlInfo((HANDLE)Listener, IOCTL_AFD_START_LISTEN, NULL, 0, &Info);
    ok(Status == STATUS_INVALID_PARAMETER,
       "LISTEN with no input buffer: got %lx\n", Status);

    /* The rejected requests must not have disturbed the connection */
    ok(send(Client, "x", 1, 0) == 1, "send after rejected requests failed: %d\n", WSAGetLastError());
    ok(recv(Server, &Byte, 1, 0) == 1 && Byte == 'x', "recv after rejected requests failed\n");

    closesocket(Client);
    closesocket(Server);
    closesocket(Listener);
}

static
void
TestSendBufferCounts(void)
{
    SOCKET Client, Server, Listener;
    NTSTATUS Status;
    ULONG_PTR Info;
    AFD_SEND_INFO SendInfo;
    AFD_WSABUF Bufs[2];
    char Data[4] = { 0 };

    if (!MakeTcpPair(&Client, &Server, &Listener))
    {
        skip("loopback TCP pair unavailable\n");
        return;
    }

    /* No WSABUFs at all */
    RtlZeroMemory(&SendInfo, sizeof(SendInfo));
    Bufs[0].buf = Data;
    Bufs[0].len = sizeof(Data);
    SendInfo.BufferArray = Bufs;
    SendInfo.BufferCount = 0;
    Status = AfdRawIoctlInfo((HANDLE)Client, IOCTL_AFD_SEND, &SendInfo, sizeof(SendInfo), &Info);
    ok(Status == STATUS_INVALID_PARAMETER, "SEND with BufferCount 0: got %lx\n", Status);

    /* A zero-length WSABUF in front of a real one is accepted and skipped */
    Bufs[0].buf = NULL;
    Bufs[0].len = 0;
    Bufs[1].buf = Data;
    Bufs[1].len = 3;
    memcpy(Data, "abc", 3);
    SendInfo.BufferArray = Bufs;
    SendInfo.BufferCount = 2;
    Status = AfdRawIoctlInfo((HANDLE)Client, IOCTL_AFD_SEND, &SendInfo, sizeof(SendInfo), &Info);
    ok(Status == STATUS_SUCCESS, "SEND with an empty leading WSABUF: got %lx\n", Status);
    ok(Info == 3, "SEND with an empty leading WSABUF sent %lu bytes\n", (ULONG)Info);
    Data[0] = Data[1] = Data[2] = 0;
    ok(recv(Server, Data, 3, 0) == 3 && !memcmp(Data, "abc", 3),
       "data after the empty WSABUF did not arrive intact\n");

    /* All buffers empty: succeeds with nothing sent */
    Bufs[1].buf = NULL;
    Bufs[1].len = 0;
    Status = AfdRawIoctlInfo((HANDLE)Client, IOCTL_AFD_SEND, &SendInfo, sizeof(SendInfo), &Info);
    ok(Status == STATUS_SUCCESS, "SEND with only empty WSABUFs: got %lx\n", Status);
    ok(Info == 0, "SEND with only empty WSABUFs reported %lu bytes\n", (ULONG)Info);

    closesocket(Client);
    closesocket(Server);
    closesocket(Listener);
}

static
void
TestDatagramSendValidation(void)
{
    SOCKET Rx, Tx;
    struct sockaddr_in Addr;
    int Len = sizeof(Addr);
    NTSTATUS Status;
    ULONG_PTR Info;
    AFD_SEND_INFO_UDP Udp;
    AFD_WSABUF Buf;
    UCHAR Ta[FIELD_OFFSET(TRANSPORT_ADDRESS, Address[0].Address) + 14];
    PTRANSPORT_ADDRESS Dest = (PTRANSPORT_ADDRESS)Ta;
    char Data[4] = "udp";
    char In[8] = { 0 };

    Rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Rx != INVALID_SOCKET && Tx != INVALID_SOCKET, "UDP sockets: %d\n", WSAGetLastError());
    if (Rx == INVALID_SOCKET || Tx == INVALID_SOCKET)
        return;

    RtlZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    ok(bind(Rx, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed\n");
    ok(getsockname(Rx, (struct sockaddr *)&Addr, &Len) == 0, "getsockname failed\n");
    {
        struct sockaddr_in Local;
        RtlZeroMemory(&Local, sizeof(Local));
        Local.sin_family = AF_INET;
        Local.sin_addr.s_addr = inet_addr("127.0.0.1");
        ok(bind(Tx, (struct sockaddr *)&Local, sizeof(Local)) == 0, "bind failed\n");
    }

    /* Destination as msafd builds it */
    RtlZeroMemory(Ta, sizeof(Ta));
    Dest->TAAddressCount = 1;
    Dest->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    Dest->Address[0].AddressLength = 14;
    RtlCopyMemory(&Dest->Address[0].Address, (PUCHAR)&Addr + FIELD_OFFSET(struct sockaddr, sa_data), 14);

    Buf.buf = Data;
    Buf.len = 3;
    RtlZeroMemory(&Udp, sizeof(Udp));
    Udp.BufferArray = &Buf;
    Udp.BufferCount = 1;
    Udp.TdiConnection.RemoteAddress = Dest;
    Udp.TdiConnection.RemoteAddressLength = sizeof(Ta);

    /* Short / empty requests */
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_SEND_DATAGRAM, NULL, 0, &Info);
    ok(Status == STATUS_INVALID_PARAMETER, "SEND_DATAGRAM with no input: got %lx\n", Status);
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_SEND_DATAGRAM, In, sizeof(In), &Info);
    ok(Status == STATUS_INVALID_PARAMETER, "SEND_DATAGRAM with a short input: got %lx\n", Status);
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_RECV_DATAGRAM, NULL, 0, &Info);
    ok(Status == STATUS_INVALID_PARAMETER, "RECV_DATAGRAM with no input: got %lx\n", Status);
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_RECV_DATAGRAM, In, sizeof(In), &Info);
    ok(Status == STATUS_INVALID_PARAMETER, "RECV_DATAGRAM with a short input: got %lx\n", Status);

    /* No WSABUFs */
    Udp.BufferCount = 0;
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_SEND_DATAGRAM, &Udp, sizeof(Udp), &Info);
    ok(Status == STATUS_INVALID_PARAMETER, "SEND_DATAGRAM with BufferCount 0: got %lx\n", Status);
    Udp.BufferCount = 1;

    /* Unusable destinations: none, too short for a transport address, unknown type */
    Udp.TdiConnection.RemoteAddress = NULL;
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_SEND_DATAGRAM, &Udp, sizeof(Udp), &Info);
    ok(Status != STATUS_SUCCESS, "SEND_DATAGRAM with a NULL destination: got %lx\n", Status);
    Udp.TdiConnection.RemoteAddress = Dest;

    Udp.TdiConnection.RemoteAddressLength = 4;
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_SEND_DATAGRAM, &Udp, sizeof(Udp), &Info);
    ok(Status == STATUS_INVALID_PARAMETER, "SEND_DATAGRAM with a 4-byte destination: got %lx\n", Status);
    Udp.TdiConnection.RemoteAddressLength = sizeof(Ta);

    Dest->Address[0].AddressType = 0x7fff;
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_SEND_DATAGRAM, &Udp, sizeof(Udp), &Info);
    ok(Status != STATUS_SUCCESS, "SEND_DATAGRAM with an unknown address type: got %lx\n", Status);
    Dest->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;

    /* The same well-formed request still delivers */
    Status = AfdRawIoctlInfo((HANDLE)Tx, IOCTL_AFD_SEND_DATAGRAM, &Udp, sizeof(Udp), &Info);
    ok(Status == STATUS_SUCCESS, "valid SEND_DATAGRAM: got %lx\n", Status);
    ok(Info == 3, "valid SEND_DATAGRAM sent %lu bytes\n", (ULONG)Info);
    {
        char Got[8] = { 0 };
        DWORD Timeout = 5000;
        setsockopt(Rx, SOL_SOCKET, SO_RCVTIMEO, (char *)&Timeout, sizeof(Timeout));
        ok(recvfrom(Rx, Got, sizeof(Got), 0, NULL, NULL) == 3 && !memcmp(Got, "udp", 3),
           "datagram did not arrive: %d\n", WSAGetLastError());
    }

    /* and so does ordinary sendto/recvfrom */
    ok(sendto(Tx, "hi", 2, 0, (struct sockaddr *)&Addr, sizeof(Addr)) == 2, "sendto failed: %d\n", WSAGetLastError());
    {
        char Got[8] = { 0 };
        ok(recvfrom(Rx, Got, sizeof(Got), 0, NULL, NULL) == 2 && !memcmp(Got, "hi", 2),
           "sendto datagram did not arrive: %d\n", WSAGetLastError());
    }

    closesocket(Rx);
    closesocket(Tx);
}

static
void
TestLoopbackIoUnregressed(void)
{
    SOCKET Client, Server, Listener;
    char Out[1024], In[1024];
    int Got, Total;
    UINT i;

    if (!MakeTcpPair(&Client, &Server, &Listener))
    {
        skip("loopback TCP pair unavailable\n");
        return;
    }
    for (i = 0; i < sizeof(Out); i++)
        Out[i] = (char)(i * 7);

    ok(send(Client, Out, sizeof(Out), 0) == (int)sizeof(Out), "send failed: %d\n", WSAGetLastError());
    for (Total = 0; Total < (int)sizeof(In); Total += Got)
    {
        Got = recv(Server, In + Total, sizeof(In) - Total, 0);
        if (Got <= 0)
            break;
    }
    ok(Total == (int)sizeof(Out) && !memcmp(In, Out, sizeof(Out)), "loopback payload mismatch (%d)\n", Total);

    ok(shutdown(Client, SD_SEND) == 0, "shutdown failed: %d\n", WSAGetLastError());
    ok(recv(Server, In, sizeof(In), 0) == 0, "expected orderly close\n");

    closesocket(Client);
    closesocket(Server);
    closesocket(Listener);
}

START_TEST(defensive)
{
    WSADATA WsaData;
    TestEnumShortBuffer();
    TestInfoShortBuffer();
    TestBindOverlongAddress();
    TestWindowResizeOnLiveSocketIsNoOp();
    TestValidOpsStillWork();

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }
    TestShortRequestsRejected();
    TestSendBufferCounts();
    TestDatagramSendValidation();
    TestLoopbackIoUnregressed();
    WSACleanup();
}
