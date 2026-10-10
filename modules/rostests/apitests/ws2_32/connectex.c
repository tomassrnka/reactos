/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test ConnectEx on a socket with a ConnectEx in progress, done or failed
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <mswsock.h>
#include <iphlpapi.h>

typedef struct _CX_REQUEST
{
    OVERLAPPED Overlapped;
    LONG Packets;
    DWORD Error;
    DWORD Bytes;
} CX_REQUEST, *PCX_REQUEST;

static LPFN_CONNECTEX pConnectEx;
static HANDLE Port;

static
BOOL
GetConnectEx(SOCKET Socket)
{
    GUID Guid = WSAID_CONNECTEX;
    DWORD Bytes;

    if (pConnectEx)
        return TRUE;
    return WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid),
                    &pConnectEx, sizeof(pConnectEx), &Bytes, NULL, NULL) == 0;
}

/* A TCP socket bound to Address (port 0) and associated with the completion port */
static
SOCKET
BoundSocket(ULONG Address)
{
    SOCKET Socket;
    struct sockaddr_in Addr;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = Address;
    if (bind(Socket, (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR ||
        !GetConnectEx(Socket) ||
        CreateIoCompletionPort((HANDLE)Socket, Port, 0, 0) != Port)
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }
    return Socket;
}

static
SOCKET
Listener(struct sockaddr_in *Addr)
{
    SOCKET Socket;
    int Len = sizeof(*Addr);

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;
    ZeroMemory(Addr, sizeof(*Addr));
    Addr->sin_family = AF_INET;
    Addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Socket, (struct sockaddr *)Addr, sizeof(*Addr)) == SOCKET_ERROR ||
        listen(Socket, 4) == SOCKET_ERROR ||
        getsockname(Socket, (struct sockaddr *)Addr, &Len) == SOCKET_ERROR)
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }
    return Socket;
}

/* Collect completion packets until none arrives for Milliseconds */
static
void
Drain(DWORD Milliseconds)
{
    DWORD Bytes;
    ULONG_PTR Key;
    LPOVERLAPPED Overlapped;
    PCX_REQUEST Request;
    BOOL Ret;

    for (;;)
    {
        Overlapped = NULL;
        Ret = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Overlapped, Milliseconds);
        if (!Overlapped)
            break;
        Request = CONTAINING_RECORD(Overlapped, CX_REQUEST, Overlapped);
        Request->Packets++;
        Request->Error = Ret ? 0 : GetLastError();
        Request->Bytes = Bytes;
    }
}

/* Start a ConnectEx; returns 0 if it completed or is pending, else the error */
static
int
StartConnectEx(SOCKET Socket, const struct sockaddr_in *Addr, PVOID Buffer, DWORD Length, PCX_REQUEST Request)
{
    DWORD Sent = 0;

    ZeroMemory(Request, sizeof(*Request));
    if (pConnectEx(Socket, (const struct sockaddr *)Addr, sizeof(*Addr), Buffer, Length, &Sent, &Request->Overlapped))
        return 0;
    return WSAGetLastError() == ERROR_IO_PENDING ? 0 : WSAGetLastError();
}

/* Wait until the request has a completion packet */
static
BOOL
WaitRequest(PCX_REQUEST Request, DWORD Milliseconds)
{
    DWORD Start = GetTickCount();

    while (!Request->Packets && GetTickCount() - Start < Milliseconds)
        Drain(100);
    return Request->Packets != 0;
}

/* An unused address on a local IPv4 subnet: nothing answers its ARP request,
 * so a connect to it stays pending for seconds */
static
BOOL
SilentAddress(struct sockaddr_in *Addr)
{
    IP_ADAPTER_INFO *Info, *Adapter;
    IP_ADDR_STRING *Ip;
    ULONG Size = 0, Local = 0, Mask = 0, Candidate;

    if (GetAdaptersInfo(NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return FALSE;
    Info = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Info)
        return FALSE;
    if (GetAdaptersInfo(Info, &Size) == ERROR_SUCCESS)
    {
        for (Adapter = Info; Adapter && !Local; Adapter = Adapter->Next)
        {
            for (Ip = &Adapter->IpAddressList; Ip && !Local; Ip = Ip->Next)
            {
                Candidate = ntohl(inet_addr(Ip->IpAddress.String));
                Mask = ntohl(inet_addr(Ip->IpMask.String));
                if (Candidate && Candidate != INADDR_NONE && (Candidate >> 24) != 127 &&
                    (Candidate >> 16) != 0xA9FE && Mask && (~Mask) >= 8)
                    Local = Candidate;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, Info);
    if (!Local)
        return FALSE;

    /* Five below the broadcast address; for a /24 that is .250 */
    Candidate = (Local & Mask) | (~Mask - 5);
    if (Candidate == Local)
        Candidate--;
    ZeroMemory(Addr, sizeof(*Addr));
    Addr->sin_family = AF_INET;
    Addr->sin_port = htons(9);
    Addr->sin_addr.s_addr = htonl(Candidate);
    return TRUE;
}

/* accept, bounded: a connect that failed leaves nothing to accept */
static
SOCKET
AcceptWithin(SOCKET Server, DWORD Milliseconds)
{
    fd_set ReadSet;
    struct timeval Timeout;

    Timeout.tv_sec = Milliseconds / 1000;
    Timeout.tv_usec = (Milliseconds % 1000) * 1000;
    FD_ZERO(&ReadSet);
    FD_SET(Server, &ReadSet);
    if (select(0, &ReadSet, NULL, NULL, &Timeout) != 1)
        return INVALID_SOCKET;
    return accept(Server, NULL, NULL);
}

/* Each case has its own static requests and send data: a completion that
 * arrives after a failed check must land neither on a stack frame that is gone
 * nor on a later case's request */
/* The first ConnectEx is still connecting: the second one fails at once with
 * WSAEINVAL (Windows Server 2008 R2 and Windows 10) and leaves the first pending */
static
void
TestSecondWhileConnecting(void)
{
    static CX_REQUEST First, Second, Third;
    static char FirstData[32], SecondData[32];
    SOCKET Client;
    struct sockaddr_in Silent;
    int Error, SecondError, ThirdError, i;

    /* A test run right after boot may still wait for its DHCP lease */
    for (i = 0; i < 60 && !SilentAddress(&Silent); i++)
        Sleep(500);
    if (i == 60)
    {
        skip("No local IPv4 subnet to connect into\n");
        return;
    }
    Client = BoundSocket(htonl(INADDR_ANY));
    if (Client == INVALID_SOCKET)
    {
        skip("Setup failed: %d\n", WSAGetLastError());
        return;
    }
    memset(FirstData, 'A', sizeof(FirstData));
    memset(SecondData, 'B', sizeof(SecondData));

    Error = StartConnectEx(Client, &Silent, FirstData, sizeof(FirstData), &First);
    Drain(300);
    if (Error || First.Packets)
    {
        skip("The connect to the silent address did not stay pending (error %d, %lu)\n", Error, First.Error);
        closesocket(Client);
        Drain(100);
        return;
    }

    SecondError = StartConnectEx(Client, &Silent, SecondData, sizeof(SecondData), &Second);
    ThirdError = StartConnectEx(Client, &Silent, NULL, 0, &Third);
    Drain(300);
    if (First.Packets)
    {
        /* Something answered after all: the socket is no longer connecting */
        skip("The connect to the silent address ended early (error %lu)\n", First.Error);
    }
    else
    {
        ok(SecondError == WSAEINVAL, "Second ConnectEx while connecting: error %d\n", SecondError);
        ok(ThirdError == WSAEINVAL, "Second ConnectEx without data while connecting: error %d\n", ThirdError);
        ok(Second.Packets == 0, "Second ConnectEx got %ld completions\n", Second.Packets);
        ok(Third.Packets == 0, "Third ConnectEx got %ld completions\n", Third.Packets);
    }

    closesocket(Client);
    if (!First.Packets)
    {
        ok(WaitRequest(&First, 5000), "First ConnectEx did not complete after the close\n");
        ok(First.Packets == 1, "First ConnectEx got %ld completions\n", First.Packets);
        /* ERROR_OPERATION_ABORTED, unless the connect failed on its own just before the close */
        if (First.Error != ERROR_OPERATION_ABORTED)
            skip("The first ConnectEx ended with %lu, not by the close\n", First.Error);
        Drain(200);
        ok(Second.Packets == 0, "Second ConnectEx got %ld completions\n", Second.Packets);
        ok(Third.Packets == 0, "Third ConnectEx got %ld completions\n", Third.Packets);
    }
    Drain(200);
}

/* After a ConnectEx completed, another one reports the socket connected */
static
void
TestSecondAfterConnected(void)
{
    static CX_REQUEST First, Second, Third;
    static char FirstData[32];
    SOCKET Server, Client, Accepted;
    struct sockaddr_in Addr;
    int Error;

    Server = Listener(&Addr);
    Client = BoundSocket(htonl(INADDR_LOOPBACK));
    if (Server == INVALID_SOCKET || Client == INVALID_SOCKET)
    {
        skip("Setup failed: %d\n", WSAGetLastError());
        goto Cleanup;
    }
    memcpy(FirstData, "connectex data", sizeof("connectex data"));

    Error = StartConnectEx(Client, &Addr, FirstData, 16, &First);
    ok(Error == 0, "First ConnectEx failed: %d\n", Error);
    if (Error)
        goto Cleanup;
    Accepted = AcceptWithin(Server, 5000);
    ok(Accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    ok(WaitRequest(&First, 5000), "First ConnectEx did not complete\n");
    ok(First.Error == 0 && First.Bytes == 16, "First ConnectEx: error %lu, %lu bytes\n",
       First.Error, First.Bytes);
    if (Accepted == INVALID_SOCKET)
        goto Cleanup;

    Error = StartConnectEx(Client, &Addr, FirstData, 16, &Second);
    ok(Error == WSAEISCONN, "ConnectEx on a connected socket: error %d\n", Error);
    ok(setsockopt(Client, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) == 0,
       "SO_UPDATE_CONNECT_CONTEXT failed: %d\n", WSAGetLastError());
    Error = StartConnectEx(Client, &Addr, NULL, 0, &Third);
    ok(Error == WSAEISCONN, "ConnectEx after SO_UPDATE_CONNECT_CONTEXT: error %d\n", Error);
    Drain(300);
    ok(Second.Packets == 0, "Second ConnectEx got %ld completions\n", Second.Packets);
    ok(Third.Packets == 0, "Third ConnectEx got %ld completions\n", Third.Packets);
    ok(First.Packets == 1, "First ConnectEx got %ld completions\n", First.Packets);

    closesocket(Accepted);
Cleanup:
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    Drain(200);
}

/* A loopback port that is bound but not listening refuses the connect. (A
 * local ReactOS fault-injection build, not in the tree, fails this request's
 * transport connect before submission instead.) Either way the request
 * completes exactly once, or fails at the call with no completion, and a later
 * ConnectEx on the same socket gets only its own completion. */
static
void
TestFailedThenAgain(void)
{
    static CX_REQUEST First, Second;
    static char FirstData[32], SecondData[32];
    SOCKET Server, Client, Reserved, Accepted = INVALID_SOCKET;
    struct sockaddr_in Addr, Closed;
    int Error, Len = sizeof(Closed);

    Server = Listener(&Addr);
    Client = BoundSocket(htonl(INADDR_LOOPBACK));
    Reserved = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Closed = Addr;
    Closed.sin_port = 0;
    if (Server == INVALID_SOCKET || Client == INVALID_SOCKET || Reserved == INVALID_SOCKET ||
        bind(Reserved, (struct sockaddr *)&Closed, sizeof(Closed)) == SOCKET_ERROR ||
        getsockname(Reserved, (struct sockaddr *)&Closed, &Len) == SOCKET_ERROR)
    {
        skip("Setup failed: %d\n", WSAGetLastError());
        goto Cleanup;
    }
    memcpy(FirstData, "refused request", sizeof("refused request"));
    memcpy(SecondData, "connectex data", sizeof("connectex data"));

    Error = StartConnectEx(Client, &Closed, FirstData, 16, &First);
    if (Error)
    {
        trace("ConnectEx to a closed port failed at the call: %d\n", Error);
        Drain(1000);
        ok(First.Packets == 0, "A ConnectEx that failed at the call got %ld completions\n", First.Packets);
    }
    else
    {
        ok(WaitRequest(&First, 10000), "ConnectEx to a closed port did not complete\n");
        Drain(300);
        ok(First.Packets == 1, "ConnectEx to a closed port got %ld completions\n", First.Packets);
        ok(First.Error != 0, "ConnectEx to a closed port succeeded\n");
        trace("ConnectEx to a closed port completed with %lu\n", First.Error);
        if (!First.Packets)
            goto Cleanup;
    }

    First.Packets = 0;
    Error = StartConnectEx(Client, &Addr, SecondData, 16, &Second);
    if (Error)
    {
        trace("ConnectEx after a failed one: error %d\n", Error);
        Drain(500);
        ok(Second.Packets == 0, "A ConnectEx that failed at the call got %ld completions\n", Second.Packets);
    }
    else
    {
        Accepted = AcceptWithin(Server, 5000);
        ok(Accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
        ok(WaitRequest(&Second, 5000), "ConnectEx after a failed one did not complete\n");
        Drain(500);
        ok(Second.Packets == 1, "ConnectEx after a failed one got %ld completions\n", Second.Packets);
        ok(Second.Error == 0, "ConnectEx after a failed one: error %lu\n", Second.Error);
    }
    ok(First.Packets == 0, "The failed ConnectEx completed again with %lu\n", First.Error);

Cleanup:
    if (Accepted != INVALID_SOCKET)
        closesocket(Accepted);
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    if (Reserved != INVALID_SOCKET)
        closesocket(Reserved);
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    Drain(200);
}

START_TEST(connectex)
{
    WSADATA WsaData;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");
    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed: %lu\n", GetLastError());
    if (Port)
    {
        TestSecondAfterConnected();
        TestSecondWhileConnecting();
        TestFailedThenAgain();
        CloseHandle(Port);
    }
    WSACleanup();
}
