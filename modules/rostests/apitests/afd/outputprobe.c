/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Contract tests for the METHOD_NEITHER outputs of AFD that write
 *              to the caller's raw output pointer (GET_TDI_HANDLES and
 *              GET_{CONNECT,DISCONNECT}_{DATA,OPTIONS}): a kernel address or
 *              an unmapped or read-only user page fails the request with
 *              STATUS_ACCESS_VIOLATION and the socket stays usable.
 *              Also: connect and disconnect on a socket with connect and
 *              disconnect data/options set (the completion used to copy the
 *              options from a NULL buffer).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

static
NTSTATUS
OpIoctl(
    _In_ HANDLE Socket,
    _In_ ULONG Code,
    _In_opt_ PVOID In,
    _In_ ULONG InLen,
    _In_opt_ PVOID Out,
    _In_ ULONG OutLen,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Event;
    LARGE_INTEGER Timeout;

    if (Information)
        *Information = (ULONG_PTR)-1;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    IoStatus.Status = STATUS_PENDING;
    IoStatus.Information = (ULONG_PTR)-1;
    Status = NtDeviceIoControlFile(Socket, Event, NULL, NULL, &IoStatus,
                                   Code, In, InLen, Out, OutLen);
    if (Status == STATUS_PENDING)
    {
        Timeout.QuadPart = -10000000LL * 10;
        if (NtWaitForSingleObject(Event, FALSE, &Timeout) != STATUS_WAIT_0)
        {
            NtCancelIoFile(Socket, &IoStatus);
            NtWaitForSingleObject(Event, FALSE, NULL);
        }
        Status = IoStatus.Status;
    }
    if (Information && !NT_ERROR(Status))
        *Information = IoStatus.Information;

    NtClose(Event);
    return Status;
}

/* Bad output pointers: an unmapped (reserved, not committed) page, a read-only
 * page, and a kernel address (on amd64 a non-canonical one). The unmapped page
 * goes first: before the fix a raw kernel write to it stops the test right
 * there, and never reaches the kernel address. */
typedef struct _BAD_OUTPUT
{
    PCSTR Name;
    PVOID Pointer;
} BAD_OUTPUT;

static BAD_OUTPUT BadOutputs[3];
static PVOID ReservedPage, ReadOnlyPage;

static
BOOL
SetupBadOutputs(void)
{
    ReservedPage = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    ReadOnlyPage = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    ok(ReservedPage != NULL && ReadOnlyPage != NULL, "VirtualAlloc failed %lu\n", GetLastError());
    if (!ReservedPage || !ReadOnlyPage)
        return FALSE;

    BadOutputs[0].Name = "unmapped";
    BadOutputs[0].Pointer = ReservedPage;
    BadOutputs[1].Name = "read-only";
    BadOutputs[1].Pointer = ReadOnlyPage;
    BadOutputs[2].Name = "kernel";
#ifdef _WIN64
    BadOutputs[2].Pointer = (PVOID)(ULONG_PTR)0x8000000000000000ULL;
#else
    /* A kernel address natively; under WoW64 an unmapped address of the 64-bit
       user space, which faults inside the kernel's SEH instead */
    BadOutputs[2].Pointer = (PVOID)(ULONG_PTR)0xFFFFF000UL;
#endif
    return TRUE;
}

static
void
FreeBadOutputs(void)
{
    if (ReservedPage)
        VirtualFree(ReservedPage, 0, MEM_RELEASE);
    if (ReadOnlyPage)
        VirtualFree(ReadOnlyPage, 0, MEM_RELEASE);
    ReservedPage = ReadOnlyPage = NULL;
}

static
SOCKET
BoundTcpSocket(void)
{
    struct sockaddr_in a;
    SOCKET s;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(s != INVALID_SOCKET, "socket failed %d\n", WSAGetLastError());
    if (s == INVALID_SOCKET)
        return s;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0, "bind failed %d\n", WSAGetLastError());
    return s;
}

static
void
TestTdiHandlesBadOutput(void)
{
    AFD_TDI_HANDLE_DATA Handles;
    ULONG Flags = AFD_ADDRESS_HANDLE | AFD_CONNECTION_HANDLE;
    ULONG_PTR Info;
    NTSTATUS Status;
    SOCKET s;
    ULONG i;

    s = BoundTcpSocket();
    if (s == INVALID_SOCKET)
        return;

    for (i = 0; i < RTL_NUMBER_OF(BadOutputs); i++)
    {
        Status = OpIoctl((HANDLE)s, IOCTL_AFD_GET_TDI_HANDLES, &Flags, sizeof(Flags),
                         BadOutputs[i].Pointer, sizeof(Handles), &Info);
        ok(Status == STATUS_ACCESS_VIOLATION,
           "GET_TDI_HANDLES to a %s output: expected STATUS_ACCESS_VIOLATION, got 0x%lx\n",
           BadOutputs[i].Name, Status);
    }

    /* The socket still answers a valid request */
    memset(&Handles, 0xA5, sizeof(Handles));
    Status = OpIoctl((HANDLE)s, IOCTL_AFD_GET_TDI_HANDLES, &Flags, sizeof(Flags),
                     &Handles, sizeof(Handles), &Info);
    ok(Status == STATUS_SUCCESS, "GET_TDI_HANDLES to a valid output: got 0x%lx\n", Status);
    ok(Handles.TdiAddressHandle != NULL &&
       Handles.TdiAddressHandle != (HANDLE)(ULONG_PTR)0xA5A5A5A5A5A5A5A5ULL,
       "bound socket: no address handle returned (%p)\n", Handles.TdiAddressHandle);

    closesocket(s);
}

static const struct
{
    PCSTR Name;
    ULONG SetCode;
    ULONG GetCode;
} DataOptions[] =
{
    { "CONNECT_DATA", IOCTL_AFD_SET_CONNECT_DATA, IOCTL_AFD_GET_CONNECT_DATA },
    { "CONNECT_OPTIONS", IOCTL_AFD_SET_CONNECT_OPTIONS, IOCTL_AFD_GET_CONNECT_OPTIONS },
    { "DISCONNECT_DATA", IOCTL_AFD_SET_DISCONNECT_DATA, IOCTL_AFD_GET_DISCONNECT_DATA },
    { "DISCONNECT_OPTIONS", IOCTL_AFD_SET_DISCONNECT_OPTIONS, IOCTL_AFD_GET_DISCONNECT_OPTIONS },
};

static
void
TestDataOptionsBadOutput(void)
{
    UCHAR Blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, Out[8];
    ULONG_PTR Info;
    NTSTATUS Status;
    SOCKET s;
    ULONG i, j;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(s != INVALID_SOCKET, "socket failed %d\n", WSAGetLastError());
    if (s == INVALID_SOCKET)
        return;

    for (i = 0; i < RTL_NUMBER_OF(DataOptions); i++)
    {
        Status = OpIoctl((HANDLE)s, DataOptions[i].SetCode, Blob, sizeof(Blob), NULL, 0, NULL);
        ok(Status == STATUS_SUCCESS, "SET_%s: got 0x%lx\n", DataOptions[i].Name, Status);
        if (!NT_SUCCESS(Status))
            continue;

        for (j = 0; j < RTL_NUMBER_OF(BadOutputs); j++)
        {
            Status = OpIoctl((HANDLE)s, DataOptions[i].GetCode, NULL, 0,
                             BadOutputs[j].Pointer, sizeof(Out), &Info);
            ok(Status == STATUS_ACCESS_VIOLATION,
               "GET_%s to a %s output: expected STATUS_ACCESS_VIOLATION, got 0x%lx\n",
               DataOptions[i].Name, BadOutputs[j].Name, Status);
        }

        /* A valid output still works; nothing was received, so nothing is returned */
        Status = OpIoctl((HANDLE)s, DataOptions[i].GetCode, NULL, 0, Out, sizeof(Out), &Info);
        ok(Status == STATUS_SUCCESS, "GET_%s to a valid output: got 0x%lx\n", DataOptions[i].Name, Status);
        ok(Info == 0, "GET_%s: returned %Iu bytes, expected 0\n", DataOptions[i].Name, Info);
    }

    closesocket(s);
}

static
BOOL
PingPong(SOCKET a, SOCKET b, char c)
{
    char d = 0;
    return send(a, &c, 1, 0) == 1 && recv(b, &d, 1, 0) == 1 && d == c;
}

/* Connect and disconnect with connect/disconnect data and options set on both
 * ends. The connect and the graceful-disconnect completions used to copy
 * 4 bytes of options from a NULL buffer (the return info reports
 * OptionsLength = 4 with no buffer), a bugcheck. */
static
void
TestConnectDisconnectWithOptions(void)
{
    UCHAR Blob[4] = { 0xC1, 0xC2, 0xC3, 0xC4 }, Out[4];
    struct sockaddr_in a;
    int len = sizeof(a);
    SOCKET lst, cli = INVALID_SOCKET, srv = INVALID_SOCKET;
    ULONG_PTR Info;
    NTSTATUS Status;
    char c;
    ULONG i;

    lst = BoundTcpSocket();
    if (lst == INVALID_SOCKET)
        return;
    getsockname(lst, (struct sockaddr *)&a, &len);
    ok(listen(lst, 4) == 0, "listen failed %d\n", WSAGetLastError());

    cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(cli != INVALID_SOCKET, "socket failed %d\n", WSAGetLastError());
    if (cli == INVALID_SOCKET)
        goto out;

    for (i = 0; i < RTL_NUMBER_OF(DataOptions); i++)
    {
        Status = OpIoctl((HANDLE)cli, DataOptions[i].SetCode, Blob, sizeof(Blob), NULL, 0, NULL);
        ok(Status == STATUS_SUCCESS, "client SET_%s: got 0x%lx\n", DataOptions[i].Name, Status);
    }

    ok(connect(cli, (struct sockaddr *)&a, sizeof(a)) == 0, "connect failed %d\n", WSAGetLastError());
    srv = accept(lst, NULL, NULL);
    ok(srv != INVALID_SOCKET, "accept failed %d\n", WSAGetLastError());
    if (srv == INVALID_SOCKET)
        goto out;

    /* The accepted end too (its return info is built at accept) */
    Status = OpIoctl((HANDLE)srv, IOCTL_AFD_SET_DISCONNECT_OPTIONS, Blob, sizeof(Blob), NULL, 0, NULL);
    ok(Status == STATUS_SUCCESS, "server SET_DISCONNECT_OPTIONS: got 0x%lx\n", Status);
    Status = OpIoctl((HANDLE)srv, IOCTL_AFD_SET_DISCONNECT_DATA, Blob, sizeof(Blob), NULL, 0, NULL);
    ok(Status == STATUS_SUCCESS, "server SET_DISCONNECT_DATA: got 0x%lx\n", Status);

    ok(PingPong(cli, srv, 'a'), "client to server data failed %d\n", WSAGetLastError());
    ok(PingPong(srv, cli, 'b'), "server to client data failed %d\n", WSAGetLastError());

    /* The transport returns no connect data or options */
    Status = OpIoctl((HANDLE)cli, IOCTL_AFD_GET_CONNECT_OPTIONS, NULL, 0, Out, sizeof(Out), &Info);
    ok(Status == STATUS_SUCCESS && Info == 0, "GET_CONNECT_OPTIONS after connect: 0x%lx, %Iu bytes\n", Status, Info);
    Status = OpIoctl((HANDLE)cli, IOCTL_AFD_GET_CONNECT_DATA, NULL, 0, Out, sizeof(Out), &Info);
    ok(Status == STATUS_SUCCESS && Info == 0, "GET_CONNECT_DATA after connect: 0x%lx, %Iu bytes\n", Status, Info);

    /* Graceful disconnect, each way */
    ok(shutdown(cli, SD_SEND) == 0, "client shutdown failed %d\n", WSAGetLastError());
    ok(recv(srv, &c, 1, 0) == 0, "server did not see the client FIN (%d)\n", WSAGetLastError());
    ok(shutdown(srv, SD_SEND) == 0, "server shutdown failed %d\n", WSAGetLastError());
    ok(recv(cli, &c, 1, 0) == 0, "client did not see the server FIN (%d)\n", WSAGetLastError());

    Status = OpIoctl((HANDLE)cli, IOCTL_AFD_GET_DISCONNECT_OPTIONS, NULL, 0, Out, sizeof(Out), &Info);
    ok(Status == STATUS_SUCCESS && Info == 0, "GET_DISCONNECT_OPTIONS after disconnect: 0x%lx, %Iu bytes\n", Status, Info);
    Status = OpIoctl((HANDLE)srv, IOCTL_AFD_GET_DISCONNECT_DATA, NULL, 0, Out, sizeof(Out), &Info);
    ok(Status == STATUS_SUCCESS && Info == 0, "GET_DISCONNECT_DATA after disconnect: 0x%lx, %Iu bytes\n", Status, Info);

out:
    if (srv != INVALID_SOCKET)
        closesocket(srv);
    if (cli != INVALID_SOCKET)
        closesocket(cli);
    closesocket(lst);
}

START_TEST(outputprobe)
{
    WSADATA wd;

    ok(WSAStartup(MAKEWORD(2, 2), &wd) == 0, "WSAStartup failed\n");
    if (SetupBadOutputs())
    {
        TestTdiHandlesBadOutput();
        TestDataOptionsBadOutput();
    }
    FreeBadOutputs();
    WSACleanup();
}

START_TEST(connectoptions)
{
    WSADATA wd;

    ok(WSAStartup(MAKEWORD(2, 2), &wd) == 0, "WSAStartup failed\n");
    TestConnectDisconnectWithOptions();
    WSACleanup();
}
