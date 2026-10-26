/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     AFD datagram sends that stay in the transport (T124b).
 *              The real transports complete every datagram send inside the
 *              call, so AFD never sees a send that is still in flight. These
 *              tests send through the stub transport afdstub.sys, which keeps
 *              a datagram until the test completes it, and check that
 *              - a send request completes with the result of its own
 *                datagram, not that of the request next in the queue, when a
 *                neighbouring request was cancelled or the datagrams complete
 *                out of order (T124);
 *              - closing a socket waits for every datagram in the transport,
 *                not only for the last one sent;
 *              - the inline completion path of the real transport is intact.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#include <winsvc.h>
#include "afdstub.h"

#define HOT_SENDS 50000

static HANDLE Ctl;

typedef struct _SEND
{
    IO_STATUS_BLOCK Iosb;
    HANDLE Event;
    NTSTATUS Issue;
    AFD_WSABUF Buf;
    AFD_SEND_INFO_UDP Udp;
    UCHAR Data[64];
    UCHAR Ta[FIELD_OFFSET(TRANSPORT_ADDRESS, Address[0].Address) + 14];
} SEND;

static
NTSTATUS
StubIoctl(ULONG Code, PVOID In, ULONG InLen, PVOID Out, ULONG OutLen)
{
    IO_STATUS_BLOCK Iosb;

    return NtDeviceIoControlFile(Ctl, NULL, NULL, NULL, &Iosb, Code, In, InLen, Out, OutLen);
}

static
void
StubSet(ULONG Hold, ULONG Honor, ULONG CompleteOnCleanup)
{
    AFDSTUB_SET Set;
    NTSTATUS Status;

    Set.HoldSends = Hold;
    Set.HonorCancel = Honor;
    Set.CompleteOnCleanup = CompleteOnCleanup;
    Status = StubIoctl(IOCTL_AFDSTUB_SET, &Set, sizeof(Set), NULL, 0);
    ok_hex(Status, STATUS_SUCCESS);
}

static
void
StubQuery(AFDSTUB_STATE *State)
{
    NTSTATUS Status;

    RtlZeroMemory(State, sizeof(*State));
    Status = StubIoctl(IOCTL_AFDSTUB_QUERY, NULL, 0, State, sizeof(*State));
    ok_hex(Status, STATUS_SUCCESS);
}

static
NTSTATUS
StubComplete(ULONG Index, NTSTATUS Status)
{
    AFDSTUB_COMPLETE Req;

    Req.Index = Index;
    Req.Status = Status;
    return StubIoctl(IOCTL_AFDSTUB_COMPLETE, &Req, sizeof(Req), NULL, 0);
}

static
BOOLEAN
OpenControl(void)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(AFDSTUB_DEVICE_NAME);
    OBJECT_ATTRIBUTES Attr;
    IO_STATUS_BLOCK Iosb;

    InitializeObjectAttributes(&Attr, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NT_SUCCESS(NtCreateFile(&Ctl, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &Attr, &Iosb,
                                   NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT,
                                   NULL, 0));
}

static
BOOLEAN
LoadStub(SC_HANDLE *ServiceOut)
{
    SC_HANDLE Scm, Service;
    WCHAR Path[MAX_PATH];
    PWCHAR Slash;
    ULONG i;

    *ServiceOut = NULL;
    if (OpenControl())
        return TRUE;

    Scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!Scm)
    {
        skip("OpenSCManager failed: %lu\n", GetLastError());
        return FALSE;
    }
    GetModuleFileNameW(NULL, Path, RTL_NUMBER_OF(Path));
    Slash = wcsrchr(Path, L'\\');
    if (!Slash)
        Slash = Path - 1;
    wcscpy(Slash + 1, L"afdstub.sys");

    Service = CreateServiceW(Scm, L"afdstub", L"AFD stub transport", SERVICE_ALL_ACCESS,
                             SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                             Path, NULL, NULL, NULL, NULL, NULL);
    if (!Service && GetLastError() == ERROR_SERVICE_EXISTS)
        Service = OpenServiceW(Scm, L"afdstub", SERVICE_ALL_ACCESS);
    CloseServiceHandle(Scm);
    if (!Service)
    {
        skip("cannot create the stub transport service: %lu\n", GetLastError());
        return FALSE;
    }
    if (!StartServiceW(Service, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
    {
        skip("cannot start %ls: %lu\n", Path, GetLastError());
        CloseServiceHandle(Service);
        return FALSE;
    }
    *ServiceOut = Service;
    for (i = 0; i < 50; i++)
    {
        if (OpenControl())
            return TRUE;
        Sleep(100);
    }
    skip("cannot open the stub transport\n");
    return FALSE;
}

static
HANDLE
NewSocket(void)
{
    HANDLE Sock = NULL;
    NTSTATUS Status;

    Status = AfdCreateSocketOnTransport(&Sock, AF_INET, SOCK_DGRAM, IPPROTO_UDP, AFDSTUB_DEVICE_NAME);
    ok_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? Sock : NULL;
}

/* Issues a datagram send of Len bytes of Tag; the request pends while the
 * stub holds the datagram */
static
void
SendStart(HANDLE Sock, SEND *S, UCHAR Tag, ULONG Len)
{
    PTRANSPORT_ADDRESS Dest = (PTRANSPORT_ADDRESS)S->Ta;
    struct sockaddr_in Addr;

    RtlZeroMemory(S, sizeof(*S));
    NtCreateEvent(&S->Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    RtlFillMemory(S->Data, sizeof(S->Data), Tag);
    S->Buf.buf = (PCHAR)S->Data;
    S->Buf.len = Len;

    RtlZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_port = _byteswap_ushort(9);
    Addr.sin_addr.s_addr = 0x0100007f;
    Dest->TAAddressCount = 1;
    Dest->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
    Dest->Address[0].AddressLength = 14;
    RtlCopyMemory(&Dest->Address[0].Address, (PUCHAR)&Addr + FIELD_OFFSET(struct sockaddr, sa_data), 14);

    S->Udp.BufferArray = &S->Buf;
    S->Udp.BufferCount = 1;
    S->Udp.TdiConnection.RemoteAddress = Dest;
    S->Udp.TdiConnection.RemoteAddressLength = sizeof(S->Ta);

    S->Iosb.Status = STATUS_PENDING;
    S->Iosb.Information = 0;
    S->Issue = NtDeviceIoControlFile(Sock, S->Event, NULL, NULL, &S->Iosb, IOCTL_AFD_SEND_DATAGRAM,
                                     &S->Udp, sizeof(S->Udp), NULL, 0);
}

/* TRUE when the request has completed (waits up to Ms) */
static
BOOLEAN
SendDone(SEND *S, ULONG Ms)
{
    LARGE_INTEGER Timeout;

    if (S->Issue != STATUS_PENDING)
        return TRUE;
    Timeout.QuadPart = -(LONGLONG)Ms * 10000;
    return NtWaitForSingleObject(S->Event, FALSE, &Timeout) == STATUS_WAIT_0;
}

static
NTSTATUS
SendStatus(SEND *S)
{
    return S->Issue == STATUS_PENDING ? S->Iosb.Status : S->Issue;
}

static
void
SendFree(SEND *S)
{
    NtClose(S->Event);
}

static
void
WaitHeld(ULONG Count)
{
    AFDSTUB_STATE State;
    ULONG i;

    for (i = 0; i < 100; i++)
    {
        StubQuery(&State);
        if (State.Held == Count)
            return;
        Sleep(10);
    }
    ok(FALSE, "%lu datagrams in the transport, expected %lu\n", State.Held, Count);
}

static BOOLEAN WaitSocketFreed(AFDSTUB_STATE *State, ULONG Ms);

/* Completes whatever is left in the transport and closes the socket */
static
void
Finish(HANDLE Sock)
{
    AFDSTUB_STATE State;

    StubComplete(AFDSTUB_ALL, STATUS_SUCCESS);
    NtClose(Sock);
    ok(WaitSocketFreed(&State, 10000), "socket not freed\n");
    ok(State.Held == 0, "datagrams left in the transport: %lu\n", State.Held);
}

static
void
TestInlineSends(void)
{
    AFDSTUB_STATE State;
    HANDLE Sock;
    SEND S;
    ULONG i;

    StubSet(0, 1, 1);
    Sock = NewSocket();
    if (!Sock)
        return;

    for (i = 0; i < 3; i++)
    {
        SendStart(Sock, &S, 'a', 10 + i);
        ok(SendDone(&S, 5000), "send %lu did not complete\n", i);
        ok_hex(SendStatus(&S), STATUS_SUCCESS);
        ok_dec((ULONG)S.Iosb.Information, 10 + i);
        SendFree(&S);
    }
    StubQuery(&State);
    ok_dec(State.SendsSeen, 3);
    ok_dec(State.SendsInline, 3);
    ok_dec(State.Held, 0);

    /* an empty datagram never reaches the transport */
    SendStart(Sock, &S, 'z', 0);
    ok(SendDone(&S, 5000), "empty send did not complete\n");
    SendFree(&S);

    Finish(Sock);
}

/* The datagram of a cancelled request is still in the transport. Completing
 * it must not complete the request now at the head of the queue. */
static
void
TestCancelledOwnerDoesNotStealCompletion(void)
{
    AFDSTUB_STATE State;
    HANDLE Sock;
    SEND A, B, C;
    IO_STATUS_BLOCK Cancel;
    NTSTATUS Status;
    NTSTATUS (NTAPI *pNtCancelIoFileEx)(HANDLE, PIO_STATUS_BLOCK, PIO_STATUS_BLOCK);

    /* Vista+ API: not exported by the 2003-targeted ntdll */
    pNtCancelIoFileEx = (PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCancelIoFileEx");
    if (!pNtCancelIoFileEx)
    {
        skip("NtCancelIoFileEx is not available\n");
        return;
    }

    StubSet(1, 1, 1);
    Sock = NewSocket();
    if (!Sock)
        return;

    SendStart(Sock, &A, 'A', 10);
    SendStart(Sock, &B, 'B', 20);
    SendStart(Sock, &C, 'C', 30);
    ok_hex(A.Issue, STATUS_PENDING);
    ok_hex(B.Issue, STATUS_PENDING);
    ok_hex(C.Issue, STATUS_PENDING);
    WaitHeld(3);
    StubQuery(&State);
    ok(State.Held == 3 && State.Entry[0].FirstByte == 'A' && State.Entry[1].FirstByte == 'B' &&
       State.Entry[2].FirstByte == 'C', "datagrams in the transport out of order\n");

    /* cancel the request of the middle datagram only */
    Status = pNtCancelIoFileEx(Sock, &B.Iosb, &Cancel);
    ok_hex(Status, STATUS_SUCCESS);
    ok(SendDone(&B, 5000), "cancelled request B did not complete\n");
    ok_hex(SendStatus(&B), STATUS_CANCELLED);
    ok(!SendDone(&A, 100), "request A completed before its datagram\n");
    ok(!SendDone(&C, 100), "request C completed before its datagram\n");
    StubQuery(&State);
    ok_dec(State.Held, 3);      /* B's datagram is still in flight */

    /* A's datagram completes: A, and only A */
    ok_hex(StubComplete(0, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(SendDone(&A, 5000), "request A did not complete\n");
    ok_hex(SendStatus(&A), STATUS_SUCCESS);
    ok_dec((ULONG)A.Iosb.Information, 10);
    ok(!SendDone(&C, 100), "request C completed with A's datagram\n");

    /* the datagram of the cancelled request completes: nobody completes */
    ok_hex(StubComplete(0, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(!SendDone(&C, 300), "request C completed with the datagram of the cancelled request B (status %lx, %Iu bytes)\n",
       (ULONG)SendStatus(&C), C.Iosb.Information);

    /* C's own datagram completes C with C's length */
    ok_hex(StubComplete(0, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(SendDone(&C, 5000), "request C did not complete\n");
    ok_hex(SendStatus(&C), STATUS_SUCCESS);
    ok_dec((ULONG)C.Iosb.Information, 30);
    StubQuery(&State);
    ok_dec(State.Held, 0);

    SendFree(&A);
    SendFree(&B);
    SendFree(&C);
    Finish(Sock);
}

/* The transport may complete datagrams in any order */
static
void
TestOutOfOrderCompletion(void)
{
    AFDSTUB_STATE State;
    HANDLE Sock;
    SEND A, B, C;

    StubSet(1, 1, 1);
    Sock = NewSocket();
    if (!Sock)
        return;

    SendStart(Sock, &A, 'A', 10);
    SendStart(Sock, &B, 'B', 20);
    SendStart(Sock, &C, 'C', 30);
    WaitHeld(3);

    /* the last datagram first: it completes C with C's result, not the head A */
    ok_hex(StubComplete(2, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(SendDone(&C, 5000), "request C did not complete with its datagram\n");
    ok_hex(SendStatus(&C), STATUS_SUCCESS);
    ok_dec((ULONG)C.Iosb.Information, 30);
    ok(!SendDone(&A, 200), "request A completed with C's datagram (status %lx, %Iu bytes)\n",
       (ULONG)SendStatus(&A), A.Iosb.Information);
    ok(!SendDone(&B, 100), "request B completed with C's datagram\n");

    /* a datagram that fails completes its own request with the failure */
    ok_hex(StubComplete(1, STATUS_INSUFFICIENT_RESOURCES), STATUS_SUCCESS);
    ok(SendDone(&B, 5000), "request B did not complete with its datagram\n");
    ok_hex(SendStatus(&B), STATUS_INSUFFICIENT_RESOURCES);
    ok(!SendDone(&A, 100), "request A completed with B's datagram\n");

    ok_hex(StubComplete(0, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(SendDone(&A, 5000), "request A did not complete\n");
    ok_hex(SendStatus(&A), STATUS_SUCCESS);
    ok_dec((ULONG)A.Iosb.Information, 10);
    StubQuery(&State);
    ok_dec(State.Held, 0);

    SendFree(&A);
    SendFree(&B);
    SendFree(&C);
    Finish(Sock);
}

/* A datagram the transport fails at once (tcpip completes it inside the call
 * and still returns STATUS_PENDING) completes that request with the failure,
 * and leaves the datagrams in flight and their requests alone */
static
void
TestFailedHandoff(void)
{
    AFDSTUB_STATE State;
    HANDLE Sock;
    SEND A, B, C;

    StubSet(1, 1, 1);
    Sock = NewSocket();
    if (!Sock)
        return;

    SendStart(Sock, &A, 'A', 10);
    WaitHeld(1);
    StubSet(AFDSTUB_FAIL, 1, 1);
    SendStart(Sock, &B, 'B', 20);
    ok(SendDone(&B, 5000), "refused request B did not complete\n");
    ok_hex(SendStatus(&B), STATUS_INSUFFICIENT_RESOURCES);
    ok(!SendDone(&A, 200), "request A completed when B's hand-off failed\n");

    StubSet(1, 1, 1);
    SendStart(Sock, &C, 'C', 30);
    WaitHeld(2);
    ok_hex(StubComplete(1, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(SendDone(&C, 5000), "request C did not complete\n");
    ok_hex(SendStatus(&C), STATUS_SUCCESS);
    ok_dec((ULONG)C.Iosb.Information, 30);
    ok(!SendDone(&A, 100), "request A completed with C's datagram\n");
    ok_hex(StubComplete(0, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(SendDone(&A, 5000), "request A did not complete\n");
    ok_dec((ULONG)A.Iosb.Information, 10);
    StubQuery(&State);
    ok_dec(State.Held, 0);

    SendFree(&A);
    SendFree(&B);
    SendFree(&C);
    Finish(Sock);
}

typedef struct _CLOSE_CTX
{
    SEND S[3];
    ULONG Count;
    HANDLE Ready;
    HANDLE Sock;
} CLOSE_CTX;

/* Sends Count datagrams, then closes the socket, all in one thread so that the
 * close runs as it does in a program */
static
DWORD
WINAPI
CloseThread(PVOID Param)
{
    CLOSE_CTX *Ctx = Param;
    ULONG i;

    Ctx->Sock = NewSocket();
    if (Ctx->Sock)
    {
        for (i = 0; i < Ctx->Count; i++)
            SendStart(Ctx->Sock, &Ctx->S[i], 'A' + i, 10 * (i + 1));
    }
    SetEvent(Ctx->Ready);
    if (Ctx->Sock)
        NtClose(Ctx->Sock);
    return 0;
}

static
CLOSE_CTX *
StartClose(ULONG Count, HANDLE *Thread)
{
    static CLOSE_CTX Ctx;

    RtlZeroMemory(&Ctx, sizeof(Ctx));
    Ctx.Count = Count;
    Ctx.Ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    *Thread = CreateThread(NULL, 0, CloseThread, &Ctx, 0, NULL);
    WaitForSingleObject(Ctx.Ready, 10000);
    return &Ctx;
}

/* Waits for the stub to see the address file go (AFD closes it as the last
 * step of freeing the socket) */
static
BOOLEAN
WaitSocketFreed(AFDSTUB_STATE *State, ULONG Ms)
{
    ULONG i;

    for (i = 0; i < Ms / 10; i++)
    {
        StubQuery(State);
        if (State->AddressFilesOpen == 0)
            return TRUE;
        Sleep(10);
    }
    return FALSE;
}

/* The transport cannot take a datagram back: closing the socket has to wait
 * for every datagram, not just the one sent last */
static
void
TestCloseWaitsForAllDatagrams(void)
{
    AFDSTUB_STATE State;
    CLOSE_CTX *Ctx;
    HANDLE Thread;

    StubSet(1, 0, 1);
    Ctx = StartClose(3, &Thread);
    WaitHeld(3);

    Sleep(300);
    StubQuery(&State);
    ok_dec(State.AddressFilesOpen, 1);

    /* the last datagram completes first: the socket is still not free */
    ok_hex(StubComplete(2, STATUS_SUCCESS), STATUS_SUCCESS);
    Sleep(300);
    StubQuery(&State);
    ok(State.AddressFilesOpen == 1 && State.CleanupWhileHeld == 0,
       "socket freed with %lu datagrams still in the transport (address files %lu, cleanups with datagrams %lu)\n",
       State.Held, State.AddressFilesOpen, State.CleanupWhileHeld);

    ok_hex(StubComplete(0, STATUS_SUCCESS), STATUS_SUCCESS);
    Sleep(300);
    StubQuery(&State);
    ok(State.AddressFilesOpen == 1 && State.CleanupWhileHeld == 0,
       "socket freed with %lu datagrams still in the transport (address files %lu, cleanups with datagrams %lu)\n",
       State.Held, State.AddressFilesOpen, State.CleanupWhileHeld);

    /* the last one: now it goes */
    ok_hex(StubComplete(0, STATUS_SUCCESS), STATUS_SUCCESS);
    ok(WaitSocketFreed(&State, 10000), "socket not freed after its datagrams completed\n");
    ok_dec(State.CleanupWhileHeld, 0);
    ok_dec(State.Held, 0);
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "closing thread did not finish\n");
    CloseHandle(Thread);
    CloseHandle(Ctx->Ready);
}

/* A transport that completes its datagrams only when it is ready, also long
 * after the file was closed: the socket must outlive them */
static
void
TestLateCompletionAfterClose(void)
{
    AFDSTUB_STATE State;
    CLOSE_CTX *Ctx;
    HANDLE Thread;

    StubSet(1, 0, 0);
    Ctx = StartClose(3, &Thread);
    WaitHeld(3);

    ok_hex(StubComplete(2, STATUS_SUCCESS), STATUS_SUCCESS);
    Sleep(300);
    StubQuery(&State);
    ok(State.AddressFilesOpen == 1 && State.CleanupWhileHeld == 0,
       "socket freed with %lu datagrams still in the transport (address files %lu, cleanups with datagrams %lu)\n",
       State.Held, State.AddressFilesOpen, State.CleanupWhileHeld);

    /* these complete while the socket is being closed, or after it was freed */
    StubComplete(AFDSTUB_ALL, STATUS_SUCCESS);
    ok(WaitSocketFreed(&State, 10000), "socket not freed after its datagrams completed\n");
    ok_dec(State.Held, 0);
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "closing thread did not finish\n");
    CloseHandle(Thread);
    CloseHandle(Ctx->Ready);
}

/* A transport that takes cancels back: the close cancels every datagram and
 * is done at once */
static
void
TestCloseCancelsAllDatagrams(void)
{
    AFDSTUB_STATE State;
    CLOSE_CTX *Ctx;
    HANDLE Thread;

    StubSet(1, 1, 1);
    Ctx = StartClose(3, &Thread);
    WaitHeld(3);

    ok(WaitSocketFreed(&State, 10000), "socket not freed although its datagrams can be cancelled\n");
    ok(State.CancelledByCancel == 3 && State.CleanupWhileHeld == 0,
       "closing cancelled %lu of 3 datagrams, %lu cleanups found datagrams\n",
       State.CancelledByCancel, State.CleanupWhileHeld);
    ok_dec(State.Held, 0);
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "closing thread did not finish\n");
    CloseHandle(Thread);
    CloseHandle(Ctx->Ready);
}

/* The path every real send takes: completed inside the transport call. Timed
 * on the stub and on the loopback transport, for comparing builds. */
static
void
TestInlineHotPath(void)
{
    struct sockaddr_in Addr;
    HANDLE Sock;
    SOCKET Rx = INVALID_SOCKET, Tx = INVALID_SOCKET;
    ULONG i, Ok = 0;
    LARGE_INTEGER Start, End, Freq;
    char Buf[64] = { 1 };
    AFDSTUB_STATE State;
    int Len = sizeof(Addr);
    WSADATA WsaData;

    StubSet(0, 1, 1);
    Sock = NewSocket();
    if (Sock)
    {
        RtlZeroMemory(&Addr, sizeof(Addr));
        Addr.sin_family = AF_INET;
        Addr.sin_port = _byteswap_ushort(9);
        Addr.sin_addr.s_addr = 0x0100007f;
        QueryPerformanceCounter(&Start);
        for (i = 0; i < HOT_SENDS; i++)
        {
            if (AfdSendTo(Sock, Buf, sizeof(Buf), (struct sockaddr *)&Addr, sizeof(Addr)) == STATUS_SUCCESS)
                Ok++;
        }
        QueryPerformanceCounter(&End);
        QueryPerformanceFrequency(&Freq);
        trace("HOTPATH stub-inline: %lu sends, %lu ns each\n", Ok, (ULONG)((double)(End.QuadPart - Start.QuadPart) * 1e9 / (double)Freq.QuadPart / HOT_SENDS));
        ok_dec(Ok, HOT_SENDS);
        StubQuery(&State);
        ok_dec(State.SendsInline, HOT_SENDS);
        Finish(Sock);
    }

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
        return;
    Rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Rx != INVALID_SOCKET && Tx != INVALID_SOCKET)
    {
        RtlZeroMemory(&Addr, sizeof(Addr));
        Addr.sin_family = AF_INET;
        Addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        ok(bind(Rx, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed\n");
        ok(getsockname(Rx, (struct sockaddr *)&Addr, &Len) == 0, "getsockname failed\n");
        {
            struct sockaddr_in Local = Addr;
            Local.sin_port = 0;
            ok(bind(Tx, (struct sockaddr *)&Local, sizeof(Local)) == 0, "bind failed: %d\n", WSAGetLastError());
        }
        Ok = 0;
        QueryPerformanceCounter(&Start);
        for (i = 0; i < HOT_SENDS; i++)
        {
            if (AfdSendTo((HANDLE)Tx, Buf, sizeof(Buf), (struct sockaddr *)&Addr, sizeof(Addr)) == STATUS_SUCCESS)
                Ok++;
        }
        QueryPerformanceCounter(&End);
        QueryPerformanceFrequency(&Freq);
        trace("HOTPATH loopback-udp: %lu sends, %lu ns each\n", Ok, (ULONG)((double)(End.QuadPart - Start.QuadPart) * 1e9 / (double)Freq.QuadPart / HOT_SENDS));
        ok_dec(Ok, HOT_SENDS);
    }
    if (Rx != INVALID_SOCKET)
        closesocket(Rx);
    if (Tx != INVALID_SOCKET)
        closesocket(Tx);
    WSACleanup();
}

static
BOOLEAN
Setup(SC_HANDLE *Service)
{
    return LoadStub(Service);
}

/* The driver stays loaded and registered: a stop and delete at the end of one
 * test races with the start of the next, and the VM of the test run is thrown
 * away anyway */
static
void
Teardown(SC_HANDLE Service)
{
    NtClose(Ctl);
    if (Service)
        CloseServiceHandle(Service);
}

START_TEST(dgramstub)
{
    SC_HANDLE Service;

    if (!Setup(&Service))
        return;

    TestInlineSends();
    TestCancelledOwnerDoesNotStealCompletion();
    TestOutOfOrderCompletion();

    Teardown(Service);
}

START_TEST(dgramlate)
{
    SC_HANDLE Service;

    if (!Setup(&Service))
        return;

    TestLateCompletionAfterClose();

    Teardown(Service);
}

START_TEST(dgramfail)
{
    SC_HANDLE Service;

    if (!Setup(&Service))
        return;

    TestFailedHandoff();

    Teardown(Service);
}

START_TEST(dgramclose)
{
    SC_HANDLE Service;

    if (!Setup(&Service))
        return;

    TestCloseWaitsForAllDatagrams();
    TestCloseCancelsAllDatagrams();

    Teardown(Service);
}

START_TEST(dgramhot)
{
    SC_HANDLE Service;

    if (!Setup(&Service))
        return;

    TestInlineHotPath();

    Teardown(Service);
}
