/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Contract tests: an accept completes only into a fresh,
 *              unconnected stream socket. Accepting into a connected socket,
 *              another listener, the listener itself, a datagram socket or a
 *              bound socket fails with STATUS_INVALID_PARAMETER (WSAEINVAL),
 *              leaves the connection queued for a later accept, and leaves
 *              both sockets usable. Checked on the plain accept request
 *              (IOCTL_AFD_ACCEPT) and on AcceptEx; the AcceptEx results match
 *              Windows 10 (WSAEINVAL in every case). A socket that only had
 *              socket options set is still fresh and is accepted. Two
 *              listeners that accept into each other from two threads fail
 *              the requests; they must not deadlock on each other's lock.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"
#include <mswsock.h>

static
NTSTATUS
AsIoctl(
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
    LARGE_INTEGER Timeout;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    IoStatus.Status = STATUS_PENDING;
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

    NtClose(Event);
    return Status;
}

static struct sockaddr_in ListenAddr;

static
SOCKET
MakeListener(struct sockaddr_in *Addr)
{
    int len = sizeof(*Addr);
    SOCKET s;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(s != INVALID_SOCKET, "socket failed %d\n", WSAGetLastError());
    if (s == INVALID_SOCKET)
        return s;
    memset(Addr, 0, sizeof(*Addr));
    Addr->sin_family = AF_INET;
    Addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(s, (struct sockaddr *)Addr, sizeof(*Addr)) == 0, "bind failed %d\n", WSAGetLastError());
    getsockname(s, (struct sockaddr *)Addr, &len);
    ok(listen(s, 5) == 0, "listen failed %d\n", WSAGetLastError());
    return s;
}

static
SOCKET
Connect(const struct sockaddr_in *Addr)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(s != INVALID_SOCKET, "socket failed %d\n", WSAGetLastError());
    if (s != INVALID_SOCKET)
        ok(connect(s, (const struct sockaddr *)Addr, sizeof(*Addr)) == 0, "connect failed %d\n", WSAGetLastError());
    return s;
}

/* Wait until the listener has a connection to accept (FD_ACCEPT) */
static
BOOL
WaitForPending(SOCKET Listener)
{
    fd_set rd;
    struct timeval tv;

    FD_ZERO(&rd);
    FD_SET(Listener, &rd);
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    return select(0, &rd, NULL, NULL, &tv) == 1;
}

static
BOOL
PingPong(SOCKET a, SOCKET b, char c)
{
    char d = 0;
    return send(a, &c, 1, 0) == 1 && recv(b, &d, 1, 0) == 1 && d == c;
}

/* The non-fresh accept targets */
typedef struct _TARGETS
{
    SOCKET PairClient, PairServer;  /* a connected pair */
    SOCKET OtherListener;
    struct sockaddr_in OtherAddr;
    SOCKET Udp;                     /* unbound datagram socket */
    SOCKET UdpBound;
    SOCKET Bound;                   /* bound, not connected */
} TARGETS;

static
void
MakeTargets(SOCKET Listener, TARGETS *t)
{
    struct sockaddr_in a;

    t->PairClient = Connect(&ListenAddr);
    t->PairServer = accept(Listener, NULL, NULL);
    ok(t->PairServer != INVALID_SOCKET, "pair accept failed %d\n", WSAGetLastError());
    t->OtherListener = MakeListener(&t->OtherAddr);
    t->Udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(t->Udp != INVALID_SOCKET, "udp socket failed %d\n", WSAGetLastError());
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    t->UdpBound = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(bind(t->UdpBound, (struct sockaddr *)&a, sizeof(a)) == 0, "udp bind failed %d\n", WSAGetLastError());
    t->Bound = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(bind(t->Bound, (struct sockaddr *)&a, sizeof(a)) == 0, "tcp bind failed %d\n", WSAGetLastError());
}

/* After the rejected accepts every target still does its job */
static
void
CheckTargetsUsable(PCSTR Pass, TARGETS *t)
{
    struct sockaddr_in a;
    int len = sizeof(a);
    SOCKET c, s;
    char d = 0;

    ok(PingPong(t->PairClient, t->PairServer, 'p'), "%s: connected pair, client to server failed %d\n", Pass, WSAGetLastError());
    ok(PingPong(t->PairServer, t->PairClient, 'q'), "%s: connected pair, server to client failed %d\n", Pass, WSAGetLastError());

    c = Connect(&t->OtherAddr);
    s = accept(t->OtherListener, NULL, NULL);
    ok(s != INVALID_SOCKET, "%s: other listener no longer accepts (%d)\n", Pass, WSAGetLastError());
    if (s != INVALID_SOCKET)
    {
        ok(PingPong(c, s, 'r'), "%s: other listener's connection failed %d\n", Pass, WSAGetLastError());
        closesocket(s);
    }
    closesocket(c);

    getsockname(t->UdpBound, (struct sockaddr *)&a, &len);
    ok(sendto(t->UdpBound, "u", 1, 0, (struct sockaddr *)&a, sizeof(a)) == 1 &&
       recv(t->UdpBound, &d, 1, 0) == 1 && d == 'u',
       "%s: bound datagram socket no longer works (%d)\n", Pass, WSAGetLastError());
}

static
void
FreeTargets(TARGETS *t)
{
    closesocket(t->PairClient);
    closesocket(t->PairServer);
    closesocket(t->OtherListener);
    closesocket(t->Udp);
    closesocket(t->UdpBound);
    closesocket(t->Bound);
}

static
NTSTATUS
WaitForListen(SOCKET Listener, PULONG Sequence)
{
    UCHAR Buffer[sizeof(AFD_RECEIVED_ACCEPT_DATA) + 64];
    NTSTATUS Status;

    memset(Buffer, 0, sizeof(Buffer));
    Status = AsIoctl((HANDLE)Listener, IOCTL_AFD_WAIT_FOR_LISTEN, NULL, 0, Buffer, sizeof(Buffer));
    *Sequence = ((PAFD_RECEIVED_ACCEPT_DATA)Buffer)->SequenceNumber;
    return Status;
}

static
NTSTATUS
RawAccept(SOCKET Listener, ULONG Sequence, SOCKET Target)
{
    AFD_ACCEPT_DATA Accept;

    memset(&Accept, 0, sizeof(Accept));
    Accept.SequenceNumber = Sequence;
    Accept.ListenHandle = (HANDLE)Target;
    return AsIoctl((HANDLE)Listener, IOCTL_AFD_ACCEPT, &Accept, sizeof(Accept), NULL, 0);
}

/* IOCTL_AFD_ACCEPT (AfdAccept): accept() in msafd always passes a new socket,
 * so drive the request directly */
static
void
TestPlainAccept(void)
{
    TARGETS t;
    SOCKET lst, cli, srv;
    ULONG Seq = 0;
    NTSTATUS Status;
    ULONG i;
    struct { PCSTR Name; SOCKET *Socket; } Cases[] =
    {
        { "connected (accepted end)", &t.PairServer },
        { "connected (client end)", &t.PairClient },
        { "another listener", &t.OtherListener },
        { "the listener itself", &lst },
        { "datagram (unbound)", &t.Udp },
        { "datagram (bound)", &t.UdpBound },
        { "bound, not connected", &t.Bound },
    };

    lst = MakeListener(&ListenAddr);
    if (lst == INVALID_SOCKET)
        return;
    MakeTargets(lst, &t);

    cli = Connect(&ListenAddr);
    Status = WaitForListen(lst, &Seq);
    ok(Status == STATUS_SUCCESS, "WAIT_FOR_LISTEN: got 0x%lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
        {
            Status = RawAccept(lst, Seq, *Cases[i].Socket);
            ok(Status == STATUS_INVALID_PARAMETER,
               "IOCTL_AFD_ACCEPT into %s: expected STATUS_INVALID_PARAMETER, got 0x%lx\n",
               Cases[i].Name, Status);
        }
    }

    CheckTargetsUsable("accept", &t);

    /* The connection is still queued: a normal accept() takes it */
    srv = accept(lst, NULL, NULL);
    ok(srv != INVALID_SOCKET, "accept after the rejected requests failed %d\n", WSAGetLastError());
    if (srv != INVALID_SOCKET)
    {
        ok(PingPong(cli, srv, 'x'), "queued connection, client to server failed %d\n", WSAGetLastError());
        ok(PingPong(srv, cli, 'y'), "queued connection, server to client failed %d\n", WSAGetLastError());
        closesocket(srv);
    }

    closesocket(cli);
    FreeTargets(&t);
    closesocket(lst);
}

static LPFN_ACCEPTEX pAcceptEx;

/* Returns 0 or the WSA error of the AcceptEx, waiting for its completion */
static
int
TryAcceptEx(SOCKET Listener, SOCKET Target)
{
    char Buffer[2 * (sizeof(struct sockaddr_in) + 16)];
    DWORD Got = 0, Flags = 0;
    OVERLAPPED ov;
    int Error = 0;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = WSACreateEvent();
    if (!pAcceptEx(Listener, Target, Buffer, 0, sizeof(struct sockaddr_in) + 16,
                   sizeof(struct sockaddr_in) + 16, &Got, &ov))
    {
        Error = WSAGetLastError();
        if (Error == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, 10000) != WAIT_OBJECT_0)
            {
                CancelIo((HANDLE)Listener);
                WaitForSingleObject(ov.hEvent, 10000);
                Error = WSAETIMEDOUT;
            }
            else
            {
                Error = WSAGetOverlappedResult(Listener, &ov, &Got, FALSE, &Flags) ? 0 : WSAGetLastError();
            }
        }
    }
    WSACloseEvent(ov.hEvent);
    return Error;
}

/* AcceptEx (AfdSuperAccept) */
static
void
TestAcceptEx(void)
{
    GUID Guid = WSAID_ACCEPTEX;
    DWORD n;
    TARGETS t;
    SOCKET lst, cli, srv;
    int Error;
    ULONG i;
    struct { PCSTR Name; SOCKET *Socket; } Cases[] =
    {
        { "connected (accepted end)", &t.PairServer },
        { "connected (client end)", &t.PairClient },
        { "another listener", &t.OtherListener },
        { "the listener itself", &lst },
        { "datagram (unbound)", &t.Udp },
        { "datagram (bound)", &t.UdpBound },
        { "bound, not connected", &t.Bound },
    };

    {
        BOOL Wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &Wow64);
        if (Wow64)
        {
            skip("AcceptEx: IOCTL_AFD_SUPER_ACCEPT is not thunked for WoW64\n");
            return;
        }
    }

    lst = MakeListener(&ListenAddr);
    if (lst == INVALID_SOCKET)
        return;
    if (WSAIoctl(lst, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid),
                 &pAcceptEx, sizeof(pAcceptEx), &n, NULL, NULL) != 0 || !pAcceptEx)
    {
        skip("No AcceptEx (%d)\n", WSAGetLastError());
        closesocket(lst);
        return;
    }
    MakeTargets(lst, &t);

    cli = Connect(&ListenAddr);
    ok(WaitForPending(lst), "no pending connection on the listener\n");
    for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
    {
        Error = TryAcceptEx(lst, *Cases[i].Socket);
        ok(Error == WSAEINVAL, "AcceptEx into %s: expected WSAEINVAL, got %d\n", Cases[i].Name, Error);
    }

    CheckTargetsUsable("AcceptEx", &t);

    /* The connection is still queued: an AcceptEx into a fresh socket takes it */
    srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Error = TryAcceptEx(lst, srv);
    ok(Error == 0, "AcceptEx into a fresh socket failed %d\n", Error);
    if (Error == 0)
    {
        setsockopt(srv, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&lst, sizeof(lst));
        ok(PingPong(cli, srv, 'x'), "queued connection, client to server failed %d\n", WSAGetLastError());
        ok(PingPong(srv, cli, 'y'), "queued connection, server to client failed %d\n", WSAGetLastError());
    }
    closesocket(srv);
    closesocket(cli);

    /* A socket that only had an option set (never bound) is still fresh */
    cli = Connect(&ListenAddr);
    ok(WaitForPending(lst), "no pending connection on the listener\n");
    srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    n = 32768;
    ok(setsockopt(srv, SOL_SOCKET, SO_RCVBUF, (char *)&n, sizeof(n)) == 0, "SO_RCVBUF failed %d\n", WSAGetLastError());
    Error = TryAcceptEx(lst, srv);
    ok(Error == 0, "AcceptEx into a socket with SO_RCVBUF set failed %d\n", Error);
    if (Error == 0)
    {
        setsockopt(srv, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&lst, sizeof(lst));
        ok(PingPong(cli, srv, 'x'), "SO_RCVBUF socket, client to server failed %d\n", WSAGetLastError());
        ok(PingPong(srv, cli, 'y'), "SO_RCVBUF socket, server to client failed %d\n", WSAGetLastError());
    }
    closesocket(srv);
    closesocket(cli);

    FreeTargets(&t);
    closesocket(lst);
}

/* IOCTL_AFD_ACCEPT into a socket that only had an option set */
static
void
TestPlainAcceptSetopt(void)
{
    SOCKET lst, cli, tgt;
    ULONG Seq = 0;
    NTSTATUS Status;
    int n = 32768;

    lst = MakeListener(&ListenAddr);
    if (lst == INVALID_SOCKET)
        return;
    cli = Connect(&ListenAddr);
    tgt = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(setsockopt(tgt, SOL_SOCKET, SO_RCVBUF, (char *)&n, sizeof(n)) == 0, "SO_RCVBUF failed %d\n", WSAGetLastError());
    Status = WaitForListen(lst, &Seq);
    ok(Status == STATUS_SUCCESS, "WAIT_FOR_LISTEN: got 0x%lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        Status = RawAccept(lst, Seq, tgt);
        ok(Status == STATUS_SUCCESS,
           "IOCTL_AFD_ACCEPT into a socket with SO_RCVBUF set: expected success, got 0x%lx\n", Status);
    }
    closesocket(tgt);
    closesocket(cli);
    closesocket(lst);
}

/* Two listeners accept into each other from two threads. Both requests must
 * fail at once; taking the second socket's lock while holding the first's
 * deadlocks them (the kernel waits are not alertable, so the threads cannot be
 * killed either). A watchdog reports that instead of hanging the run. */
#define CROSS_ROUNDS 400
#define WATCHDOG_MS 30000

typedef struct _CROSS
{
    SOCKET Listener, Target;
    ULONG Seq;
    BOOL SuperAccept;
    HANDLE Go;
    ULONG Done, Wrong;
    NTSTATUS LastStatus;
    int LastError;
} CROSS;

static
DWORD
WINAPI
CrossThread(LPVOID Param)
{
    CROSS *c = Param;
    ULONG i;

    WaitForSingleObject(c->Go, INFINITE);
    for (i = 0; i < CROSS_ROUNDS; i++)
    {
        if (c->SuperAccept)
        {
            int Error = TryAcceptEx(c->Listener, c->Target);
            if (Error != WSAEINVAL)
            {
                c->Wrong++;
                c->LastError = Error;
            }
        }
        else
        {
            NTSTATUS Status = RawAccept(c->Listener, c->Seq, c->Target);
            if (Status != STATUS_INVALID_PARAMETER)
            {
                c->Wrong++;
                c->LastStatus = Status;
            }
        }
        c->Done++;
    }
    return 0;
}

static
void
TestCrossAccept(BOOL SuperAccept)
{
    PCSTR Name = SuperAccept ? "AcceptEx" : "IOCTL_AFD_ACCEPT";
    SOCKET a, b, ca, cb;
    struct sockaddr_in aa, ba;
    CROSS x[2];
    HANDLE Threads[2];
    HANDLE Go;
    DWORD Wait;
    NTSTATUS Status;
    ULONG Seq;

    if (SuperAccept && !pAcceptEx)
    {
        skip("AcceptEx cross accept: no AcceptEx (or WoW64)\n");
        return;
    }

    a = MakeListener(&aa);
    b = MakeListener(&ba);
    if (a == INVALID_SOCKET || b == INVALID_SOCKET)
        return;
    ca = Connect(&aa);
    cb = Connect(&ba);
    ok(WaitForPending(a), "%s: no pending connection on A\n", Name);
    ok(WaitForPending(b), "%s: no pending connection on B\n", Name);

    Go = CreateEventW(NULL, TRUE, FALSE, NULL);
    memset(x, 0, sizeof(x));
    x[0].Listener = a; x[0].Target = b;
    x[1].Listener = b; x[1].Target = a;
    x[0].SuperAccept = x[1].SuperAccept = SuperAccept;
    x[0].Go = x[1].Go = Go;
    if (!SuperAccept)
    {
        Status = WaitForListen(a, &Seq);
        ok(Status == STATUS_SUCCESS, "WAIT_FOR_LISTEN A: got 0x%lx\n", Status);
        x[0].Seq = Seq;
        Status = WaitForListen(b, &Seq);
        ok(Status == STATUS_SUCCESS, "WAIT_FOR_LISTEN B: got 0x%lx\n", Status);
        x[1].Seq = Seq;
    }

    Threads[0] = CreateThread(NULL, 0, CrossThread, &x[0], 0, NULL);
    Threads[1] = CreateThread(NULL, 0, CrossThread, &x[1], 0, NULL);
    SetEvent(Go);
    Wait = WaitForMultipleObjects(2, Threads, TRUE, WATCHDOG_MS);
    if (Wait != WAIT_OBJECT_0)
    {
        ok(0, "%s: cross accept deadlocked (done %lu/%lu and %lu/%lu rounds)\n",
           Name, x[0].Done, (ULONG)CROSS_ROUNDS, x[1].Done, (ULONG)CROSS_ROUNDS);
        /* The stuck threads cannot be terminated and the sockets cannot be
         * closed; end the process now so that the failure is reported. */
        {
            /* Straight to the output handle: the C library buffers may not be flushed in time */
            char Msg[160];
            DWORD Written;
            int Len = _snprintf(Msg, sizeof(Msg),
                                "acceptstate: %s cross accept DEADLOCK (rounds %lu and %lu of %lu)\r\n",
                                Name, x[0].Done, x[1].Done, (ULONG)CROSS_ROUNDS);
            _flushall();
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Msg, Len, &Written, NULL);
            FlushFileBuffers(GetStdHandle(STD_OUTPUT_HANDLE));
        }
        TerminateProcess(GetCurrentProcess(), 1);
        return;
    }
    ok(x[0].Wrong == 0 && x[1].Wrong == 0,
       "%s: cross accept: %lu+%lu requests did not fail with the invalid parameter error (0x%lx, %d)\n",
       Name, x[0].Wrong, x[1].Wrong, x[0].LastStatus ? x[0].LastStatus : x[1].LastStatus,
       x[0].LastError ? x[0].LastError : x[1].LastError);

    CloseHandle(Threads[0]);
    CloseHandle(Threads[1]);
    CloseHandle(Go);
    closesocket(ca);
    closesocket(cb);
    closesocket(a);
    closesocket(b);
}

START_TEST(acceptstate)
{
    WSADATA wd;

    ok(WSAStartup(MAKEWORD(2, 2), &wd) == 0, "WSAStartup failed\n");
    TestPlainAccept();
    TestPlainAcceptSetopt();
    TestAcceptEx();
    TestCrossAccept(FALSE);
    TestCrossAccept(TRUE);
    WSACleanup();
}
