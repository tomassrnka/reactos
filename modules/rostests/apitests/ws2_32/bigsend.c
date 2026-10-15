/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a large send on a blocking stream socket sends all its data
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "ws2_32.h"

#define BIG_SEND (1024 * 1024)

static
UCHAR
Pattern(unsigned Round, unsigned Offset)
{
    unsigned x = Round * 2654435761u + Offset * 40503u;
    return (UCHAR)(x ^ (x >> 13) ^ (x >> 24));
}

static
BOOL
OpenPair(SOCKET *Client, SOCKET *Server)
{
    SOCKET Listener;
    struct sockaddr_in Address;
    int Length = sizeof(Address);

    *Client = *Server = INVALID_SOCKET;
    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Listener == INVALID_SOCKET)
        return FALSE;
    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Listener, (struct sockaddr *)&Address, sizeof(Address)) ||
        listen(Listener, 1) ||
        getsockname(Listener, (struct sockaddr *)&Address, &Length))
    {
        closesocket(Listener);
        return FALSE;
    }
    *Client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (*Client == INVALID_SOCKET || connect(*Client, (struct sockaddr *)&Address, sizeof(Address)))
    {
        closesocket(Listener);
        if (*Client != INVALID_SOCKET)
            closesocket(*Client);
        *Client = INVALID_SOCKET;
        return FALSE;
    }
    *Server = accept(Listener, NULL, NULL);
    closesocket(Listener);
    if (*Server == INVALID_SOCKET)
    {
        closesocket(*Client);
        *Client = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

typedef struct _READER
{
    SOCKET Socket;
    PUCHAR Data;
    DWORD Length;
    DWORD Delay;
    int Error;
} READER;

/* Starts reading after Delay ms and reads until the peer shuts down its side */
static
DWORD
WINAPI
ReaderThread(LPVOID Parameter)
{
    READER *Reader = Parameter;
    int Got;

    Sleep(Reader->Delay);
    for (;;)
    {
        Got = recv(Reader->Socket, (char *)Reader->Data + Reader->Length,
                   min(65536, BIG_SEND + 65536 - Reader->Length), 0);
        if (Got == 0)
            break;
        if (Got < 0)
        {
            Reader->Error = WSAGetLastError();
            break;
        }
        Reader->Length += Got;
        if (Reader->Length >= BIG_SEND + 65536)
            break;
    }
    return 0;
}

/* Mode 0: send(), mode 1: WSASend without overlapped, mode 2: overlapped WSASend */
static
void
TestBigSend(int Mode)
{
    static const char *Names[] = { "send", "WSASend", "overlapped WSASend" };
    SOCKET Client, Server;
    READER Reader;
    HANDLE Thread;
    PUCHAR Buffer;
    DWORD i, Sent = 0, Flags = 0;
    WSABUF Buf;
    WSAOVERLAPPED Overlapped;
    int Result, Error = 0;

    if (!OpenPair(&Client, &Server))
    {
        ok(0, "%s: connection setup failed: %d\n", Names[Mode], WSAGetLastError());
        return;
    }

    Buffer = HeapAlloc(GetProcessHeap(), 0, BIG_SEND);
    ZeroMemory(&Reader, sizeof(Reader));
    Reader.Socket = Server;
    Reader.Data = HeapAlloc(GetProcessHeap(), 0, BIG_SEND + 65536);
    Reader.Delay = 1000;
    for (i = 0; i < BIG_SEND; i++)
        Buffer[i] = Pattern(Mode, i);
    Thread = CreateThread(NULL, 0, ReaderThread, &Reader, 0, NULL);

    Buf.buf = (char *)Buffer;
    Buf.len = BIG_SEND;
    if (Mode == 0)
    {
        Result = send(Client, (char *)Buffer, BIG_SEND, 0);
        if (Result < 0)
            Error = WSAGetLastError();
        else
            Sent = Result;
        ok(Result == BIG_SEND, "send returned %d, error %d\n", Result, Error);
    }
    else if (Mode == 1)
    {
        Result = WSASend(Client, &Buf, 1, &Sent, 0, NULL, NULL);
        ok(Result == 0, "WSASend failed: %d\n", WSAGetLastError());
        ok(Sent == BIG_SEND, "WSASend sent %lu bytes\n", Sent);
    }
    else
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        Result = WSASend(Client, &Buf, 1, &Sent, 0, &Overlapped, NULL);
        ok(Result == 0 || WSAGetLastError() == WSA_IO_PENDING, "WSASend failed: %d\n", WSAGetLastError());
        ok(WaitForSingleObject(Overlapped.hEvent, 20000) == WAIT_OBJECT_0, "overlapped WSASend did not complete\n");
        ok(WSAGetOverlappedResult(Client, &Overlapped, &Sent, FALSE, &Flags), "overlapped WSASend failed: %d\n", WSAGetLastError());
        ok(Sent == BIG_SEND, "overlapped WSASend sent %lu bytes\n", Sent);
        CloseHandle(Overlapped.hEvent);
    }

    ok(shutdown(Client, SD_SEND) == 0, "%s: shutdown failed: %d\n", Names[Mode], WSAGetLastError());
    ok(WaitForSingleObject(Thread, 20000) == WAIT_OBJECT_0, "%s: the reader did not finish\n", Names[Mode]);
    ok(Reader.Error == 0, "%s: the reader failed: %d\n", Names[Mode], Reader.Error);
    ok(Reader.Length == Sent, "%s: the peer received %lu bytes, %lu sent\n", Names[Mode], Reader.Length, Sent);
    for (i = 0; i < Reader.Length && i < Sent; i++)
    {
        if (Reader.Data[i] != Pattern(Mode, i))
        {
            ok(0, "%s: byte %lu is 0x%02x, expected 0x%02x\n", Names[Mode], i, Reader.Data[i], Pattern(Mode, i));
            break;
        }
    }

    CloseHandle(Thread);
    HeapFree(GetProcessHeap(), 0, Reader.Data);
    HeapFree(GetProcessHeap(), 0, Buffer);
    closesocket(Client);
    closesocket(Server);
}

START_TEST(bigsend)
{
    WSADATA WsaData;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");
    TestBigSend(0);
    TestBigSend(1);
    TestBigSend(2);
    WSACleanup();
}
