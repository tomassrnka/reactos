/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that cancelled overlapped UDP receives release their buffers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "ws2_32.h"

#define CANCEL_COUNT 1000
#define SystemPoolTagInformationClass 22

typedef LONG (WINAPI *PNT_QUERY_SYSTEM_INFORMATION)(ULONG, PVOID, ULONG, PULONG);

typedef struct _POOL_TAG_ENTRY
{
    ULONG TagUlong;
    ULONG PagedAllocs;
    ULONG PagedFrees;
    SIZE_T PagedUsed;
    ULONG NonPagedAllocs;
    ULONG NonPagedFrees;
    SIZE_T NonPagedUsed;
} POOL_TAG_ENTRY;

/* Pool bytes and outstanding allocations of every tag that starts with "Af" (the AFD driver's tags) */
static
BOOL
QueryAfdPool(SIZE_T *Bytes, LONG *Allocations)
{
    static PNT_QUERY_SYSTEM_INFORMATION pNtQuerySystemInformation;
    ULONG Size = 256 * 1024, Returned = 0, Count, i;
    PUCHAR Buffer;
    LONG Status;

    if (!pNtQuerySystemInformation)
        pNtQuerySystemInformation = (PNT_QUERY_SYSTEM_INFORMATION)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                                                 "NtQuerySystemInformation");
    if (!pNtQuerySystemInformation)
        return FALSE;

    for (;;)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Buffer)
            return FALSE;
        Status = pNtQuerySystemInformation(SystemPoolTagInformationClass, Buffer, Size, &Returned);
        if (Status != (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */ || Size >= 16 * 1024 * 1024)
            break;
        HeapFree(GetProcessHeap(), 0, Buffer);
        Size *= 2;
    }
    if (Status < 0)
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return FALSE;
    }

    Count = *(PULONG)Buffer;
    *Bytes = 0;
    *Allocations = 0;
    for (i = 0; i < Count; i++)
    {
        POOL_TAG_ENTRY *Entry = (POOL_TAG_ENTRY *)(Buffer + sizeof(SIZE_T) + i * sizeof(POOL_TAG_ENTRY));
        if ((Entry->TagUlong & 0xFFFF) == ('A' | ('f' << 8)))
        {
            *Bytes += Entry->PagedUsed + Entry->NonPagedUsed;
            *Allocations += (LONG)(Entry->PagedAllocs - Entry->PagedFrees) +
                            (LONG)(Entry->NonPagedAllocs - Entry->NonPagedFrees);
        }
    }
    HeapFree(GetProcessHeap(), 0, Buffer);
    return TRUE;
}

/* Post one overlapped WSARecvFrom with nothing to receive and cancel it; returns TRUE if it ended cancelled */
static
BOOL
CancelOneReceive(SOCKET Socket, HANDLE Event)
{
    WSAOVERLAPPED Overlapped;
    WSABUF Buf;
    char Data[1500];
    struct sockaddr_in From;
    int FromLength = sizeof(From);
    DWORD Received = 0, Flags = 0;
    int Result;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = Event;
    ResetEvent(Event);
    Buf.buf = Data;
    Buf.len = sizeof(Data);
    Result = WSARecvFrom(Socket, &Buf, 1, &Received, &Flags, (struct sockaddr *)&From, &FromLength, &Overlapped, NULL);
    if (Result == 0 || WSAGetLastError() != WSA_IO_PENDING)
        return FALSE;

    CancelIo((HANDLE)Socket);
    if (WaitForSingleObject(Event, 5000) != WAIT_OBJECT_0)
        return FALSE;
    return !WSAGetOverlappedResult(Socket, &Overlapped, &Received, FALSE, &Flags) &&
           WSAGetLastError() == WSA_OPERATION_ABORTED;
}

START_TEST(udpcancel)
{
    WSADATA WsaData;
    SOCKET Socket;
    struct sockaddr_in Address;
    HANDLE Event;
    SIZE_T BytesBefore, BytesAfter;
    LONG AllocationsBefore, AllocationsAfter;
    int i, Cancelled = 0;

    ok(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0, "WSAStartup failed\n");

    Socket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(Socket != INVALID_SOCKET, "WSASocket failed: %d\n", WSAGetLastError());
    if (Socket == INVALID_SOCKET)
        goto Done;

    ZeroMemory(&Address, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Socket, (struct sockaddr *)&Address, sizeof(Address)) == 0, "bind failed: %d\n", WSAGetLastError());

    Event = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* Warm up, so one-time allocations do not count */
    for (i = 0; i < 20; i++)
        CancelOneReceive(Socket, Event);
    Sleep(200);

    if (!QueryAfdPool(&BytesBefore, &AllocationsBefore))
    {
        skip("SystemPoolTagInformation is not available\n");
        CloseHandle(Event);
        closesocket(Socket);
        goto Done;
    }

    for (i = 0; i < CANCEL_COUNT; i++)
        Cancelled += CancelOneReceive(Socket, Event);
    Sleep(500);

    ok(QueryAfdPool(&BytesAfter, &AllocationsAfter), "SystemPoolTagInformation failed\n");
    ok(Cancelled == CANCEL_COUNT, "%d of %d receives ended cancelled\n", Cancelled, CANCEL_COUNT);
    /* A leak of one buffer array per receive is about 50 KB here */
    ok((LONG_PTR)(BytesAfter - BytesBefore) < 16 * 1024,
       "AFD pool grew by %ld bytes over %d cancelled receives (%lu -> %lu)\n",
       (long)(BytesAfter - BytesBefore), CANCEL_COUNT, (unsigned long)BytesBefore, (unsigned long)BytesAfter);
    ok(AllocationsAfter - AllocationsBefore < 100,
       "AFD pool has %ld more allocations after %d cancelled receives\n",
       (long)(AllocationsAfter - AllocationsBefore), CANCEL_COUNT);

    /* The socket still receives */
    {
        SOCKET Sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int Length = sizeof(Address);
        char Data[16];
        getsockname(Socket, (struct sockaddr *)&Address, &Length);
        ok(sendto(Sender, "datagram", 8, 0, (struct sockaddr *)&Address, sizeof(Address)) == 8,
           "sendto failed: %d\n", WSAGetLastError());
        Length = recv(Socket, Data, sizeof(Data), 0);
        ok(Length == 8, "recv returned %d, error %d\n", Length, WSAGetLastError());
        closesocket(Sender);
    }

    CloseHandle(Event);
    closesocket(Socket);

Done:
    WSACleanup();
}
