/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for AcceptEx with an initial receive
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#include <winsock2.h>
#include <mswsock.h>

#define DATA_LENGTH 256
#define ADDRESS_LENGTH (sizeof(SOCKADDR_IN) + 16)
#define BUFFER_SIZE 8192
#define CANARY 0xCC
#define WAIT_MS 5000

typedef enum _SEND_MODE
{
    SendAfterPost,      /* the connection arrives while AcceptEx waits */
    SendBeforePost,     /* the connection and its data wait in the backlog */
    SendPartial,        /* fewer bytes than the receive length */
    SendNothing         /* the client closes without sending */
} SEND_MODE;

typedef struct _CLOSE_JOB
{
    SOCKET Sockets[3];
} CLOSE_JOB;

/* Set when a close or a request did not finish: nothing after it touches the sockets or WSACleanup */
static BOOL TeardownHung;
static LPFN_ACCEPTEX pAcceptExExtension;
static LPFN_GETACCEPTEXSOCKADDRS pGetAcceptExSockaddrsExtension;

static DWORD WINAPI CloseThread(PVOID Parameter)
{
    CLOSE_JOB *Job = Parameter;
    ULONG i;

    for (i = 0; i < ARRAYSIZE(Job->Sockets); i++)
    {
        if (Job->Sockets[i] != INVALID_SOCKET)
            closesocket(Job->Sockets[i]);
    }
    return 0;
}

/* A close can wait without bound for a transport request that never completes, so it runs
 * on a worker; if the worker does not finish, its job and handle are left to it for good */
static BOOL CloseSockets(SOCKET First, SOCKET Second, SOCKET Third)
{
    CLOSE_JOB *Job;
    HANDLE Thread;

    Job = HeapAlloc(GetProcessHeap(), 0, sizeof(*Job));
    if (!Job)
        return FALSE;
    Job->Sockets[0] = First;
    Job->Sockets[1] = Second;
    Job->Sockets[2] = Third;

    Thread = CreateThread(NULL, 0, CloseThread, Job, 0, NULL);
    if (!Thread)
        return FALSE;
    if (WaitForSingleObject(Thread, WAIT_MS) != WAIT_OBJECT_0)
        return FALSE;
    CloseHandle(Thread);
    HeapFree(GetProcessHeap(), 0, Job);
    return TRUE;
}

static UCHAR Pattern(ULONG Index)
{
    return (UCHAR)((Index * 7 + 0x41) & 0xFF);
}

static BOOL CreateListener(SOCKET *Listener, SOCKADDR_IN *Address)
{
    int Length = sizeof(*Address);

    *Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(*Listener != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (*Listener == INVALID_SOCKET)
        return FALSE;

    ZeroMemory(Address, sizeof(*Address));
    Address->sin_family = AF_INET;
    Address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(*Listener, (SOCKADDR *)Address, sizeof(*Address)) ||
        getsockname(*Listener, (SOCKADDR *)Address, &Length) ||
        listen(*Listener, 5))
    {
        ok(0, "listener setup failed: %d\n", WSAGetLastError());
        closesocket(*Listener);
        *Listener = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

static BOOL SendAll(SOCKET Socket, const UCHAR *Data, int Length)
{
    while (Length > 0)
    {
        int Sent = send(Socket, (const char *)Data, Length, 0);
        if (Sent <= 0)
            return FALSE;
        Data += Sent;
        Length -= Sent;
    }
    return TRUE;
}

static BOOL WaitReadable(SOCKET Socket)
{
    struct timeval Timeout = { WAIT_MS / 1000, 0 };
    fd_set ReadSet;

    FD_ZERO(&ReadSet);
    FD_SET(Socket, &ReadSet);
    return select(0, &ReadSet, NULL, NULL, &Timeout) == 1;
}

/* Reads until Length bytes arrived, the peer closed or nothing came for WAIT_MS; returns the count */
static int ReceiveAll(SOCKET Socket, UCHAR *Data, int Length)
{
    int Total = 0;

    while (Total < Length && WaitReadable(Socket))
    {
        int Received = recv(Socket, (char *)Data + Total, Length - Total, 0);
        if (Received <= 0)
            break;
        Total += Received;
    }
    return Total;
}

static void CheckSockaddr(const SOCKADDR *Sockaddr, INT Length, const SOCKADDR_IN *Expected,
                          const UCHAR *AreaStart, const char *Name)
{
    const SOCKADDR_IN *Address = (const SOCKADDR_IN *)Sockaddr;

    ok(Length == sizeof(SOCKADDR_IN), "%s length = %d\n", Name, Length);
    ok((const UCHAR *)Sockaddr >= AreaStart &&
       (const UCHAR *)Sockaddr + sizeof(SOCKADDR_IN) <= AreaStart + ADDRESS_LENGTH,
       "%s address %p outside its area %p\n", Name, Sockaddr, AreaStart);
    if ((const UCHAR *)Sockaddr < AreaStart || (const UCHAR *)Sockaddr >= AreaStart + ADDRESS_LENGTH)
        return;
    ok(Address->sin_family == AF_INET, "%s family = %u\n", Name, Address->sin_family);
    ok(Address->sin_port == Expected->sin_port, "%s port = %u, expected %u\n",
       Name, ntohs(Address->sin_port), ntohs(Expected->sin_port));
    ok(Address->sin_addr.s_addr == Expected->sin_addr.s_addr, "%s address = %08lx, expected %08lx\n",
       Name, (ULONG)ntohl(Address->sin_addr.s_addr), (ULONG)ntohl(Expected->sin_addr.s_addr));
}

static void TestAcceptEx(LPFN_ACCEPTEX pAcceptEx, LPFN_GETACCEPTEXSOCKADDRS pGetAcceptExSockaddrs,
                         const char *Via, DWORD ReceiveLength, DWORD LocalLength, SEND_MODE Mode)
{
    SOCKET Listener, Acceptor = INVALID_SOCKET, Client = INVALID_SOCKET;
    SOCKADDR_IN ListenAddress, ClientAddress;
    int Length, Sent, Received, Total;
    DWORD Bytes = 0, Flags = 0, Wait, Used, i, Bad;
    OVERLAPPED *Overlapped;
    UCHAR *Buffer = NULL, Data[DATA_LENGTH], Rest[DATA_LENGTH];
    SOCKADDR *LocalSockaddr = NULL, *RemoteSockaddr = NULL;
    INT LocalSockaddrLength = 0, RemoteSockaddrLength = 0;
    BOOL Ret, Pending = FALSE;

    if (TeardownHung)
        return;

    trace("%s: receive length %lu, local length %lu, mode %d\n", Via, ReceiveLength, LocalLength, Mode);

    /* Left allocated if the request never completes */
    Overlapped = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Overlapped));
    if (!Overlapped)
        return;
    Overlapped->hEvent = WSACreateEvent();
    if (!CreateListener(&Listener, &ListenAddress))
        goto cleanup;

    Acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Acceptor != INVALID_SOCKET && Client != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Acceptor == INVALID_SOCKET || Client == INVALID_SOCKET)
        goto cleanup;

    Buffer = VirtualAlloc(NULL, BUFFER_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok(Buffer != NULL, "VirtualAlloc failed: %lu\n", GetLastError());
    if (!Buffer)
        goto cleanup;
    memset(Buffer, CANARY, BUFFER_SIZE);
    Used = ReceiveLength + LocalLength + ADDRESS_LENGTH;

    Sent = (Mode == SendNothing) ? 0 : (Mode == SendPartial) ? 10 : DATA_LENGTH;
    for (i = 0; i < DATA_LENGTH; i++)
        Data[i] = Pattern(i);

    if (Mode == SendBeforePost)
    {
        ok(connect(Client, (SOCKADDR *)&ListenAddress, sizeof(ListenAddress)) == 0,
           "connect failed: %d\n", WSAGetLastError());
        ok(SendAll(Client, Data, Sent), "send failed: %d\n", WSAGetLastError());
        Sleep(200);
    }

    Ret = pAcceptEx(Listener, Acceptor, Buffer, ReceiveLength, LocalLength, ADDRESS_LENGTH,
                    &Bytes, Overlapped);
    ok(Ret || WSAGetLastError() == ERROR_IO_PENDING, "AcceptEx returned %d, error %d\n",
       Ret, WSAGetLastError());
    if (!Ret && WSAGetLastError() != ERROR_IO_PENDING)
        goto cleanup;
    Pending = !Ret;

    if (Mode != SendBeforePost)
    {
        ok(connect(Client, (SOCKADDR *)&ListenAddress, sizeof(ListenAddress)) == 0,
           "connect failed: %d\n", WSAGetLastError());
        if (ReceiveLength)
        {
            /* AcceptEx waits for the first data */
            Wait = WaitForSingleObject(Overlapped->hEvent, 300);
            ok(Wait == WAIT_TIMEOUT, "AcceptEx completed before any data: %lu\n", Wait);
        }
        if (Mode == SendNothing)
            ok(shutdown(Client, SD_SEND) == 0, "shutdown failed: %d\n", WSAGetLastError());
        else
            ok(SendAll(Client, Data, Sent), "send failed: %d\n", WSAGetLastError());
    }

    Wait = WaitForSingleObject(Overlapped->hEvent, WAIT_MS);
    ok(Wait == WAIT_OBJECT_0, "AcceptEx did not complete: %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        goto cleanup;
    Pending = FALSE;
    Ret = WSAGetOverlappedResult(Listener, Overlapped, &Bytes, FALSE, &Flags);
    ok(Ret, "AcceptEx failed: %d\n", WSAGetLastError());
    if (!Ret)
        goto cleanup;

    if (!ReceiveLength || Mode == SendNothing)
        ok(Bytes == 0, "received %lu bytes, expected 0\n", Bytes);
    else if (Mode == SendPartial)
        ok(Bytes == (DWORD)Sent, "received %lu bytes, expected %d\n", Bytes, Sent);
    else
        ok(Bytes >= 1 && Bytes <= ReceiveLength, "received %lu bytes\n", Bytes);
    if (Bytes > ReceiveLength)
        Bytes = ReceiveLength;

    /* The initial data starts the buffer */
    for (i = 0, Bad = 0; i < Bytes; i++)
        Bad += (Buffer[i] != Data[i]);
    ok(Bad == 0, "%lu of %lu received bytes wrong\n", Bad, Bytes);

    /* Nothing after the three areas is written */
    for (i = Used, Bad = 0; i < BUFFER_SIZE; i++)
        Bad += (Buffer[i] != CANARY);
    ok(Bad == 0, "%lu bytes written past the %lu-byte buffer\n", Bad, Used);

    ok(setsockopt(Acceptor, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&Listener, sizeof(Listener)) == 0,
       "SO_UPDATE_ACCEPT_CONTEXT failed: %d\n", WSAGetLastError());

    pGetAcceptExSockaddrs(Buffer, ReceiveLength, LocalLength, ADDRESS_LENGTH,
                          &LocalSockaddr, &LocalSockaddrLength, &RemoteSockaddr, &RemoteSockaddrLength);
    Length = sizeof(ClientAddress);
    ok(getsockname(Client, (SOCKADDR *)&ClientAddress, &Length) == 0, "getsockname failed: %d\n", WSAGetLastError());
    if (LocalLength == ADDRESS_LENGTH)
        CheckSockaddr(LocalSockaddr, LocalSockaddrLength, &ListenAddress, Buffer + ReceiveLength, "local");
    CheckSockaddr(RemoteSockaddr, RemoteSockaddrLength, &ClientAddress, Buffer + ReceiveLength + LocalLength, "remote");

    /* The connection keeps receiving after the initial data */
    Total = Bytes;
    if (Total < Sent)
    {
        Received = ReceiveAll(Acceptor, Rest, Sent - Total);
        ok(Received == Sent - Total, "received %d of the remaining %d bytes, error %d\n",
           Received, Sent - Total, WSAGetLastError());
        for (i = 0, Bad = 0; i < (DWORD)Received; i++)
            Bad += (Rest[i] != Data[Total + i]);
        ok(Bad == 0, "%lu of %d remaining bytes wrong\n", Bad, Received);
        Total += Received;
    }
    if (Mode != SendNothing)
    {
        ok(SendAll(Client, Data, 8), "second send failed: %d\n", WSAGetLastError());
        Received = ReceiveAll(Acceptor, Rest, 8);
        ok(Received == 8, "second receive got %d bytes, error %d\n", Received, WSAGetLastError());
        ok(Received != 8 || !memcmp(Rest, Data, 8), "second receive data wrong\n");
        ok(shutdown(Client, SD_SEND) == 0, "shutdown failed: %d\n", WSAGetLastError());
    }
    if (WaitReadable(Acceptor))
    {
        Received = recv(Acceptor, (char *)Rest, sizeof(Rest), 0);
        ok(Received == 0, "receive after the peer closed returned %d, error %d\n", Received, WSAGetLastError());
    }
    else
    {
        ok(0, "the peer's close was not seen\n");
    }

cleanup:
    /* Closing the sockets ends a request that is still pending. If that does not finish, the
     * transport may still use the buffer and the OVERLAPPED, so they are left allocated */
    if (!CloseSockets(Client, Acceptor, Listener))
    {
        ok(0, "closing the sockets did not finish\n");
        TeardownHung = TRUE;
        return;
    }
    if (Pending && Overlapped && WaitForSingleObject(Overlapped->hEvent, WAIT_MS) != WAIT_OBJECT_0)
    {
        ok(0, "AcceptEx still pending after its sockets were closed\n");
        TeardownHung = TRUE;
        return;
    }
    if (Buffer)
        VirtualFree(Buffer, 0, MEM_RELEASE);
    if (Overlapped)
    {
        WSACloseEvent(Overlapped->hEvent);
        HeapFree(GetProcessHeap(), 0, Overlapped);
    }
}

START_TEST(AcceptEx)
{
    GUID AcceptExGuid = WSAID_ACCEPTEX, SockaddrsGuid = WSAID_GETACCEPTEXSOCKADDRS;
    WSADATA WsaData;
    SOCKET Socket;
    DWORD Bytes;
    int Error;

    Error = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok(Error == 0, "WSAStartup failed: %d\n", Error);
    if (Error)
        return;

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Socket != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Socket != INVALID_SOCKET)
    {
        ok(!WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &AcceptExGuid, sizeof(AcceptExGuid),
                     &pAcceptExExtension, sizeof(pAcceptExExtension), &Bytes, NULL, NULL),
           "AcceptEx pointer: %d\n", WSAGetLastError());
        ok(!WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &SockaddrsGuid, sizeof(SockaddrsGuid),
                     &pGetAcceptExSockaddrsExtension, sizeof(pGetAcceptExSockaddrsExtension), &Bytes, NULL, NULL),
           "GetAcceptExSockaddrs pointer: %d\n", WSAGetLastError());
        closesocket(Socket);
    }

    if (pAcceptExExtension && pGetAcceptExSockaddrsExtension)
    {
        TestAcceptEx(pAcceptExExtension, pGetAcceptExSockaddrsExtension, "extension", DATA_LENGTH, ADDRESS_LENGTH, SendAfterPost);
        TestAcceptEx(pAcceptExExtension, pGetAcceptExSockaddrsExtension, "extension", DATA_LENGTH, ADDRESS_LENGTH, SendBeforePost);
        TestAcceptEx(pAcceptExExtension, pGetAcceptExSockaddrsExtension, "extension", DATA_LENGTH, ADDRESS_LENGTH, SendPartial);
        TestAcceptEx(pAcceptExExtension, pGetAcceptExSockaddrsExtension, "extension", DATA_LENGTH, ADDRESS_LENGTH, SendNothing);
        TestAcceptEx(pAcceptExExtension, pGetAcceptExSockaddrsExtension, "extension", 0, ADDRESS_LENGTH, SendAfterPost);
        /* No local address area, as Windows allows */
        TestAcceptEx(pAcceptExExtension, pGetAcceptExSockaddrsExtension, "extension", 0, 0, SendAfterPost);
        TestAcceptEx(pAcceptExExtension, pGetAcceptExSockaddrsExtension, "extension", DATA_LENGTH, 0, SendAfterPost);
    }
    TestAcceptEx(AcceptEx, GetAcceptExSockaddrs, "mswsock", DATA_LENGTH, ADDRESS_LENGTH, SendAfterPost);
    TestAcceptEx(AcceptEx, GetAcceptExSockaddrs, "mswsock", 0, ADDRESS_LENGTH, SendAfterPost);

    /* A close or a request that never finished leaves the stack in an unknown state */
    if (TeardownHung)
    {
        skip("a socket close or an AcceptEx request did not finish\n");
        return;
    }
    WSACleanup();
}
