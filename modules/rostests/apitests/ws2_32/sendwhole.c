/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     A send of a blocking stream socket is sent whole: send() returns the whole length and an
 *              overlapped WSASend with a completion routine completes with it (OpenSSH's sshd-session
 *              treats a short completion as a broken assumption and loses the rest of the data)
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
static volatile LONG ReaderDone;

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
    static char Buffer[65536];
    int Count;

    Sleep(300);
    while ((Count = recv(s, Buffer, sizeof(Buffer), 0)) > 0)
        InterlockedExchangeAdd64((LONG64 *)&Received, Count);
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

START_TEST(sendwhole)
{
    WSADATA Data;
    SOCKET Listener, Client, Server;
    struct sockaddr_in Addr;
    int Length = sizeof(Addr), Sent;
    HANDLE Thread;
    char *Large;
    ULONG Start;

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");
    Listener = socket(AF_INET, SOCK_STREAM, 0);
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed\n");
    ok(getsockname(Listener, (struct sockaddr *)&Addr, &Length) == 0, "getsockname failed\n");
    ok(listen(Listener, 1) == 0, "listen failed\n");
    Client = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(connect(Client, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "connect failed: %d\n", WSAGetLastError());
    Server = accept(Listener, NULL, NULL);
    ok(Server != INVALID_SOCKET, "accept failed\n");

    Thread = CreateThread(NULL, 0, Reader, (LPVOID)(ULONG_PTR)Server, 0, NULL);

    OverlappedSend(Client, SMALL_LEN);
    OverlappedSend(Client, LARGE_LEN);
    OverlappedSend(Client, HUGE_LEN);

    /* The plain blocking send */
    Large = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, HUGE_LEN);
    Sent = send(Client, Large, LARGE_LEN, 0);
    ok(Sent == LARGE_LEN, "send(%d) returned %d\n", LARGE_LEN, Sent);
    Sent = send(Client, Large, HUGE_LEN, 0);
    ok(Sent == HUGE_LEN, "send(%d) returned %d\n", HUGE_LEN, Sent);
    HeapFree(GetProcessHeap(), 0, Large);

    shutdown(Client, SD_SEND);
    ok(WaitForSingleObject(Thread, 60000) == WAIT_OBJECT_0, "the reader did not finish\n");
    Start = SMALL_LEN + LARGE_LEN + HUGE_LEN + LARGE_LEN + HUGE_LEN;
    ok(Received == Start, "the reader got %I64d bytes, expected %lu\n", (LONG64)Received, Start);
    CloseHandle(Thread);
    closesocket(Client);
    closesocket(Server);
    closesocket(Listener);
    WSACleanup();
}
