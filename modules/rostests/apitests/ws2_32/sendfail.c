/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test send and receive buffer size changes on busy stream sockets
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "ws2_32.h"

#define CHUNK 1000

/* A chunk starts with its number, so a receiver can tell which sends reached it */
static
void
FillChunk(PUCHAR Chunk, unsigned Index)
{
    unsigned i, x;

    Chunk[0] = (UCHAR)Index;
    Chunk[1] = (UCHAR)(Index >> 8);
    Chunk[2] = (UCHAR)(Index >> 16);
    Chunk[3] = (UCHAR)(Index >> 24);
    for (i = 4; i < CHUNK; i++)
    {
        x = Index * 2654435761u + i * 40503u;
        Chunk[i] = (UCHAR)(x ^ (x >> 13) ^ (x >> 24));
    }
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

static
int
GetBuffer(SOCKET Socket, int Option)
{
    int Value = -1, Length = sizeof(Value);

    if (getsockopt(Socket, SOL_SOCKET, Option, (char *)&Value, &Length))
        return -WSAGetLastError();
    return Value;
}

/* Reads until Bytes are received or the socket stays silent for Idle ms; returns the count */
static
DWORD
ReadSome(SOCKET Socket, PUCHAR Data, DWORD Bytes, DWORD Idle)
{
    DWORD Got = 0;
    fd_set Set;
    struct timeval Timeout;
    int Result;

    while (Got < Bytes)
    {
        FD_ZERO(&Set);
        FD_SET(Socket, &Set);
        Timeout.tv_sec = Idle / 1000;
        Timeout.tv_usec = (Idle % 1000) * 1000;
        if (select(0, &Set, NULL, NULL, &Timeout) <= 0)
            break;
        Result = recv(Socket, (char *)Data + Got, min(65536, Bytes - Got), 0);
        if (Result <= 0)
            break;
        Got += Result;
    }
    return Got;
}

/*
 * The stream holds whole chunks with growing numbers. Returns the number of chunks and marks
 * each chunk that arrived in Seen.
 */
static
unsigned
CheckChunks(PUCHAR Data, DWORD Length, BOOL *Seen, unsigned Count, const char *Name)
{
    UCHAR Expected[CHUNK];
    unsigned Index, Last = 0, Chunks = 0;
    DWORD Offset;
    BOOL First = TRUE;

    ok(Length % CHUNK == 0, "%s: the stream ends inside a chunk (%lu bytes)\n", Name, Length);
    for (Offset = 0; Offset + CHUNK <= Length; Offset += CHUNK)
    {
        Index = Data[Offset] | (Data[Offset + 1] << 8) | (Data[Offset + 2] << 16) | ((unsigned)Data[Offset + 3] << 24);
        if (Index >= Count || (!First && Index <= Last))
        {
            ok(0, "%s: chunk at offset %lu has number %u after %u\n", Name, Offset, Index, Last);
            return Chunks;
        }
        FillChunk(Expected, Index);
        if (memcmp(Expected, Data + Offset, CHUNK))
        {
            ok(0, "%s: chunk %u is damaged\n", Name, Index);
            return Chunks;
        }
        Seen[Index] = TRUE;
        Last = Index;
        First = FALSE;
        Chunks++;
    }
    return Chunks;
}


typedef struct _STREAM
{
    SOCKET Client, Server;
    PUCHAR Data;
    DWORD Length;
    unsigned Chunks;
    volatile LONG Done;
    int SendError, ReceiveError;
} STREAM;

/* Sends numbered chunks, each in one blocking send */
static
DWORD
WINAPI
ChunkSender(LPVOID Parameter)
{
    STREAM *Stream = Parameter;
    UCHAR Chunk[CHUNK];
    unsigned i;

    for (i = 0; i < Stream->Chunks; i++)
    {
        FillChunk(Chunk, i);
        if (send(Stream->Client, (char *)Chunk, CHUNK, 0) != CHUNK)
        {
            Stream->SendError = WSAGetLastError();
            break;
        }
    }
    shutdown(Stream->Client, SD_SEND);
    InterlockedExchange(&Stream->Done, 1);
    return 0;
}

static
DWORD
WINAPI
StreamReceiver(LPVOID Parameter)
{
    STREAM *Stream = Parameter;
    int Got;

    while (Stream->Length < Stream->Chunks * CHUNK)
    {
        Got = recv(Stream->Server, (char *)Stream->Data + Stream->Length,
                   min(65536, Stream->Chunks * CHUNK - Stream->Length), 0);
        if (Got == 0)
            break;
        if (Got < 0)
        {
            Stream->ReceiveError = WSAGetLastError();
            break;
        }
        Stream->Length += Got;
    }
    return 0;
}

/* The send buffer size is changed again and again while a stream of sends goes out */
static
void
TestSendBuffer(void)
{
    static const int Sizes[] = { 3000, 20000, 8192, 40000, 1500, 12000, 5000, 60000 };
    STREAM Stream;
    HANDLE Sender, Receiver;
    BOOL *Seen;
    DWORD Sets = 0, Mismatches = 0, Failures = 0;
    int Size = 0, Result;
    unsigned Arrived;

    if (!OpenPair(&Stream.Client, &Stream.Server))
    {
        ok(0, "connection setup failed: %d\n", WSAGetLastError());
        return;
    }
    Stream.Chunks = 16000;
    Stream.Length = 0;
    Stream.Done = 0;
    Stream.SendError = Stream.ReceiveError = 0;
    Stream.Data = HeapAlloc(GetProcessHeap(), 0, Stream.Chunks * CHUNK);
    Seen = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Stream.Chunks * sizeof(BOOL));
    Receiver = CreateThread(NULL, 0, StreamReceiver, &Stream, 0, NULL);
    Sender = CreateThread(NULL, 0, ChunkSender, &Stream, 0, NULL);

    while (!Stream.Done)
    {
        Size = Sizes[Sets % (sizeof(Sizes) / sizeof(Sizes[0]))];
        Result = setsockopt(Stream.Client, SOL_SOCKET, SO_SNDBUF, (char *)&Size, sizeof(Size));
        if (Result != 0)
            Failures++;
        else if (GetBuffer(Stream.Client, SO_SNDBUF) != Size)
            Mismatches++;
        Sets++;
        Sleep(0);
    }
    ok(WaitForSingleObject(Sender, 30000) == WAIT_OBJECT_0, "the sender did not finish\n");
    ok(WaitForSingleObject(Receiver, 30000) == WAIT_OBJECT_0, "the receiver did not finish\n");
    trace("%lu size changes during %u sends\n", Sets, Stream.Chunks);
    ok(Failures == 0, "setsockopt failed %lu times\n", Failures);
    ok(Mismatches == 0, "SO_SNDBUF did not read back the size just set %lu of %lu times\n", Mismatches, Sets);
    ok(Stream.SendError == 0, "a send failed: %d\n", Stream.SendError);
    ok(Stream.ReceiveError == 0, "a receive failed: %d\n", Stream.ReceiveError);
    Arrived = CheckChunks(Stream.Data, Stream.Length, Seen, Stream.Chunks, "send buffer");
    ok(Arrived == Stream.Chunks, "%u of %u chunks arrived\n", Arrived, Stream.Chunks);
    /* The last size is in effect once the stream is idle */
    ok(GetBuffer(Stream.Client, SO_SNDBUF) == Size, "SO_SNDBUF is %d, expected %d\n", GetBuffer(Stream.Client, SO_SNDBUF), Size);

    CloseHandle(Sender);
    CloseHandle(Receiver);
    HeapFree(GetProcessHeap(), 0, Stream.Data);
    HeapFree(GetProcessHeap(), 0, Seen);
    closesocket(Stream.Client);
    closesocket(Stream.Server);
}

typedef struct _SENDER
{
    SOCKET Socket;
    PUCHAR Data;
    DWORD Length;
    DWORD Sent;
    int Error;
} SENDER;

/* Sends in pieces, whatever a send accepts */
static
DWORD
WINAPI
SenderThread(LPVOID Parameter)
{
    SENDER *Sender = Parameter;
    int Result;

    while (Sender->Sent < Sender->Length)
    {
        Result = send(Sender->Socket, (char *)Sender->Data + Sender->Sent, min(1500, Sender->Length - Sender->Sent), 0);
        if (Result <= 0)
        {
            Sender->Error = WSAGetLastError();
            break;
        }
        Sender->Sent += Result;
    }
    return 0;
}

/* The receive buffer size is changed again and again while the data flows in */
static
void
TestReceiveBuffer(void)
{
    SOCKET Client, Server;
    SENDER Sender;
    HANDLE Thread;
    PUCHAR Data;
    BOOL *Seen;
    DWORD Total = 200 * CHUNK, Got = 0, Round, Read;
    UCHAR Chunk[CHUNK];
    int Size, Result;
    unsigned i, Chunks;

    if (!OpenPair(&Client, &Server))
    {
        ok(0, "connection setup failed: %d\n", WSAGetLastError());
        return;
    }
    Sender.Socket = Client;
    Sender.Data = HeapAlloc(GetProcessHeap(), 0, Total);
    Sender.Length = Total;
    Sender.Sent = 0;
    Sender.Error = 0;
    for (i = 0; i < 200; i++)
    {
        FillChunk(Chunk, i);
        CopyMemory(Sender.Data + i * CHUNK, Chunk, CHUNK);
    }
    Data = HeapAlloc(GetProcessHeap(), 0, Total);
    Seen = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 200 * sizeof(BOOL));
    Thread = CreateThread(NULL, 0, SenderThread, &Sender, 0, NULL);

    for (Round = 0; Got < Total; Round++)
    {
        Size = (Round & 1) ? 1024 : 6000;
        Result = setsockopt(Server, SOL_SOCKET, SO_RCVBUF, (char *)&Size, sizeof(Size));
        ok(Result == 0, "setsockopt failed: %d\n", WSAGetLastError());
        if (Round < 4)
            ok(GetBuffer(Server, SO_RCVBUF) == Size, "SO_RCVBUF is %d after setting %d\n", GetBuffer(Server, SO_RCVBUF), Size);
        Read = ReadSome(Server, Data + Got, min(1500, Total - Got), 3000);
        if (!Read)
            break;
        Got += Read;
    }
    ok(Got == Total, "received %lu of %lu bytes\n", Got, Total);
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "the sender did not finish\n");
    ok(Sender.Error == 0, "the sender failed: %d\n", Sender.Error);
    Chunks = CheckChunks(Data, Got, Seen, 200, "receive buffer");
    ok(Chunks == 200, "%u of 200 chunks arrived\n", Chunks);

    CloseHandle(Thread);
    HeapFree(GetProcessHeap(), 0, Sender.Data);
    HeapFree(GetProcessHeap(), 0, Data);
    HeapFree(GetProcessHeap(), 0, Seen);
    closesocket(Client);
    closesocket(Server);
}

static
void
Start(WSADATA *WsaData)
{
    ok(WSAStartup(MAKEWORD(2, 2), WsaData) == 0, "WSAStartup failed\n");
}

START_TEST(sndbuf)
{
    WSADATA WsaData;

    Start(&WsaData);
    TestSendBuffer();
    WSACleanup();
}

START_TEST(rcvbuf)
{
    WSADATA WsaData;

    Start(&WsaData);
    TestReceiveBuffer();
    WSACleanup();
}
