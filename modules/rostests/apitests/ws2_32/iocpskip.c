/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test FILE_SKIP_COMPLETION_PORT_ON_SUCCESS on sockets (the libuv pattern)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

#ifndef FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#define FILE_SKIP_SET_EVENT_ON_HANDLE        0x2
#endif

#define KEY 0x55

typedef BOOL (WINAPI *PFN_SetFileCompletionNotificationModes)(HANDLE, UCHAR);

/* Each request gets its own OVERLAPPED and buffer: a late completion never lands in a reused one */
typedef struct _REQUEST
{
    WSAOVERLAPPED Ov;
    char Data[64];
} REQUEST, *PREQUEST;

static REQUEST RequestPool[16];
static ULONG RequestNext;

static
PREQUEST
NewRequest(void)
{
    PREQUEST Request;

    if (RequestNext >= RTL_NUMBER_OF(RequestPool))
    {
        ok(FALSE, "out of requests\n");
        RequestNext = 0;
    }
    Request = &RequestPool[RequestNext++];
    ZeroMemory(Request, sizeof(*Request));
    return Request;
}

static
BOOL
GetPacket(
    HANDLE Port,
    DWORD Timeout,
    ULONG_PTR *Key,
    LPOVERLAPPED *Overlapped,
    DWORD *Bytes)
{
    *Key = 0xdeadbeef;
    *Overlapped = (LPOVERLAPPED)(ULONG_PTR)0xdeadbeef;
    *Bytes = 0xdeadbeef;
    return GetQueuedCompletionStatus(Port, Bytes, Key, Overlapped, Timeout) || *Overlapped != NULL;
}

/* Waits until Count bytes can be read from Socket */
static
BOOL
WaitReadable(
    SOCKET Socket,
    u_long Count)
{
    u_long Available = 0;
    ULONG i;

    for (i = 0; i < 500; i++)
    {
        if (ioctlsocket(Socket, FIONREAD, &Available) == 0 && Available >= Count)
            return TRUE;
        Sleep(10);
    }
    return FALSE;
}

/*
 * Checks the completion of a request that returned Result: an immediate success queues a
 * packet only without the skip flag, a pending request always queues one.
 */
static
void
CheckCompletion(
    HANDLE Port,
    PREQUEST Request,
    int Result,
    int Error,
    DWORD ImmediateBytes,
    DWORD ExpectedBytes,
    BOOL Skip,
    const char *What)
{
    ULONG_PTR Key;
    LPOVERLAPPED Pov;
    DWORD Bytes;
    BOOL Ret;

    if (Result == 0)
    {
        ok(ImmediateBytes == ExpectedBytes, "%s: %lu bytes\n", What, ImmediateBytes);
        Ret = GetPacket(Port, 200, &Key, &Pov, &Bytes);
        if (Skip)
            ok(!Ret, "%s: got a packet for an immediate success\n", What);
        else
            ok(Ret && Key == KEY && Pov == &Request->Ov && Bytes == ExpectedBytes,
               "%s: packet %d key %Ix ov %p bytes %lu\n", What, Ret, Key, Pov, Bytes);
    }
    else if (Error == WSA_IO_PENDING)
    {
        Ret = GetPacket(Port, 5000, &Key, &Pov, &Bytes);
        ok(Ret && Key == KEY && Pov == &Request->Ov && Bytes == ExpectedBytes,
           "%s: pending, packet %d key %Ix ov %p bytes %lu\n", What, Ret, Key, Pov, Bytes);
    }
    else
    {
        ok(FALSE, "%s: failed with error %d\n", What, Error);
    }
}

static
BOOL
MakePair(
    SOCKET *Server,
    SOCKET *Client)
{
    SOCKET Listener;
    struct sockaddr_in Addr;
    int Len = sizeof(Addr);
    BOOL Ok;

    *Server = *Client = INVALID_SOCKET;
    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET, "socket failed, error %d\n", WSAGetLastError());
    if (Listener == INVALID_SOCKET) return FALSE;
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Ok = bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == 0 &&
         getsockname(Listener, (struct sockaddr *)&Addr, &Len) == 0 &&
         listen(Listener, 1) == 0;
    ok(Ok, "listening socket setup failed, error %d\n", WSAGetLastError());
    if (Ok)
    {
        *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Ok = *Client != INVALID_SOCKET &&
             connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr)) == 0;
        ok(Ok, "connect failed, error %d\n", WSAGetLastError());
    }
    /* The connection is queued once connect returned, so accept does not block */
    if (Ok)
    {
        *Server = accept(Listener, NULL, NULL);
        Ok = *Server != INVALID_SOCKET;
        ok(Ok, "accept failed, error %d\n", WSAGetLastError());
    }
    closesocket(Listener);
    return Ok;
}

static
void
Test_Socket(
    PFN_SetFileCompletionNotificationModes pSetModes,
    BOOL Skip)
{
    SOCKET Server, Client;
    HANDLE Port = NULL;
    PREQUEST Request;
    WSABUF Buf;
    char Data[64];
    DWORD Bytes, Flags;
    ULONG_PTR Key;
    LPOVERLAPPED Pov;
    BOOL Ret;
    int Result, Error;

    if (!MakePair(&Server, &Client)) goto out;
    Port = CreateIoCompletionPort((HANDLE)Server, NULL, KEY, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed, error %lu\n", GetLastError());
    if (!Port) goto out;
    if (Skip)
    {
        Ret = pSetModes((HANDLE)Server,
                        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
        ok(Ret, "SetFileCompletionNotificationModes failed, error %lu\n", GetLastError());
    }

    /* Data waiting: a zero-byte read and a real read both complete at once */
    ok(send(Client, "hello", 5, 0) == 5, "send failed, error %d\n", WSAGetLastError());
    if (!WaitReadable(Server, 5))
    {
        ok(FALSE, "data did not arrive\n");
        goto out;
    }

    Request = NewRequest();
    Buf.buf = Request->Data;
    Buf.len = 0;
    Flags = 0;
    Bytes = 0;
    Result = WSARecv(Server, &Buf, 1, &Bytes, &Flags, &Request->Ov, NULL);
    Error = WSAGetLastError();
    ok(Result == 0, "zero-byte WSARecv returned %d, error %d\n", Result, Error);
    CheckCompletion(Port, Request, Result, Error, Bytes, 0, Skip, "zero-byte receive");

    Request = NewRequest();
    Buf.buf = Request->Data;
    Buf.len = sizeof(Request->Data);
    Flags = 0;
    Bytes = 0;
    Result = WSARecv(Server, &Buf, 1, &Bytes, &Flags, &Request->Ov, NULL);
    Error = WSAGetLastError();
    ok(Result == 0, "WSARecv returned %d, error %d\n", Result, Error);
    CheckCompletion(Port, Request, Result, Error, Bytes, 5, Skip, "receive");

    /* A send: completes at once or pends; only the pending case is queued with the flag */
    Request = NewRequest();
    Buf.buf = "0123456789";
    Buf.len = 10;
    Bytes = 0;
    Result = WSASend(Server, &Buf, 1, &Bytes, 0, &Request->Ov, NULL);
    Error = WSAGetLastError();
    CheckCompletion(Port, Request, Result, Error, Bytes, 10, Skip, "send");
    if (WaitReadable(Client, 10))
    {
        Result = recv(Client, Data, sizeof(Data), 0);
        ok(Result == 10, "recv returned %d\n", Result);
    }
    else
    {
        ok(FALSE, "sent data did not arrive\n");
    }

    /* No data: the receive pends and its completion is queued */
    Request = NewRequest();
    Buf.buf = Request->Data;
    Buf.len = sizeof(Request->Data);
    Flags = 0;
    Result = WSARecv(Server, &Buf, 1, &Bytes, &Flags, &Request->Ov, NULL);
    Error = WSAGetLastError();
    ok(Result == SOCKET_ERROR && Error == WSA_IO_PENDING, "WSARecv returned %d, error %d\n",
       Result, Error);
    ok(send(Client, "abc", 3, 0) == 3, "send failed, error %d\n", WSAGetLastError());
    if (Result == SOCKET_ERROR && Error == WSA_IO_PENDING)
    {
        CheckCompletion(Port, Request, Result, Error, 0, 3, Skip, "pending receive");
        if (Skip)
        {
            ok(WaitForSingleObject((HANDLE)Server, 0) == WAIT_TIMEOUT,
               "socket handle signaled with FILE_SKIP_SET_EVENT_ON_HANDLE\n");
        }
    }

    /* Nothing left over */
    Ret = GetPacket(Port, 200, &Key, &Pov, &Bytes);
    ok(!Ret, "extra packet key %Ix ov %p\n", Key, Pov);

out:
    if (Server != INVALID_SOCKET) closesocket(Server);
    if (Client != INVALID_SOCKET) closesocket(Client);
    if (Port) CloseHandle(Port);
}

START_TEST(iocpskip)
{
    WSADATA WsaData;
    PFN_SetFileCompletionNotificationModes pSetModes;

    pSetModes = (PFN_SetFileCompletionNotificationModes)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetFileCompletionNotificationModes");
    if (!pSetModes)
    {
        skip("SetFileCompletionNotificationModes not exported\n");
        return;
    }
    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");
    Test_Socket(pSetModes, FALSE);
    Test_Socket(pSetModes, TRUE);
    WSACleanup();
}
