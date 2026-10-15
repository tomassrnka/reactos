/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that cancelled stream sends never duplicate, replay or corrupt the stream
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "ws2_32.h"

typedef BOOL (WINAPI *PCANCEL_IO_EX)(HANDLE, LPOVERLAPPED);
static PCANCEL_IO_EX pCancelIoEx;

static unsigned RandomState;
/* After one send that never completes, do not wait 30 s for each of the others */
static BOOL Stuck;

static
unsigned
Random(void)
{
    RandomState = RandomState * 1103515245u + 12345u;
    return (RandomState >> 8) & 0xFFFFFF;
}

/* Byte Offset of send number Index */
static
UCHAR
Pattern(unsigned Index, unsigned Offset)
{
    unsigned x = Index * 2654435761u + Offset * 40503u;
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

typedef struct _RECEIVER
{
    SOCKET Socket;
    PUCHAR Data;
    SIZE_T Length, Capacity;
    DWORD StartDelay;
    int Error;
    HANDLE Thread;
    HANDLE Go;
} RECEIVER;

static
DWORD
WINAPI
ReceiverThread(LPVOID Parameter)
{
    RECEIVER *Receiver = Parameter;
    unsigned Count = 0;
    int Got;

    WaitForSingleObject(Receiver->Go, INFINITE);
    Sleep(Receiver->StartDelay);
    for (;;)
    {
        if (Receiver->Capacity - Receiver->Length < 65536)
        {
            PUCHAR NewData = HeapAlloc(GetProcessHeap(), 0, Receiver->Capacity * 2 + 65536);
            if (!NewData)
            {
                Receiver->Error = -1;
                break;
            }
            if (Receiver->Data)
            {
                CopyMemory(NewData, Receiver->Data, Receiver->Length);
                HeapFree(GetProcessHeap(), 0, Receiver->Data);
            }
            Receiver->Data = NewData;
            Receiver->Capacity = Receiver->Capacity * 2 + 65536;
        }
        Got = recv(Receiver->Socket, (char *)Receiver->Data + Receiver->Length, 65536, 0);
        if (Got == 0)
            break;
        if (Got < 0)
        {
            Receiver->Error = WSAGetLastError();
            break;
        }
        Receiver->Length += Got;
        /* Short pauses keep the sender's queue busy */
        if ((++Count % 7) == 0)
            Sleep(Count % 3);
    }
    return 0;
}

typedef struct _SEND_RECORD
{
    WSAOVERLAPPED Overlapped;
    PUCHAR Data;
    DWORD Length;
    BOOL Pending;
    BOOL Cancelled;
    DWORD Error;
    DWORD Bytes;
    BOOL Done;
} SEND_RECORD;

static
void
FinishSend(SOCKET Socket, SEND_RECORD *Record)
{
    DWORD Flags = 0;

    if (Record->Done)
        return;
    if (WaitForSingleObject(Record->Overlapped.hEvent, Stuck ? 100 : 30000) != WAIT_OBJECT_0)
    {
        ok(0, "a send did not complete\n");
        Stuck = TRUE;
    }
    if (WSAGetOverlappedResult(Socket, &Record->Overlapped, &Record->Bytes, FALSE, &Flags))
        Record->Error = 0;
    else
        Record->Error = WSAGetLastError();
    Record->Done = TRUE;
    HeapFree(GetProcessHeap(), 0, Record->Data);
    Record->Data = NULL;
}

/* The receiver must see the first Bytes bytes of every send, in order, and nothing else */
static
void
CheckStream(SEND_RECORD *Records, int Count, const RECEIVER *Receiver, const char *Name)
{
    SIZE_T Offset = 0;
    int i, Cancelled = 0, Withdrawn = 0, Pending = 0, Delivered = 0;
    DWORD j, Expected;
    BOOL Bad = FALSE;

    for (i = 0; i < Count && !Bad; i++)
    {
        Pending += Records[i].Pending;
        if (Records[i].Error)
        {
            ok(Records[i].Error == WSA_OPERATION_ABORTED, "%s: send %d failed with %lu\n", Name, i, Records[i].Error);
            Cancelled++;
            Withdrawn += (Records[i].Bytes == 0);
        }
        ok(Records[i].Bytes <= Records[i].Length, "%s: send %d reported %lu bytes for %lu\n",
           Name, i, Records[i].Bytes, Records[i].Length);

        Expected = Records[i].Bytes;
        /* Windows reports 0 bytes for a cancelled send whose data it still delivers */
        if (Records[i].Error && Expected == 0 && Offset + Records[i].Length <= Receiver->Length)
        {
            for (j = 0; j < Records[i].Length; j++)
            {
                if (Receiver->Data[Offset + j] != Pattern(i, j))
                    break;
            }
            if (j == Records[i].Length)
            {
                Expected = Records[i].Length;
                Delivered++;
            }
        }

        for (j = 0; j < Expected && j < Records[i].Length; j++, Offset++)
        {
            if (Offset >= Receiver->Length)
            {
                ok(0, "%s: the stream ends at %lu, inside send %d (byte %lu of %lu expected)\n",
                   Name, (ULONG)Receiver->Length, i, j, Expected);
                Bad = TRUE;
                break;
            }
            if (Receiver->Data[Offset] != Pattern(i, j))
            {
                ok(0, "%s: stream byte %lu is 0x%02x, expected 0x%02x (send %d byte %lu, error %lu, %lu bytes reported)\n",
                   Name, (ULONG)Offset, Receiver->Data[Offset], Pattern(i, j), i, j, Records[i].Error, Records[i].Bytes);
                Bad = TRUE;
                break;
            }
        }
    }
    if (!Bad)
    {
        ok(Offset == Receiver->Length, "%s: %lu bytes arrived after the expected %lu\n",
           Name, (ULONG)(Receiver->Length - Offset), (ULONG)Offset);
    }
    trace("%s: %d sends, %d pending at issue, %d cancelled (%d reported 0 bytes, %d of those delivered anyway), %lu bytes expected, %lu received\n",
          Name, Count, Pending, Cancelled, Withdrawn, Delivered, (ULONG)Offset, (ULONG)Receiver->Length);
}

/* Count overlapped sends of random sizes up to MaxLength; about a quarter is cancelled at once */
static
void
StressRound(int Count, unsigned MaxLength, DWORD ReaderDelay, unsigned Seed, const char *Name)
{
    SOCKET Client, Server;
    RECEIVER Receiver;
    SEND_RECORD *Records;
    int i, Issued = 0;
    const int Window = 32;

    if (!OpenPair(&Client, &Server))
    {
        ok(0, "%s: connection setup failed: %d\n", Name, WSAGetLastError());
        return;
    }
    Records = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Count * sizeof(*Records));
    ZeroMemory(&Receiver, sizeof(Receiver));
    Receiver.Socket = Server;
    Receiver.StartDelay = ReaderDelay;
    Receiver.Go = CreateEventW(NULL, TRUE, FALSE, NULL);
    Receiver.Thread = CreateThread(NULL, 0, ReceiverThread, &Receiver, 0, NULL);
    RandomState = Seed;

    for (i = 0; i < Count; i++)
    {
        SEND_RECORD *Record = &Records[i];
        WSABUF Buf;
        DWORD Sent = 0, j;

        if (i >= Window)
            FinishSend(Client, &Records[i - Window]);

        Record->Length = 1 + Random() % MaxLength;
        Record->Data = HeapAlloc(GetProcessHeap(), 0, Record->Length);
        for (j = 0; j < Record->Length; j++)
            Record->Data[j] = Pattern(i, j);
        Record->Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        Buf.buf = (char *)Record->Data;
        Buf.len = Record->Length;
        if (WSASend(Client, &Buf, 1, &Sent, 0, &Record->Overlapped, NULL))
        {
            if (WSAGetLastError() != WSA_IO_PENDING)
            {
                ok(0, "%s: WSASend %d failed: %d\n", Name, i, WSAGetLastError());
                HeapFree(GetProcessHeap(), 0, Record->Data);
                Record->Data = NULL;
                CloseHandle(Record->Overlapped.hEvent);
                break;
            }
            Record->Pending = TRUE;
        }
        Issued = i + 1;
        if ((Random() % 4) == 0)
        {
            if (Random() % 2)
                Sleep(0);
            Record->Cancelled = pCancelIoEx((HANDLE)Client, &Record->Overlapped);
        }
        if (i == 0)
            SetEvent(Receiver.Go);
    }
    SetEvent(Receiver.Go);

    for (i = 0; i < Issued; i++)
        FinishSend(Client, &Records[i]);

    ok(shutdown(Client, SD_SEND) == 0, "%s: shutdown failed: %d\n", Name, WSAGetLastError());
    ok(WaitForSingleObject(Receiver.Thread, 60000) == WAIT_OBJECT_0, "%s: the receiver did not see the end of the stream\n", Name);
    ok(Receiver.Error == 0, "%s: the receiver failed: %d\n", Name, Receiver.Error);
    CheckStream(Records, Issued, &Receiver, Name);

    /* The connection still carries data both ways after the cancellations */
    {
        char Byte = 'x';
        ok(send(Server, &Byte, 1, 0) == 1, "%s: send back failed: %d\n", Name, WSAGetLastError());
        ok(recv(Client, &Byte, 1, 0) == 1, "%s: recv back failed: %d\n", Name, WSAGetLastError());
    }

    for (i = 0; i < Issued; i++)
        CloseHandle(Records[i].Overlapped.hEvent);
    HeapFree(GetProcessHeap(), 0, Records);
    if (Receiver.Data)
        HeapFree(GetProcessHeap(), 0, Receiver.Data);
    CloseHandle(Receiver.Thread);
    CloseHandle(Receiver.Go);
    closesocket(Client);
    closesocket(Server);
}

START_TEST(sendcancel)
{
    WSADATA WsaData;
    int Round;

    pCancelIoEx = (PCANCEL_IO_EX)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CancelIoEx");
    if (!pCancelIoEx)
    {
        skip("CancelIoEx is not available\n");
        return;
    }
    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");

    for (Round = 0; Round < 3; Round++)
    {
        StressRound(1500, 12000, 0, 1 + Round, "reader keeps up");
        StressRound(1500, 12000, 300, 11 + Round, "reader starts late");
        StressRound(1500, 600, 0, 21 + Round, "small sends");
    }

    WSACleanup();
}
