/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Zero-length UDP datagrams are delivered, and getsockname after a UDP connect
 *              reports the address of the interface that reaches the peer
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <mswsock.h>
#include <iphlpapi.h>

/* The expected values were recorded on Windows 10 22H2. */

static
BOOL
OpenReceiver(SOCKET *Receiver, struct sockaddr_in *To, ULONG Address)
{
    int Length = sizeof(*To);

    *Receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(*Receiver != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (*Receiver == INVALID_SOCKET)
        return FALSE;

    ZeroMemory(To, sizeof(*To));
    To->sin_family = AF_INET;
    if (bind(*Receiver, (struct sockaddr *)To, sizeof(*To)) == SOCKET_ERROR ||
        getsockname(*Receiver, (struct sockaddr *)To, &Length) == SOCKET_ERROR)
    {
        ok(0, "bind/getsockname failed: %d\n", WSAGetLastError());
        closesocket(*Receiver);
        return FALSE;
    }
    To->sin_addr.s_addr = Address;
    return TRUE;
}

static
int
Readable(SOCKET Socket, DWORD TimeoutMs)
{
    fd_set ReadSet;
    struct timeval Timeout;

    FD_ZERO(&ReadSet);
    FD_SET(Socket, &ReadSet);
    Timeout.tv_sec = TimeoutMs / 1000;
    Timeout.tv_usec = (TimeoutMs % 1000) * 1000;
    return select(0, &ReadSet, NULL, NULL, &Timeout);
}

#define MODE_RECV       0
#define MODE_RECVFROM   1
#define MODE_WSARECVFROM 2
#define MODE_WSARECVMSG 3
#define MODE_PEEK       4
#define MODE_OVERLAPPED 5

static LPFN_WSARECVMSG pWSARecvMsg;

/* Send a datagram of SendLength bytes and receive it the way Mode says. The
 * receive must return Expected bytes from the sender (when the call reports
 * the address) with no error. */
static
void
CheckReceive(const char *Name, ULONG Address, int Mode, int SendLength)
{
    SOCKET Receiver, Sender;
    struct sockaddr_in To, SenderName, From;
    char Buffer[32];
    WSABUF Wsabuf;
    DWORD Bytes = 77, Flags = 0;
    int Length, FromLength = sizeof(From), Result, Error = 0, Selected, ReportsAddress = 1;
    WSAOVERLAPPED Overlapped;

    if (!OpenReceiver(&Receiver, &To, Address))
        return;
    Sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(Sender != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Sender == INVALID_SOCKET)
    {
        closesocket(Receiver);
        return;
    }
    /* The datagram's source port names the sender */
    ZeroMemory(&SenderName, sizeof(SenderName));
    SenderName.sin_family = AF_INET;
    bind(Sender, (struct sockaddr *)&SenderName, sizeof(SenderName));
    Length = sizeof(SenderName);
    getsockname(Sender, (struct sockaddr *)&SenderName, &Length);

    FillMemory(Buffer, sizeof(Buffer), 0x55);
    Wsabuf.buf = Buffer;
    Wsabuf.len = sizeof(Buffer);
    ZeroMemory(&From, sizeof(From));

    if (Mode == MODE_OVERLAPPED)
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = WSACreateEvent();
        Result = WSARecvFrom(Receiver, &Wsabuf, 1, &Bytes, &Flags,
                             (struct sockaddr *)&From, &FromLength, &Overlapped, NULL);
        Error = Result ? WSAGetLastError() : 0;
        ok(Result == SOCKET_ERROR && Error == WSA_IO_PENDING, "%s: posted receive: %d, %d\n", Name, Result, Error);
    }

    Result = sendto(Sender, Buffer, SendLength, 0, (struct sockaddr *)&To, sizeof(To));
    ok(Result == SendLength, "%s: sendto returned %d, error %d\n", Name, Result, WSAGetLastError());

    if (Mode == MODE_OVERLAPPED)
    {
        BOOL Ok;
        DWORD Wait = WaitForSingleObject(Overlapped.hEvent, 2000);

        ok(Wait == WAIT_OBJECT_0, "%s: wait %lu\n", Name, Wait);
        Bytes = 77;
        Flags = 0xdead;
        Ok = Wait == WAIT_OBJECT_0 && WSAGetOverlappedResult(Receiver, &Overlapped, &Bytes, FALSE, &Flags);
        ok(Ok, "%s: result failed: %d\n", Name, WSAGetLastError());
        ok(Bytes == (DWORD)SendLength, "%s: %lu bytes, expected %d\n", Name, Bytes, SendLength);
        WSACloseEvent(Overlapped.hEvent);
        goto Check;
    }

    Selected = Readable(Receiver, 2000);
    ok(Selected == 1, "%s: select returned %d, the datagram is not readable\n", Name, Selected);
    if (Selected != 1)
        goto Done;

    switch (Mode)
    {
        case MODE_RECV:
        case MODE_PEEK:
            ReportsAddress = 0;
            Result = recv(Receiver, Buffer, sizeof(Buffer), Mode == MODE_PEEK ? MSG_PEEK : 0);
            ok(Result == SendLength, "%s: recv returned %d, error %d, expected %d\n",
               Name, Result, WSAGetLastError(), SendLength);
            if (Mode == MODE_PEEK)
            {
                /* The peeked datagram stays queued */
                Result = recv(Receiver, Buffer, sizeof(Buffer), 0);
                ok(Result == SendLength, "%s: recv after peek returned %d, error %d\n",
                   Name, Result, WSAGetLastError());
            }
            break;
        case MODE_RECVFROM:
            Result = recvfrom(Receiver, Buffer, sizeof(Buffer), 0, (struct sockaddr *)&From, &FromLength);
            ok(Result == SendLength, "%s: recvfrom returned %d, error %d, expected %d\n",
               Name, Result, WSAGetLastError(), SendLength);
            break;
        case MODE_WSARECVFROM:
            Result = WSARecvFrom(Receiver, &Wsabuf, 1, &Bytes, &Flags,
                                 (struct sockaddr *)&From, &FromLength, NULL, NULL);
            ok(Result == 0, "%s: WSARecvFrom returned %d, error %d\n", Name, Result, WSAGetLastError());
            ok(Bytes == (DWORD)SendLength, "%s: %lu bytes, expected %d\n", Name, Bytes, SendLength);
            ok(Flags == 0, "%s: flags 0x%lx\n", Name, Flags);
            break;
        case MODE_WSARECVMSG:
        {
            WSAMSG Message;

            if (!pWSARecvMsg)
            {
                skip("WSARecvMsg is not available\n");
                goto Done;
            }
            ZeroMemory(&Message, sizeof(Message));
            Message.name = (struct sockaddr *)&From;
            Message.namelen = sizeof(From);
            Message.lpBuffers = &Wsabuf;
            Message.dwBufferCount = 1;
            Result = pWSARecvMsg(Receiver, &Message, &Bytes, NULL, NULL);
            ok(Result == 0, "%s: WSARecvMsg returned %d, error %d\n", Name, Result, WSAGetLastError());
            ok(Bytes == (DWORD)SendLength, "%s: %lu bytes, expected %d\n", Name, Bytes, SendLength);
            ok(Message.dwFlags == 0, "%s: flags 0x%lx\n", Name, Message.dwFlags);
            FromLength = Message.namelen;
            break;
        }
    }

Check:
    if (ReportsAddress)
    {
        ok(FromLength == sizeof(From), "%s: address length %d\n", Name, FromLength);
        ok(From.sin_family == AF_INET && From.sin_port == SenderName.sin_port,
           "%s: from family %u port %u, expected port %u\n",
           Name, From.sin_family, ntohs(From.sin_port), ntohs(SenderName.sin_port));
        ok(From.sin_addr.s_addr == Address, "%s: from address 0x%08lx, expected 0x%08lx\n",
           Name, ntohl(From.sin_addr.s_addr), ntohl(Address));
    }

    /* Nothing else is queued */
    {
        u_long NonBlocking = 1;

        ioctlsocket(Receiver, FIONBIO, &NonBlocking);
        Result = recv(Receiver, Buffer, sizeof(Buffer), 0);
        ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK,
           "%s: second recv returned %d, error %d\n", Name, Result, WSAGetLastError());
    }

Done:
    closesocket(Sender);
    closesocket(Receiver);
}

/* Datagrams come out one by one, in order, whatever their length */
static
void
CheckSequence(ULONG Address)
{
    static const struct { const char *Data; int Length; } Datagrams[] = { { "abc", 3 }, { "", 0 }, { "de", 2 }, { "", 0 } };
    SOCKET Receiver, Sender;
    struct sockaddr_in To;
    char Buffer[16];
    int i, Result;
    u_long NonBlocking = 1;

    if (!OpenReceiver(&Receiver, &To, Address))
        return;
    Sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Sender == INVALID_SOCKET || connect(Sender, (struct sockaddr *)&To, sizeof(To)) != 0)
    {
        ok(0, "sender setup failed: %d\n", WSAGetLastError());
        goto Done;
    }

    /* On a connected socket send of zero bytes sends a zero-length datagram */
    for (i = 0; i < 4; i++)
    {
        Result = send(Sender, Datagrams[i].Data, Datagrams[i].Length, 0);
        ok(Result == Datagrams[i].Length, "send %d returned %d, error %d\n", i, Result, WSAGetLastError());
    }
    Sleep(300);

    ioctlsocket(Receiver, FIONBIO, &NonBlocking);
    for (i = 0; i < 4; i++)
    {
        Result = recv(Receiver, Buffer, sizeof(Buffer), 0);
        ok(Result == Datagrams[i].Length, "recv %d returned %d, error %d, expected %d\n",
           i, Result, WSAGetLastError(), Datagrams[i].Length);
    }
    Result = recv(Receiver, Buffer, sizeof(Buffer), 0);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "recv at the end returned %d, error %d\n",
       Result, WSAGetLastError());

Done:
    if (Sender != INVALID_SOCKET)
        closesocket(Sender);
    closesocket(Receiver);
}

static
ULONG
FindInterfaceAddress(void)
{
    IP_ADAPTER_INFO Adapters[8];
    ULONG Size = sizeof(Adapters);
    PIP_ADAPTER_INFO Adapter;

    if (GetAdaptersInfo(Adapters, &Size) != NO_ERROR)
        return 0;
    for (Adapter = Adapters; Adapter; Adapter = Adapter->Next)
    {
        ULONG Address = inet_addr(Adapter->IpAddressList.IpAddress.String);

        if (Address != 0 && Address != INADDR_NONE)
            return Address;
    }
    return 0;
}

START_TEST(udpzerolen)
{
    WSADATA WsaData;
    ULONG Interface;
    int Mode;

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }

    {
        GUID Guid = WSAID_WSARECVMSG;
        DWORD Returned;
        SOCKET Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (Socket != INVALID_SOCKET)
        {
            WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &Guid, sizeof(Guid),
                     &pWSARecvMsg, sizeof(pWSARecvMsg), &Returned, NULL, NULL);
            closesocket(Socket);
        }
    }

    Interface = FindInterfaceAddress();

    for (Mode = MODE_RECV; Mode <= MODE_OVERLAPPED; Mode++)
    {
        CheckReceive("loopback zero-length", htonl(INADDR_LOOPBACK), Mode, 0);
        CheckReceive("loopback one byte", htonl(INADDR_LOOPBACK), Mode, 1);
        if (Interface)
            CheckReceive("interface zero-length", Interface, Mode, 0);
    }
    CheckSequence(htonl(INADDR_LOOPBACK));

    WSACleanup();
}
