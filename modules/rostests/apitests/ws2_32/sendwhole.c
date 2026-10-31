/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     A send of a blocking stream socket is sent whole: send() returns the whole length and an
 *              overlapped WSASend completes with it, whether it completes through a completion routine,
 *              an event or a completion port (OpenSSH's sshd-session treats a short completion as a
 *              broken assumption and loses the rest of the data). An overlapped WSASend completes whole
 *              also on a socket in non-blocking mode (FIONBIO) and with SO_SNDBUF 0.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does.
 */

#include "ws2_32.h"

#define SMALL_LEN 4096
#define LARGE_LEN 100000
#define HUGE_LEN  1000000

static volatile LONG Completed;
static DWORD CompletedBytes, CompletedError;
static volatile LONG64 Received;
static volatile LONG64 Corrupt;
static volatile LONG ReaderDone;

/* The byte at a position of the stream: lets the reader check the order and the content */
#define STREAM_BYTE(Pos) ((BYTE)((Pos) ^ ((Pos) >> 8) ^ ((Pos) >> 17)))

static void CALLBACK SendDone(DWORD Error, DWORD Bytes, LPWSAOVERLAPPED Overlapped, DWORD Flags)
{
    CompletedError = Error;
    CompletedBytes = Bytes;
    Completed = 1;
}

/* Reads everything, slowly at first so that the sender has to wait for room */
static DWORD WINAPI Reader(LPVOID Param)
{
    SOCKET s = (SOCKET)(ULONG_PTR)Param;
    static BYTE Buffer[65536];
    int Count, i;
    LONG64 Pos;

    Sleep(300);
    while ((Count = recv(s, (char *)Buffer, sizeof(Buffer), 0)) > 0)
    {
        Pos = Received;
        for (i = 0; i < Count; i++)
        {
            if (Buffer[i] != STREAM_BYTE(Pos + i))
                InterlockedIncrement64((LONG64 *)&Corrupt);
        }
        InterlockedExchangeAdd64((LONG64 *)&Received, Count);
    }
    ReaderDone = 1;
    return 0;
}

static void OverlappedSend(SOCKET Client, ULONG Length)
{
    WSAOVERLAPPED Overlapped;
    WSABUF Buf;
    DWORD Sent = 0, Start;
    char *Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Length);
    int Result, Error;

    ok(Data != NULL, "out of memory\n");
    if (!Data)
        return;
    Buf.len = Length;
    Buf.buf = Data;
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Completed = 0;
    CompletedBytes = 0xdeadbeef;
    Result = WSASend(Client, &Buf, 1, &Sent, 0, &Overlapped, SendDone);
    Error = Result ? WSAGetLastError() : 0;
    ok(Result == 0 || Error == WSA_IO_PENDING, "WSASend(%lu) failed: %d\n", Length, Error);
    Start = GetTickCount();
    while (!Completed && GetTickCount() - Start < 30000)
        SleepEx(20, TRUE);
    ok(Completed, "WSASend(%lu) did not complete\n", Length);
    ok(CompletedError == 0, "WSASend(%lu) completed with error %lu\n", Length, CompletedError);
    ok(CompletedBytes == Length, "WSASend(%lu) completed with %lu bytes\n", Length, CompletedBytes);
    HeapFree(GetProcessHeap(), 0, Data);
}

/* Client: overlapped; SendBuf < 0 leaves SO_SNDBUF alone; NonBlocking puts the client in FIONBIO mode */
static void MakePairEx(SOCKET *Client, SOCKET *Server, int SendBuf, BOOL NonBlocking)
{
    SOCKET Listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in Addr;
    int Length = sizeof(Addr);

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed\n");
    ok(getsockname(Listener, (struct sockaddr *)&Addr, &Length) == 0, "getsockname failed\n");
    ok(listen(Listener, 1) == 0, "listen failed\n");
    *Client = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (SendBuf >= 0)
        ok(setsockopt(*Client, SOL_SOCKET, SO_SNDBUF, (char *)&SendBuf, sizeof(SendBuf)) == 0,
           "SO_SNDBUF %d failed: %d\n", SendBuf, WSAGetLastError());
    ok(connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "connect failed: %d\n", WSAGetLastError());
    *Server = accept(Listener, NULL, NULL);
    ok(*Server != INVALID_SOCKET, "accept failed\n");
    closesocket(Listener);
    if (NonBlocking)
    {
        u_long One = 1;
        ok(ioctlsocket(*Client, FIONBIO, &One) == 0, "FIONBIO failed: %d\n", WSAGetLastError());
    }
}

typedef struct _SEND
{
    WSAOVERLAPPED Overlapped;
    WSABUF Buf;
    ULONG Length;
} SEND;

/* Starts an overlapped send of the next Length bytes of the stream. Nothing is waited for */
static void StartSend(SOCKET Client, SEND *Send, ULONG Length, LONG64 *StreamPos, BOOL UseEvent)
{
    DWORD Sent = 0;
    ULONG i;
    int Result, Error;

    ZeroMemory(Send, sizeof(*Send));
    Send->Length = Length;
    Send->Buf.len = Length;
    Send->Buf.buf = HeapAlloc(GetProcessHeap(), 0, Length);
    ok(Send->Buf.buf != NULL, "out of memory\n");
    if (!Send->Buf.buf)
        return;
    for (i = 0; i < Length; i++)
        Send->Buf.buf[i] = STREAM_BYTE(*StreamPos + i);
    *StreamPos += Length;
    if (UseEvent)
    {
        Send->Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        ok(Send->Overlapped.hEvent != NULL, "CreateEvent failed\n");
    }
    Result = WSASend(Client, &Send->Buf, 1, &Sent, 0, &Send->Overlapped, NULL);
    Error = Result ? WSAGetLastError() : 0;
    ok(Result == 0 || Error == WSA_IO_PENDING, "WSASend(%lu) failed: %d\n", Length, Error);
    /* An inline completion has the whole length too */
    ok(Result != 0 || Sent == Length, "WSASend(%lu) returned %lu bytes\n", Length, Sent);
}

static void EventFinish(SOCKET Client, SEND *Send)
{
    DWORD Bytes = 0xdeadbeef, Flags = 0;
    DWORD Wait = WaitForSingleObject(Send->Overlapped.hEvent, 30000);
    BOOL Ok;

    ok(Wait == WAIT_OBJECT_0, "WSASend(%lu) did not signal its event: %lu\n", Send->Length, Wait);
    Ok = WSAGetOverlappedResult(Client, &Send->Overlapped, &Bytes, FALSE, &Flags);
    ok(Ok, "WSAGetOverlappedResult(%lu) failed: %d\n", Send->Length, WSAGetLastError());
    ok(Bytes == Send->Length, "WSASend(%lu) with an event completed with %lu bytes\n", Send->Length, Bytes);
    CloseHandle(Send->Overlapped.hEvent);
    HeapFree(GetProcessHeap(), 0, Send->Buf.buf);
}

static void IocpFinish(HANDLE Port, SEND *Send, SEND *Expected, ULONG_PTR ExpectedKey)
{
    DWORD Bytes = 0xdeadbeef;
    ULONG_PTR Key = 0;
    LPOVERLAPPED Completed;
    BOOL Ok = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Completed, 30000);

    ok(Ok, "no completion of the send of %lu bytes: %lu\n", Send->Length, GetLastError());
    ok(Completed == &Expected->Overlapped, "completion of %p, expected %p\n", Completed, &Expected->Overlapped);
    ok(Key == ExpectedKey, "completion key %Iu, expected %Iu\n", Key, ExpectedKey);
    ok(Bytes == Expected->Length, "WSASend(%lu) on a completion port completed with %lu bytes\n",
       Expected->Length, Bytes);
    HeapFree(GetProcessHeap(), 0, Send->Buf.buf);
}

/* Sends on a fresh connection, then checks the reader got everything in order */
static void StreamCase(BOOL UseIocp, BOOL NonBlocking, int SendBuf)
{
    SOCKET Client, Server;
    HANDLE Thread, Port = NULL;
    SEND Sends[4];
    LONG64 Pos = 0;
    ULONG Lengths[4] = { SMALL_LEN, LARGE_LEN, HUGE_LEN, HUGE_LEN };
    ULONG Total = 0;
    int i;

    Received = 0;
    Corrupt = 0;
    ReaderDone = 0;
    MakePairEx(&Client, &Server, SendBuf, NonBlocking);
    if (UseIocp)
    {
        Port = CreateIoCompletionPort((HANDLE)Client, NULL, 7, 0);
        ok(Port != NULL, "CreateIoCompletionPort failed: %lu\n", GetLastError());
        if (!Port)
        {
            closesocket(Client);
            closesocket(Server);
            return;
        }
    }
    Thread = CreateThread(NULL, 0, Reader, (LPVOID)(ULONG_PTR)Server, 0, NULL);

    /* One at a time, each larger than the send window of AFD except the first */
    for (i = 0; i < 3; i++)
    {
        StartSend(Client, &Sends[0], Lengths[i], &Pos, !UseIocp);
        if (UseIocp)
            IocpFinish(Port, &Sends[0], &Sends[0], 7);
        else
            EventFinish(Client, &Sends[0]);
        Total += Lengths[i];
    }

    /* Several at once: they complete in order, each with its whole length, and the stream keeps its order */
    for (i = 0; i < 4; i++)
        StartSend(Client, &Sends[i], Lengths[i], &Pos, !UseIocp);
    for (i = 0; i < 4; i++)
    {
        if (UseIocp)
            IocpFinish(Port, &Sends[i], &Sends[i], 7);
        else
            EventFinish(Client, &Sends[i]);
        Total += Lengths[i];
    }

    shutdown(Client, SD_SEND);
    ok(WaitForSingleObject(Thread, 60000) == WAIT_OBJECT_0, "the reader did not finish\n");
    ok(Received == Total, "the reader got %I64d bytes, expected %lu\n", (LONG64)Received, Total);
    ok(Corrupt == 0, "%I64d bytes of the stream are out of order or wrong\n", (LONG64)Corrupt);
    CloseHandle(Thread);
    if (Port)
        CloseHandle(Port);
    closesocket(Client);
    closesocket(Server);
}

/* A completion routine, and the plain blocking send unless the socket is in non-blocking mode */
static void RoutineCase(BOOL NonBlocking, int SendBuf)
{
    SOCKET Client, Server;
    int Sent;
    HANDLE Thread;
    char *Large;
    ULONG Total;

    Received = 0;
    Corrupt = 0;
    ReaderDone = 0;
    MakePairEx(&Client, &Server, SendBuf, NonBlocking);
    Thread = CreateThread(NULL, 0, Reader, (LPVOID)(ULONG_PTR)Server, 0, NULL);

    OverlappedSend(Client, SMALL_LEN);
    OverlappedSend(Client, LARGE_LEN);
    OverlappedSend(Client, HUGE_LEN);
    Total = SMALL_LEN + LARGE_LEN + HUGE_LEN;

    if (!NonBlocking)
    {
        Large = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, HUGE_LEN);
        Sent = send(Client, Large, LARGE_LEN, 0);
        ok(Sent == LARGE_LEN, "send(%d) returned %d\n", LARGE_LEN, Sent);
        Sent = send(Client, Large, HUGE_LEN, 0);
        ok(Sent == HUGE_LEN, "send(%d) returned %d\n", HUGE_LEN, Sent);
        HeapFree(GetProcessHeap(), 0, Large);
        Total += LARGE_LEN + HUGE_LEN;
    }

    shutdown(Client, SD_SEND);
    ok(WaitForSingleObject(Thread, 60000) == WAIT_OBJECT_0, "the reader did not finish\n");
    ok(Received == Total, "the reader got %I64d bytes, expected %lu\n", (LONG64)Received, Total);
    CloseHandle(Thread);
    closesocket(Client);
    closesocket(Server);
}

START_TEST(sendwhole)
{
    WSADATA Data;
    int NonBlocking, SendBuf;

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");

    /* Blocking and non-blocking sockets, the default send buffer and SO_SNDBUF 0 (nothing buffered
     * by the socket: the oracle completes at once, still with the whole length) */
    for (NonBlocking = 0; NonBlocking < 2; NonBlocking++)
    {
        for (SendBuf = -1; SendBuf <= 0; SendBuf++)
        {
            trace("non-blocking %d, SO_SNDBUF %d\n", NonBlocking, SendBuf);
            RoutineCase(NonBlocking, SendBuf);
            StreamCase(FALSE, NonBlocking, SendBuf);
            StreamCase(TRUE, NonBlocking, SendBuf);
        }
    }

    WSACleanup();
}
