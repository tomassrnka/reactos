/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the WSARecvMsg extension function
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * WSARecvMsg is not exported by ws2_32: it is found with
 * SIO_GET_EXTENSION_FUNCTION_POINTER(WSAID_WSARECVMSG). Chrome's UDP sockets do that on every
 * read, and without it every read of a QUIC session fails with ERR_FAILED.
 * The expected values were measured on Windows 10 22H2 (x64).
 */

#include "ws2_32.h"
#include <mswsock.h>

static LPFN_WSARECVMSG pWSARecvMsg;

static
SOCKET
MakeBoundUdp(DWORD Flags, struct sockaddr_in *Address)
{
    SOCKET Sock = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, Flags);
    INT Length = sizeof(*Address);

    ok(Sock != INVALID_SOCKET, "socket failed, error %d\n", WSAGetLastError());
    if (Sock == INVALID_SOCKET)
        return Sock;

    ZeroMemory(Address, sizeof(*Address));
    Address->sin_family = AF_INET;
    Address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Sock, (SOCKADDR *)Address, sizeof(*Address)) == 0, "bind failed, error %d\n", WSAGetLastError());
    ok(getsockname(Sock, (SOCKADDR *)Address, &Length) == 0, "getsockname failed, error %d\n", WSAGetLastError());
    return Sock;
}

static
BOOL
WaitReadable(SOCKET Sock)
{
    fd_set Set;
    struct timeval Timeout = { 3, 0 };

    FD_ZERO(&Set);
    FD_SET(Sock, &Set);
    return select(0, &Set, NULL, NULL, &Timeout) == 1;
}

static
VOID
Send(SOCKET From, const struct sockaddr_in *To, const char *Data)
{
    INT Result = sendto(From, Data, (INT)strlen(Data), 0, (const SOCKADDR *)To, sizeof(*To));
    ok(Result == (INT)strlen(Data), "sendto returned %d, error %d\n", Result, WSAGetLastError());
}

static
BOOL
GetRecvMsg(SOCKET Sock, LPFN_WSARECVMSG *Function)
{
    GUID Guid = WSAID_WSARECVMSG;
    DWORD Bytes = 0;
    INT Result;

    *Function = NULL;
    Result = WSAIoctl(Sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid),
                      Function, sizeof(*Function), &Bytes, NULL, NULL);
    ok(Result == 0, "WSAIoctl(WSAID_WSARECVMSG) failed, error %d\n", WSAGetLastError());
    ok(Bytes == sizeof(*Function), "WSAIoctl returned %lu bytes\n", Bytes);
    ok(*Function != NULL, "no function returned\n");
    return Result == 0 && *Function != NULL;
}

START_TEST(WSARecvMsg)
{
    WSADATA wsaData;
    SOCKET Receiver, Sender, Tcp;
    struct sockaddr_in ReceiverAddress, SenderAddress, From;
    LPFN_WSARECVMSG pTcpRecvMsg;
    char Buffer[64], Control[64], Part1[3], Part2[16];
    WSABUF Buf, Parts[2];
    WSAMSG Msg;
    DWORD Bytes;
    INT Result;
    u_long NonBlocking;

    ok(WSAStartup(MAKEWORD(2, 2), &wsaData) == 0, "WSAStartup failed\n");

    Receiver = MakeBoundUdp(0, &ReceiverAddress);
    Sender = MakeBoundUdp(0, &SenderAddress);
    if (Receiver == INVALID_SOCKET || Sender == INVALID_SOCKET)
        goto Done;

    if (!GetRecvMsg(Receiver, &pWSARecvMsg))
        goto Done;

    /* A stream socket is handed the same function */
    Tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Tcp != INVALID_SOCKET, "tcp socket failed\n");
    if (GetRecvMsg(Tcp, &pTcpRecvMsg))
    {
        ZeroMemory(&Msg, sizeof(Msg));
        Buf.buf = Buffer;
        Buf.len = sizeof(Buffer);
        Msg.lpBuffers = &Buf;
        Msg.dwBufferCount = 1;
        Bytes = 77;
        SetLastError(0);
        Result = pTcpRecvMsg(Tcp, &Msg, &Bytes, NULL, NULL);
        ok(Result == SOCKET_ERROR, "unconnected stream socket: returned %d\n", Result);
        ok(WSAGetLastError() == WSAEINVAL, "unconnected stream socket: error %d\n", WSAGetLastError());
    }
    closesocket(Tcp);

    Buf.buf = Buffer;
    Buf.len = sizeof(Buffer);

    Bytes = 77;
    Result = pWSARecvMsg(Receiver, NULL, &Bytes, NULL, NULL);
    ok(Result == SOCKET_ERROR, "NULL message: returned %d\n", Result);
    ok(WSAGetLastError() == WSAEFAULT, "NULL message: error %d\n", WSAGetLastError());

    ZeroMemory(&Msg, sizeof(Msg));
    Msg.lpBuffers = &Buf;
    Msg.dwBufferCount = 1;
    Bytes = 77;
    Result = pWSARecvMsg((SOCKET)0x1234, &Msg, &Bytes, NULL, NULL);
    ok(Result == SOCKET_ERROR, "bad socket: returned %d\n", Result);
    ok(WSAGetLastError() == WSAENOTSOCK, "bad socket: error %d\n", WSAGetLastError());

    /* Nothing queued on a non-blocking socket */
    NonBlocking = 1;
    ok(ioctlsocket(Receiver, FIONBIO, &NonBlocking) == 0, "FIONBIO failed\n");
    ZeroMemory(&Msg, sizeof(Msg));
    Msg.name = (LPSOCKADDR)&From;
    Msg.namelen = sizeof(From);
    Msg.lpBuffers = &Buf;
    Msg.dwBufferCount = 1;
    Bytes = 77;
    Result = pWSARecvMsg(Receiver, &Msg, &Bytes, NULL, NULL);
    ok(Result == SOCKET_ERROR, "empty non-blocking: returned %d\n", Result);
    ok(WSAGetLastError() == WSAEWOULDBLOCK, "empty non-blocking: error %d\n", WSAGetLastError());
    NonBlocking = 0;
    ioctlsocket(Receiver, FIONBIO, &NonBlocking);

    /* A datagram with the sender's address, an empty control buffer and no flags */
    Send(Sender, &ReceiverAddress, "hello");
    ok(WaitReadable(Receiver), "datagram did not arrive\n");
    ZeroMemory(&Msg, sizeof(Msg));
    ZeroMemory(&From, sizeof(From));
    FillMemory(Control, sizeof(Control), 0xAA);
    Msg.name = (LPSOCKADDR)&From;
    Msg.namelen = sizeof(From);
    Msg.lpBuffers = &Buf;
    Msg.dwBufferCount = 1;
    Msg.Control.buf = Control;
    Msg.Control.len = sizeof(Control);
    Msg.dwFlags = 0;
    Bytes = 77;
    Result = pWSARecvMsg(Receiver, &Msg, &Bytes, NULL, NULL);
    ok(Result == 0, "receive: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 5, "receive: %lu bytes\n", Bytes);
    ok(!memcmp(Buffer, "hello", 5), "receive: wrong data\n");
    ok(Msg.namelen == sizeof(From), "receive: name length %d\n", Msg.namelen);
    ok(From.sin_family == AF_INET, "receive: family %d\n", From.sin_family);
    ok(From.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "receive: address %08lx\n", (ULONG)From.sin_addr.s_addr);
    ok(From.sin_port == SenderAddress.sin_port, "receive: port %d, expected %d\n", ntohs(From.sin_port), ntohs(SenderAddress.sin_port));
    ok(Msg.dwFlags == 0, "receive: flags %lx\n", Msg.dwFlags);
    ok(Msg.Control.len == 0, "receive: control length %lu\n", Msg.Control.len);

    /* No address buffer */
    Send(Sender, &ReceiverAddress, "abc");
    ok(WaitReadable(Receiver), "datagram did not arrive\n");
    ZeroMemory(&Msg, sizeof(Msg));
    Msg.lpBuffers = &Buf;
    Msg.dwBufferCount = 1;
    Bytes = 77;
    Result = pWSARecvMsg(Receiver, &Msg, &Bytes, NULL, NULL);
    ok(Result == 0, "no name: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 3, "no name: %lu bytes\n", Bytes);

    /* A datagram larger than the buffer: refused with WSAEMSGSIZE and flagged as truncated */
    Send(Sender, &ReceiverAddress, "0123456789");
    ok(WaitReadable(Receiver), "datagram did not arrive\n");
    {
        WSABUF Small;

        Small.buf = Buffer;
        Small.len = 4;
        ZeroMemory(&Msg, sizeof(Msg));
        Msg.name = (LPSOCKADDR)&From;
        Msg.namelen = sizeof(From);
        Msg.lpBuffers = &Small;
        Msg.dwBufferCount = 1;
        Bytes = 77;
        Result = pWSARecvMsg(Receiver, &Msg, &Bytes, NULL, NULL);
        ok(Result == SOCKET_ERROR, "truncated: returned %d\n", Result);
        ok(WSAGetLastError() == WSAEMSGSIZE, "truncated: error %d\n", WSAGetLastError());
        ok(Msg.dwFlags & MSG_TRUNC, "truncated: flags %lx\n", Msg.dwFlags);
        ok(!memcmp(Buffer, "0123", 4), "truncated: wrong data\n");
    }

    /* Several buffers */
    Send(Sender, &ReceiverAddress, "ABCDEFGH");
    ok(WaitReadable(Receiver), "datagram did not arrive\n");
    Parts[0].buf = Part1;
    Parts[0].len = sizeof(Part1);
    Parts[1].buf = Part2;
    Parts[1].len = sizeof(Part2);
    ZeroMemory(&Msg, sizeof(Msg));
    ZeroMemory(Part2, sizeof(Part2));
    Msg.name = (LPSOCKADDR)&From;
    Msg.namelen = sizeof(From);
    Msg.lpBuffers = Parts;
    Msg.dwBufferCount = 2;
    Bytes = 77;
    Result = pWSARecvMsg(Receiver, &Msg, &Bytes, NULL, NULL);
    ok(Result == 0, "two buffers: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 8, "two buffers: %lu bytes\n", Bytes);
    ok(!memcmp(Part1, "ABC", 3) && !memcmp(Part2, "DEFGH", 5), "two buffers: wrong data\n");

    /* An overlapped receive on an overlapped socket is pending, then completes with the datagram */
    closesocket(Receiver);
    Receiver = MakeBoundUdp(WSA_FLAG_OVERLAPPED, &ReceiverAddress);
    if (Receiver != INVALID_SOCKET)
    {
        WSAOVERLAPPED Overlapped;
        DWORD Wait, Transferred = 0, Flags = 0;

        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = WSACreateEvent();
        ZeroMemory(&Msg, sizeof(Msg));
        ZeroMemory(Buffer, sizeof(Buffer));
        Msg.name = (LPSOCKADDR)&From;
        Msg.namelen = sizeof(From);
        Msg.lpBuffers = &Buf;
        Msg.dwBufferCount = 1;
        Result = pWSARecvMsg(Receiver, &Msg, &Bytes, &Overlapped, NULL);
        ok(Result == SOCKET_ERROR, "overlapped: returned %d\n", Result);
        ok(WSAGetLastError() == WSA_IO_PENDING, "overlapped: error %d\n", WSAGetLastError());
        Send(Sender, &ReceiverAddress, "ovl!");
        Wait = WaitForSingleObject(Overlapped.hEvent, 5000);
        ok(Wait == WAIT_OBJECT_0, "overlapped: wait %lu\n", Wait);
        if (Wait == WAIT_OBJECT_0)
        {
            Result = WSAGetOverlappedResult(Receiver, &Overlapped, &Transferred, FALSE, &Flags);
            ok(Result, "overlapped: result failed, error %d\n", WSAGetLastError());
            ok(Transferred == 4, "overlapped: %lu bytes\n", Transferred);
            ok(!memcmp(Buffer, "ovl!", 4), "overlapped: wrong data\n");
            ok(Msg.namelen == sizeof(From), "overlapped: name length %d\n", Msg.namelen);
            ok(From.sin_port == SenderAddress.sin_port, "overlapped: port %d\n", ntohs(From.sin_port));
        }
        WSACloseEvent(Overlapped.hEvent);
    }

Done:
    if (Receiver != INVALID_SOCKET)
        closesocket(Receiver);
    if (Sender != INVALID_SOCKET)
        closesocket(Sender);
    WSACleanup();
}
