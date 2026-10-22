/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for WSARecvFrom with several data buffers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

#define GUARD_BYTE 0xCC
#define GUARD_SIZE 4

static SOCKET RecvSock = INVALID_SOCKET;
static SOCKET SendSock = INVALID_SOCKET;
static SOCKADDR_IN RecvAddr;
static SOCKADDR_IN SendAddr;

/* The address buffer is larger than a SOCKADDR_IN, so the returned
 * length must shrink to show that it was written */
typedef struct _FROM_BUFFER
{
    UCHAR Lead[8];
    SOCKADDR_IN Addr;
    UCHAR Spare[8];
    UCHAR Guard[8];
} FROM_BUFFER;

#define FROM_CAPACITY (sizeof(SOCKADDR_IN) + 8)

/* Lays out Count buffers of Size bytes, each followed by a guard */
static
VOID
SetupBuffers(
    _Out_writes_bytes_(MemSize) PUCHAR Mem,
    _In_ SIZE_T MemSize,
    _Out_writes_(Count) WSABUF *Buffers,
    _In_ ULONG Count,
    _In_ ULONG Size)
{
    ULONG i;

    memset(Mem, GUARD_BYTE, MemSize);
    for (i = 0; i < Count; i++)
    {
        Buffers[i].buf = (PCHAR)Mem + GUARD_SIZE + i * (Size + GUARD_SIZE);
        Buffers[i].len = Size;
    }
}

/* Checks that the buffers hold Data in order and every other byte is a guard */
static
VOID
CheckBuffers(
    _In_ const UCHAR *Mem,
    _In_ SIZE_T MemSize,
    _In_ const WSABUF *Buffers,
    _In_ ULONG Count,
    _In_ const CHAR *Data,
    _In_ ULONG DataLen,
    _In_ PCSTR Label)
{
    ULONG Index, i, Expected;
    SIZE_T Offset;
    ULONG Copied = 0;
    UCHAR Want[64];

    ok(MemSize <= sizeof(Want), "%s: buffer layout too large\n", Label);
    if (MemSize > sizeof(Want))
        return;

    memset(Want, GUARD_BYTE, MemSize);
    for (Index = 0; Index < Count && Copied < DataLen; Index++)
    {
        Offset = (PUCHAR)Buffers[Index].buf - Mem;
        Expected = DataLen - Copied;
        if (Expected > Buffers[Index].len)
            Expected = Buffers[Index].len;
        memcpy(Want + Offset, Data + Copied, Expected);
        Copied += Expected;
    }

    for (i = 0; i < MemSize; i++)
    {
        ok(Mem[i] == Want[i], "%s: byte %lu is 0x%02x, expected 0x%02x\n",
           Label, i, Mem[i], Want[i]);
    }
}

static
VOID
CheckFrom(
    _In_ const FROM_BUFFER *From,
    _In_ INT FromLen,
    _In_ PCSTR Label)
{
    ULONG i;

    ok(FromLen == sizeof(SOCKADDR_IN), "%s: FromLen = %d\n", Label, FromLen);
    ok(From->Addr.sin_family == AF_INET, "%s: sin_family = %u\n", Label, (USHORT)From->Addr.sin_family);
    ok(From->Addr.sin_port == SendAddr.sin_port, "%s: sin_port = %u, expected %u\n",
       Label, ntohs(From->Addr.sin_port), ntohs(SendAddr.sin_port));
    ok(From->Addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "%s: sin_addr = 0x%08lx\n",
       Label, ntohl(From->Addr.sin_addr.s_addr));
    for (i = 0; i < sizeof(From->Lead); i++)
    {
        ok(From->Lead[i] == GUARD_BYTE, "%s: leading address guard byte %lu is 0x%02x\n",
           Label, i, From->Lead[i]);
    }
    for (i = 0; i < sizeof(From->Guard); i++)
    {
        ok(From->Guard[i] == GUARD_BYTE, "%s: trailing address guard byte %lu is 0x%02x\n",
           Label, i, From->Guard[i]);
    }
}

static
BOOL
SendAndWait(
    _In_ const CHAR *Data,
    _In_ INT DataLen)
{
    fd_set Fds;
    struct timeval Timeout = { 5, 0 };
    INT Result;

    Result = sendto(SendSock, Data, DataLen, 0, (SOCKADDR *)&RecvAddr, sizeof(RecvAddr));
    ok(Result == DataLen, "sendto returned %d, error %d\n", Result, WSAGetLastError());
    if (Result != DataLen)
        return FALSE;

    FD_ZERO(&Fds);
    FD_SET(RecvSock, &Fds);
    Result = select(0, &Fds, NULL, NULL, &Timeout);
    ok(Result == 1, "select returned %d, error %d\n", Result, WSAGetLastError());
    return (Result == 1);
}

static
VOID
TestRecvFrom(
    _In_ ULONG Count,
    _In_ ULONG Size,
    _In_ BOOL WithFrom,
    _In_ PCSTR Label)
{
    UCHAR Mem[GUARD_SIZE + 3 * (8 + GUARD_SIZE)];
    WSABUF Buffers[3];
    FROM_BUFFER From;
    INT FromLen = FROM_CAPACITY;
    DWORD Received = 0xdeadbeef, Flags = 0;
    SIZE_T MemSize = GUARD_SIZE + Count * (Size + GUARD_SIZE);
    INT Result;

    ok(Count <= 3 && Size <= 8, "%s: bad layout\n", Label);
    if (Count > 3 || Size > 8)
        return;
    SetupBuffers(Mem, MemSize, Buffers, Count, Size);
    memset(&From, GUARD_BYTE, sizeof(From));

    if (!SendAndWait("abcdef", 6))
        return;

    Result = WSARecvFrom(RecvSock, Buffers, Count, &Received, &Flags,
                         WithFrom ? (SOCKADDR *)&From.Addr : NULL,
                         WithFrom ? &FromLen : NULL, NULL, NULL);
    ok(Result == 0, "%s: WSARecvFrom returned %d, error %d\n", Label, Result, WSAGetLastError());
    ok(Received == 6, "%s: Received = %lu\n", Label, Received);
    ok(Flags == 0, "%s: Flags = 0x%lx\n", Label, Flags);
    CheckBuffers(Mem, MemSize, Buffers, Count, "abcdef", 6, Label);
    if (WithFrom)
        CheckFrom(&From, FromLen, Label);
}

static
VOID
TestRecvFromTruncated(VOID)
{
    UCHAR Mem[GUARD_SIZE + 2 * (2 + GUARD_SIZE)];
    WSABUF Buffers[2];
    FROM_BUFFER From;
    INT FromLen = FROM_CAPACITY;
    DWORD Received, Flags = 0;
    INT Result;

    SetupBuffers(Mem, sizeof(Mem), Buffers, 2, 2);
    memset(&From, GUARD_BYTE, sizeof(From));

    if (!SendAndWait("abcdef", 6))
        return;

    Result = WSARecvFrom(RecvSock, Buffers, 2, &Received, &Flags,
                         (SOCKADDR *)&From.Addr, &FromLen, NULL, NULL);
    ok(Result == SOCKET_ERROR, "truncated: WSARecvFrom returned %d\n", Result);
    ok(WSAGetLastError() == WSAEMSGSIZE, "truncated: error %d\n", WSAGetLastError());
    CheckBuffers(Mem, sizeof(Mem), Buffers, 2, "abcdef", 4, "truncated");
}

/* WSARecv on a datagram socket takes the plain receive path */
static
VOID
TestRecvMultiBuffer(VOID)
{
    UCHAR Mem[GUARD_SIZE + 2 * (3 + GUARD_SIZE)];
    WSABUF Buffers[2];
    DWORD Received = 0xdeadbeef, Flags = 0;
    INT Result;

    SetupBuffers(Mem, sizeof(Mem), Buffers, 2, 3);

    if (!SendAndWait("abcdef", 6))
        return;

    Result = WSARecv(RecvSock, Buffers, 2, &Received, &Flags, NULL, NULL);
    ok(Result == 0, "WSARecv: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Received == 6, "WSARecv: Received = %lu\n", Received);
    CheckBuffers(Mem, sizeof(Mem), Buffers, 2, "abcdef", 6, "WSARecv");
}

/* The receive is posted first, so the datagram completes a pending request */
static
VOID
TestRecvFromPending(VOID)
{
    /* Static, so a request that outlives a failed close cannot reach the stack */
    static UCHAR Mem[GUARD_SIZE + 2 * (3 + GUARD_SIZE)];
    static WSABUF Buffers[2];
    static FROM_BUFFER From;
    static INT FromLen;
    static WSAOVERLAPPED Overlapped;
    DWORD Received = 0, Flags = 0, Wait;
    INT Result;
    BOOL Success;

    FromLen = FROM_CAPACITY;
    SetupBuffers(Mem, sizeof(Mem), Buffers, 2, 3);
    memset(&From, GUARD_BYTE, sizeof(From));
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = WSACreateEvent();
    ok(Overlapped.hEvent != WSA_INVALID_EVENT, "WSACreateEvent failed\n");
    if (Overlapped.hEvent == WSA_INVALID_EVENT)
        return;

    Result = WSARecvFrom(RecvSock, Buffers, 2, NULL, &Flags,
                         (SOCKADDR *)&From.Addr, &FromLen, &Overlapped, NULL);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING,
       "pending: WSARecvFrom returned %d, error %d\n", Result, WSAGetLastError());
    if (Result != SOCKET_ERROR || WSAGetLastError() != WSA_IO_PENDING)
    {
        WSACloseEvent(Overlapped.hEvent);
        return;
    }

    Result = sendto(SendSock, "abcdef", 6, 0, (SOCKADDR *)&RecvAddr, sizeof(RecvAddr));
    ok(Result == 6, "sendto returned %d, error %d\n", Result, WSAGetLastError());

    Wait = WaitForSingleObject(Overlapped.hEvent, 5000);
    ok(Wait == WAIT_OBJECT_0, "pending: wait returned %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        /* Closing the socket cancels the request; keep the event if it fails */
        if (closesocket(RecvSock) == 0)
        {
            RecvSock = INVALID_SOCKET;
            WSACloseEvent(Overlapped.hEvent);
        }
        return;
    }

    Success = WSAGetOverlappedResult(RecvSock, &Overlapped, &Received, FALSE, &Flags);
    ok(Success, "pending: WSAGetOverlappedResult failed, error %d\n", WSAGetLastError());
    ok(Received == 6, "pending: Received = %lu\n", Received);
    CheckBuffers(Mem, sizeof(Mem), Buffers, 2, "abcdef", 6, "pending");
    CheckFrom(&From, FromLen, "pending");

    WSACloseEvent(Overlapped.hEvent);
}

START_TEST(WSARecvFrom)
{
    WSADATA WsaData;
    INT AddrLen;
    INT Result;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    RecvSock = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    SendSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(RecvSock != INVALID_SOCKET && SendSock != INVALID_SOCKET,
       "socket failed, error %d\n", WSAGetLastError());
    if (RecvSock == INVALID_SOCKET || SendSock == INVALID_SOCKET)
        goto Cleanup;

    ZeroMemory(&RecvAddr, sizeof(RecvAddr));
    RecvAddr.sin_family = AF_INET;
    RecvAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SendAddr = RecvAddr;

    Result = bind(RecvSock, (SOCKADDR *)&RecvAddr, sizeof(RecvAddr));
    ok(Result == 0, "bind failed, error %d\n", WSAGetLastError());
    Result = bind(SendSock, (SOCKADDR *)&SendAddr, sizeof(SendAddr));
    ok(Result == 0, "bind failed, error %d\n", WSAGetLastError());

    AddrLen = sizeof(RecvAddr);
    Result = getsockname(RecvSock, (SOCKADDR *)&RecvAddr, &AddrLen);
    ok(Result == 0, "getsockname failed, error %d\n", WSAGetLastError());
    AddrLen = sizeof(SendAddr);
    Result = getsockname(SendSock, (SOCKADDR *)&SendAddr, &AddrLen);
    ok(Result == 0, "getsockname failed, error %d\n", WSAGetLastError());

    TestRecvFrom(1, 8, TRUE, "1x8");
    TestRecvFrom(2, 3, TRUE, "2x3");
    TestRecvFrom(3, 4, TRUE, "3x4");
    TestRecvFrom(2, 3, FALSE, "2x3 no address");
    TestRecvFromTruncated();
    TestRecvMultiBuffer();
    TestRecvFromPending();

Cleanup:
    if (RecvSock != INVALID_SOCKET)
        closesocket(RecvSock);
    if (SendSock != INVALID_SOCKET)
        closesocket(SendSock);
    WSACleanup();
}
