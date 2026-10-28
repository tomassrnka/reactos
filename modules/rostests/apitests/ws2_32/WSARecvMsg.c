/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the WSARecvMsg extension function
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * WSARecvMsg is not exported by ws2_32: it is found with
 * SIO_GET_EXTENSION_FUNCTION_POINTER(WSAID_WSARECVMSG). Chrome's UDP sockets do that on every
 * read, and without it every read of a QUIC session fails with ERR_FAILED.
 * The expected values were measured on Windows 10 22H2 (x64 and x86 processes).
 *
 * Control data: with IP_PKTINFO enabled a datagram carries one IP_PKTINFO control message (the
 * destination address and the interface it arrived on), without it none. The control length is
 * always the full size of the data, the control buffer takes as much of it as it can (cut off
 * byte-wise), and a datagram or control data that does not fit fails with WSAEMSGSIZE and
 * MSG_TRUNC / MSG_CTRUNC in the flags, the bytes received still counted. A control buffer
 * needs both a pointer and a size: only one of them fails with WSAEFAULT and leaves the datagram
 * queued. The interface index differs per system and is not compared.
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

/* The control message of a datagram on Windows: header and in_pktinfo, 24 bytes for a 64-bit process, 20 for a 32-bit one */
#define PKTINFO_SIZE ((ULONG)WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))

/*
 * Sends Payload to To from Sender and receives it on Receiver with a message of the given
 * buffer sizes; a ControlSize of 0 with NullControl FALSE is a buffer pointer without a size.
 */
static
INT
ReceiveWithControl(
    SOCKET Receiver,
    SOCKET Sender,
    const struct sockaddr_in *To,
    const char *Payload,
    DWORD DataSize,
    BOOL NullControl,
    DWORD ControlSize,
    WSAMSG *Msg,
    CHAR *Control,
    DWORD *Bytes)
{
    static char Data[64];
    WSABUF Buf;
    struct sockaddr_in From;
    INT Result;

    Send(Sender, To, Payload);
    if (!WaitReadable(Receiver))
    {
        ok(0, "datagram did not arrive\n");
        return -2;
    }

    ZeroMemory(Data, sizeof(Data));
    Buf.buf = Data;
    Buf.len = DataSize;
    FillMemory(Control, 64, 0xAA);
    ZeroMemory(Msg, sizeof(*Msg));
    Msg->name = (LPSOCKADDR)&From;
    Msg->namelen = sizeof(From);
    Msg->lpBuffers = &Buf;
    Msg->dwBufferCount = 1;
    Msg->Control.buf = NullControl ? NULL : Control;
    Msg->Control.len = ControlSize;
    *Bytes = 77;
    SetLastError(0xdeadbeef);
    Result = pWSARecvMsg(Receiver, Msg, Bytes, NULL, NULL);
    return Result;
}

static
VOID
CheckPktInfo(const char *Label, WSAMSG *Msg, ULONG Address)
{
    WSACMSGHDR *Cmsg = WSA_CMSG_FIRSTHDR(Msg);
    IN_PKTINFO *Info;

    ok(Cmsg != NULL, "%s: no control message\n", Label);
    if (!Cmsg)
        return;
    ok(Cmsg->cmsg_len == PKTINFO_SIZE, "%s: cmsg_len %lu\n", Label, (ULONG)Cmsg->cmsg_len);
    ok(Cmsg->cmsg_level == IPPROTO_IP, "%s: cmsg_level %d\n", Label, Cmsg->cmsg_level);
    ok(Cmsg->cmsg_type == IP_PKTINFO, "%s: cmsg_type %d\n", Label, Cmsg->cmsg_type);
    Info = (IN_PKTINFO *)WSA_CMSG_DATA(Cmsg);
    ok(Info->ipi_addr.s_addr == Address, "%s: destination %08lx, expected %08lx\n",
       Label, (ULONG)Info->ipi_addr.s_addr, (ULONG)Address);
    ok(WSA_CMSG_NXTHDR(Msg, Cmsg) == NULL, "%s: more than one control message\n", Label);
}

static
VOID
TestPktInfoOption(VOID)
{
    SOCKET Udp, Tcp;
    struct sockaddr_in Address;
    DWORD Value, Set;
    INT Length, Result;

    Udp = MakeBoundUdp(0, &Address);
    if (Udp == INVALID_SOCKET)
        return;

    Value = 0xdeadbeef;
    Length = sizeof(Value);
    Result = getsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (char *)&Value, &Length);
    ok(Result == 0 && Value == 0 && Length == sizeof(Value), "default: %d, %lu, %d\n", Result, Value, Length);

    /* Any non-zero value switches it on and reads back as 1 */
    Set = 5;
    Result = setsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (const char *)&Set, sizeof(Set));
    ok(Result == 0, "set 5: %d, error %d\n", Result, WSAGetLastError());
    Length = sizeof(Value);
    Result = getsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (char *)&Value, &Length);
    ok(Result == 0 && Value == 1 && Length == sizeof(Value), "after set 5: %d, %lu, %d\n", Result, Value, Length);

    /* A short value or buffer is WSAEFAULT and changes nothing */
    Set = 0;
    SetLastError(0);
    Result = setsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (const char *)&Set, 1);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "set 1 byte: %d, error %d\n", Result, WSAGetLastError());
    Length = 1;
    Result = getsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (char *)&Value, &Length);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "get 1 byte: %d, error %d\n", Result, WSAGetLastError());
    Length = sizeof(Value);
    Value = 0xdeadbeef;
    Result = getsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (char *)&Value, &Length);
    ok(Result == 0 && Value == 1, "unchanged by the short set: %d, %lu\n", Result, Value);

    Set = 0;
    Result = setsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (const char *)&Set, sizeof(Set));
    ok(Result == 0, "set 0: %d, error %d\n", Result, WSAGetLastError());
    Length = sizeof(Value);
    Result = getsockopt(Udp, IPPROTO_IP, IP_PKTINFO, (char *)&Value, &Length);
    ok(Result == 0 && Value == 0, "after set 0: %d, %lu\n", Result, Value);
    closesocket(Udp);

    /* A stream socket has no packet information */
    Tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Tcp != INVALID_SOCKET)
    {
        Set = 1;
        SetLastError(0);
        Result = setsockopt(Tcp, IPPROTO_IP, IP_PKTINFO, (const char *)&Set, sizeof(Set));
        ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "tcp set: %d, error %d\n", Result, WSAGetLastError());
        Length = sizeof(Value);
        Result = getsockopt(Tcp, IPPROTO_IP, IP_PKTINFO, (char *)&Value, &Length);
        ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "tcp get: %d, error %d\n", Result, WSAGetLastError());
        closesocket(Tcp);
    }
}

static
VOID
TestControlData(VOID)
{
    SOCKET Receiver, Sender;
    struct sockaddr_in ReceiverAddress, SenderAddress;
    WSAMSG Msg;
    CHAR Control[64];
    DWORD Bytes, Yes = 1;
    INT Result;
    ULONG Loopback = htonl(INADDR_LOOPBACK);
    ULONG Full = PKTINFO_SIZE;

    Receiver = MakeBoundUdp(0, &ReceiverAddress);
    Sender = MakeBoundUdp(0, &SenderAddress);
    if (Receiver == INVALID_SOCKET || Sender == INVALID_SOCKET)
        goto Done;

    /* Without IP_PKTINFO there is no control data, whatever the buffer */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, 1, &Msg, Control, &Bytes);
    ok(Result == 0, "off, 1 byte: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 5 && Msg.dwFlags == 0 && Msg.Control.len == 0,
       "off, 1 byte: %lu bytes, flags %lx, control %lu\n", Bytes, Msg.dwFlags, Msg.Control.len);
    ok((UCHAR)Control[0] == 0xAA, "off: the control buffer was written\n");

    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, TRUE, 0, &Msg, Control, &Bytes);
    ok(Result == 0, "off, no control: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 5 && Msg.dwFlags == 0 && Msg.Control.len == 0, "off, no control: %lu bytes, flags %lx, control %lu\n",
       Bytes, Msg.dwFlags, Msg.Control.len);

    /* A buffer without a size, or a size without a buffer: WSAEFAULT, nothing is received or touched */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, 0, &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "buffer without size: returned %d, error %d\n",
       Result, WSAGetLastError());
    ok(Bytes == 77, "buffer without size: bytes changed to %lu\n", Bytes);
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "world", 64, TRUE, 24, &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT, "size without buffer: returned %d, error %d\n",
       Result, WSAGetLastError());
    ok(Msg.Control.len == 24 && Bytes == 77, "size without buffer: control %lu, bytes %lu\n", Msg.Control.len, Bytes);
    /* both datagrams are still queued, in order */
    {
        WSABUF Buf;
        char Data[16];

        ZeroMemory(&Msg, sizeof(Msg));
        Buf.buf = Data;
        Buf.len = sizeof(Data);
        Msg.lpBuffers = &Buf;
        Msg.dwBufferCount = 1;
        Result = pWSARecvMsg(Receiver, &Msg, &Bytes, NULL, NULL);
        ok(Result == 0 && Bytes == 5 && !memcmp(Data, "hello", 5), "first queued: %d, %lu\n", Result, Bytes);
        Result = pWSARecvMsg(Receiver, &Msg, &Bytes, NULL, NULL);
        ok(Result == 0 && Bytes == 5 && !memcmp(Data, "world", 5), "second queued: %d, %lu\n", Result, Bytes);
    }

    /* IP_PKTINFO on */
    ok(setsockopt(Receiver, IPPROTO_IP, IP_PKTINFO, (const char *)&Yes, sizeof(Yes)) == 0, "IP_PKTINFO failed\n");

    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, sizeof(Control), &Msg, Control, &Bytes);
    ok(Result == 0, "on: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 5 && Msg.dwFlags == 0 && Msg.Control.len == Full,
       "on: %lu bytes, flags %lx, control %lu (expected %lu)\n", Bytes, Msg.dwFlags, Msg.Control.len, Full);
    CheckPktInfo("on", &Msg, Loopback);

    /* Exactly the size of the message */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, Full, &Msg, Control, &Bytes);
    ok(Result == 0, "exact: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Msg.dwFlags == 0 && Msg.Control.len == Full, "exact: flags %lx, control %lu\n", Msg.dwFlags, Msg.Control.len);
    CheckPktInfo("exact", &Msg, Loopback);

    /* One byte short: the datagram is received, WSAEMSGSIZE and MSG_CTRUNC, the full size is reported */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, Full - 1, &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE, "short by one: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 5 && Msg.dwFlags == MSG_CTRUNC && Msg.Control.len == Full,
       "short by one: %lu bytes, flags %lx, control %lu\n", Bytes, Msg.dwFlags, Msg.Control.len);

    /* A control buffer of 1 byte, and a pointer without a size is WSAEFAULT, see above */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, 1, &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE, "1 byte: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 5 && Msg.dwFlags == MSG_CTRUNC && Msg.Control.len == Full,
       "1 byte: %lu bytes, flags %lx, control %lu\n", Bytes, Msg.dwFlags, Msg.Control.len);
    ok((UCHAR)Control[1] == 0xAA, "1 byte: written past the buffer\n");

    /* The message is cut off byte-wise: the header and the destination address fit in header + 4 bytes */
    {
        DWORD Header = (DWORD)WSA_CMSGDATA_ALIGN(sizeof(WSACMSGHDR));

        Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, Header + 4, &Msg, Control, &Bytes);
        ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE, "cut off: returned %d, error %d\n", Result, WSAGetLastError());
        ok(Msg.dwFlags == MSG_CTRUNC && Msg.Control.len == Full, "cut off: flags %lx, control %lu\n", Msg.dwFlags, Msg.Control.len);
        {
            WSACMSGHDR *Cmsg = (WSACMSGHDR *)Control;

            ok(Cmsg->cmsg_len == Full && Cmsg->cmsg_level == IPPROTO_IP && Cmsg->cmsg_type == IP_PKTINFO,
               "cut off: header %lu, %d, %d\n", (ULONG)Cmsg->cmsg_len, Cmsg->cmsg_level, Cmsg->cmsg_type);
#ifdef _WIN64
            ok(((IN_PKTINFO *)(Control + Header))->ipi_addr.s_addr == Loopback, "cut off: destination missing\n");
            ok((UCHAR)Control[Header + 4] == 0xAA, "cut off: written past the buffer\n");
#else
            /* A 32-bit process gets the header, or the data as a whole, nothing in between */
            ok((UCHAR)Control[Header] == 0xAA && (UCHAR)Control[Header + 3] == 0xAA, "cut off: partial data written\n");
#endif
        }
    }

    /* No control buffer at all: the datagram is received and flagged */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, TRUE, 0, &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE, "no buffer: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 5 && Msg.dwFlags == MSG_CTRUNC && Msg.Control.len == Full,
       "no buffer: %lu bytes, flags %lx, control %lu\n", Bytes, Msg.dwFlags, Msg.Control.len);

    /* A datagram larger than the buffer: MSG_TRUNC, the bytes that fit are counted, the control data is complete */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "0123456789", 4, FALSE, sizeof(Control), &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE, "data cut: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 4 && Msg.dwFlags == MSG_TRUNC && Msg.Control.len == Full,
       "data cut: %lu bytes, flags %lx, control %lu\n", Bytes, Msg.dwFlags, Msg.Control.len);
    CheckPktInfo("data cut", &Msg, Loopback);

    /* both cut off */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "0123456789", 4, FALSE, 1, &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE, "both cut: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 4 && Msg.dwFlags == (MSG_TRUNC | MSG_CTRUNC) && Msg.Control.len == Full,
       "both cut: %lu bytes, flags %lx, control %lu\n", Bytes, Msg.dwFlags, Msg.Control.len);

    /* an empty buffer list receives nothing, the datagram is flagged */
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "0123456789", 0, FALSE, sizeof(Control), &Msg, Control, &Bytes);
    ok(Result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE, "no data buffer: returned %d, error %d\n", Result, WSAGetLastError());
    ok(Bytes == 0 && Msg.dwFlags == MSG_TRUNC && Msg.Control.len == Full,
       "no data buffer: %lu bytes, flags %lx, control %lu\n", Bytes, Msg.dwFlags, Msg.Control.len);

    /* Switched off again */
    Yes = 0;
    ok(setsockopt(Receiver, IPPROTO_IP, IP_PKTINFO, (const char *)&Yes, sizeof(Yes)) == 0, "IP_PKTINFO off failed\n");
    Result = ReceiveWithControl(Receiver, Sender, &ReceiverAddress, "hello", 64, FALSE, sizeof(Control), &Msg, Control, &Bytes);
    ok(Result == 0 && Msg.Control.len == 0 && Msg.dwFlags == 0, "off again: %d, control %lu, flags %lx\n",
       Result, Msg.Control.len, Msg.dwFlags);

    /* The address the datagram was sent to, when it is not the loopback address: the adapter address */
    {
        char Name[256];
        struct hostent *Host;
        struct sockaddr_in Adapter;
        SOCKET Bound, Source;

        Yes = 1;
        Host = (gethostname(Name, sizeof(Name)) == 0) ? gethostbyname(Name) : NULL;
        if (Host && Host->h_addrtype == AF_INET && Host->h_addr_list[0] &&
            *(ULONG *)Host->h_addr_list[0] != Loopback && *(ULONG *)Host->h_addr_list[0] != 0)
        {
            ULONG AdapterAddress = *(ULONG *)Host->h_addr_list[0];

            /* the sender is not bound to the loopback address, so it can reach the adapter's */
            Source = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Bound = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            ZeroMemory(&Adapter, sizeof(Adapter));
            Adapter.sin_family = AF_INET;
            if (Bound != INVALID_SOCKET &&
                bind(Bound, (SOCKADDR *)&Adapter, sizeof(Adapter)) == 0)
            {
                INT AdapterLength = sizeof(Adapter);

                getsockname(Bound, (SOCKADDR *)&Adapter, &AdapterLength);
            }
            if (Bound != INVALID_SOCKET && Source != INVALID_SOCKET)
            {
                setsockopt(Bound, IPPROTO_IP, IP_PKTINFO, (const char *)&Yes, sizeof(Yes));
                Adapter.sin_addr.s_addr = AdapterAddress;
                Result = ReceiveWithControl(Bound, Source, &Adapter, "hello", 64, FALSE, sizeof(Control), &Msg, Control, &Bytes);
                if (Result != -2)
                {
                    ok(Result == 0, "adapter: returned %d, error %d\n", Result, WSAGetLastError());
                    CheckPktInfo("adapter", &Msg, AdapterAddress);
                }
            }
            if (Bound != INVALID_SOCKET)
                closesocket(Bound);
            if (Source != INVALID_SOCKET)
                closesocket(Source);
        }
        else
        {
            skip("no adapter address, IP_PKTINFO for it is not tested\n");
        }
    }

Done:
    if (Receiver != INVALID_SOCKET)
        closesocket(Receiver);
    if (Sender != INVALID_SOCKET)
        closesocket(Sender);
}

/* The control data of an overlapped receive is written when it completes, into the caller's message */
static
VOID
TestControlDataOverlapped(VOID)
{
    SOCKET Receiver, Sender;
    struct sockaddr_in ReceiverAddress, SenderAddress;
    WSAOVERLAPPED Overlapped;
    WSAMSG Msg;
    WSABUF Buf;
    CHAR Control[64], Data[16];
    DWORD Yes = 1, Wait, Transferred, Flags;
    INT Result;
    BOOL Success;
    ULONG Loopback = htonl(INADDR_LOOPBACK);
    ULONG Full = PKTINFO_SIZE;
    DWORD Sizes[2] = { 1, sizeof(Control) };
    UINT i;

    Receiver = MakeBoundUdp(WSA_FLAG_OVERLAPPED, &ReceiverAddress);
    Sender = MakeBoundUdp(0, &SenderAddress);
    if (Receiver == INVALID_SOCKET || Sender == INVALID_SOCKET)
        goto Done;
    ok(setsockopt(Receiver, IPPROTO_IP, IP_PKTINFO, (const char *)&Yes, sizeof(Yes)) == 0, "IP_PKTINFO failed\n");

    for (i = 0; i < 2; i++)
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = WSACreateEvent();
        FillMemory(Control, sizeof(Control), 0xAA);
        ZeroMemory(&Msg, sizeof(Msg));
        Buf.buf = Data;
        Buf.len = sizeof(Data);
        Msg.lpBuffers = &Buf;
        Msg.dwBufferCount = 1;
        Msg.Control.buf = Control;
        Msg.Control.len = Sizes[i];
        Result = pWSARecvMsg(Receiver, &Msg, NULL, &Overlapped, NULL);
        ok(Result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING, "pending %u: returned %d, error %d\n", i, Result, WSAGetLastError());
        ok(Msg.Control.len == Sizes[i] && Msg.dwFlags == 0, "pending %u: control %lu, flags %lx\n", i, Msg.Control.len, Msg.dwFlags);

        Send(Sender, &ReceiverAddress, "ovl!");
        Wait = WaitForSingleObject(Overlapped.hEvent, 5000);
        ok(Wait == WAIT_OBJECT_0, "completion %u: wait %lu\n", i, Wait);
        if (Wait == WAIT_OBJECT_0)
        {
            Transferred = 0xdeadbeef;
            Flags = 0xdeadbeef;
            SetLastError(0);
            Success = WSAGetOverlappedResult(Receiver, &Overlapped, &Transferred, FALSE, &Flags);
            ok(Transferred == 4, "completion %u: %lu bytes\n", i, Transferred);
            ok(Msg.Control.len == Full, "completion %u: control %lu\n", i, Msg.Control.len);
            if (i == 0)
            {
                /* Too small: the result is WSAEMSGSIZE and the message carries MSG_CTRUNC */
                ok(!Success && WSAGetLastError() == WSAEMSGSIZE, "completion %u: result %d, error %d\n", i, Success, WSAGetLastError());
                ok(Msg.dwFlags == MSG_CTRUNC, "completion %u: flags %lx\n", i, Msg.dwFlags);
            }
            else
            {
                ok(Success, "completion %u: result failed, error %d\n", i, WSAGetLastError());
                ok(Flags == 0 && Msg.dwFlags == 0, "completion %u: flags %lx, message flags %lx\n", i, Flags, Msg.dwFlags);
                CheckPktInfo("overlapped", &Msg, Loopback);
            }
        }
        WSACloseEvent(Overlapped.hEvent);
    }

Done:
    if (Receiver != INVALID_SOCKET)
        closesocket(Receiver);
    if (Sender != INVALID_SOCKET)
        closesocket(Sender);
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
        ok(Bytes == 4, "truncated: %lu bytes\n", Bytes);
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

    closesocket(Receiver);
    Receiver = INVALID_SOCKET;

    TestPktInfoOption();
    TestControlData();
    TestControlDataOverlapped();

Done:
    if (Receiver != INVALID_SOCKET)
        closesocket(Receiver);
    if (Sender != INVALID_SOCKET)
        closesocket(Sender);
    WSACleanup();
}
