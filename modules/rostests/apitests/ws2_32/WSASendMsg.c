/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for WSASendMsg
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values were measured on Windows Server 2008 R2 (x86 and x64).
 */

#include "ws2_32.h"

typedef INT (WINAPI *FN_WSASendMsg)(SOCKET, LPWSAMSG, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

static FN_WSASendMsg pWSASendMsg;
static SOCKET g_Receiver;
static struct sockaddr_in g_ReceiverAddress;

#define NOT_SET 0xDEADBEEF

static void CheckFailure(PCSTR What, INT Ret, INT Error, INT ExpectedError, DWORD Sent)
{
    ok(Ret == SOCKET_ERROR, "%s: returned %d\n", What, Ret);
    ok(Error == ExpectedError, "%s: error %d, expected %d\n", What, Error, ExpectedError);
    ok(Sent == NOT_SET, "%s: bytes sent %lu\n", What, Sent);
}

#define RECEIVE_TIMEOUT -1
#define RECEIVE_ERROR -2

/* The length of the datagram that arrived, RECEIVE_TIMEOUT if none did, or RECEIVE_ERROR */
static int ReceiveOn(SOCKET Socket, char *Buffer, int Size, long Milliseconds)
{
    fd_set Readable;
    struct timeval Timeout;
    int Ready;

    Timeout.tv_sec = Milliseconds / 1000;
    Timeout.tv_usec = (Milliseconds % 1000) * 1000;
    FD_ZERO(&Readable);
    FD_SET(Socket, &Readable);
    Ready = select(0, &Readable, NULL, NULL, &Timeout);
    if (Ready == 0)
        return RECEIVE_TIMEOUT;
    if (Ready != 1)
        return RECEIVE_ERROR;
    Ready = recvfrom(Socket, Buffer, Size, 0, NULL, NULL);
    return Ready < 0 ? RECEIVE_ERROR : Ready;
}

static int Receive(char *Buffer, int Size, long Milliseconds)
{
    return ReceiveOn(g_Receiver, Buffer, Size, Milliseconds);
}

static void ExpectDatagram(PCSTR What, const char *Expected, int ExpectedLength)
{
    char Buffer[128];
    int Length;

    Length = Receive(Buffer, sizeof(Buffer), 2000);
    ok(Length == ExpectedLength, "%s: received %d bytes, expected %d\n", What, Length, ExpectedLength);
    if (Length == ExpectedLength && Length > 0)
        ok(!memcmp(Buffer, Expected, Length), "%s: wrong data\n", What);
}

static void ExpectNothing(PCSTR What)
{
    char Buffer[128];
    int Length = Receive(Buffer, sizeof(Buffer), 500);

    ok(Length == RECEIVE_TIMEOUT, "%s: unexpected result %d\n", What, Length);
}

static void InitMessage(WSAMSG *Msg, WSABUF *Buffers, DWORD Count)
{
    ZeroMemory(Msg, sizeof(*Msg));
    Msg->name = (LPSOCKADDR)&g_ReceiverAddress;
    Msg->namelen = sizeof(g_ReceiverAddress);
    Msg->lpBuffers = Buffers;
    Msg->dwBufferCount = Count;
}

static void Send(PCSTR What, SOCKET Socket, WSAMSG *Msg, DWORD Flags)
{
    DWORD Sent = NOT_SET;
    INT Ret;

    WSASetLastError(0);
    Ret = pWSASendMsg(Socket, Msg, Flags, &Sent, NULL, NULL);
    ok(Ret == 0, "%s: returned %d, error %d\n", What, Ret, WSAGetLastError());
    ok(Sent == 10, "%s: sent %lu bytes\n", What, Sent);
}

static void TestData(SOCKET Sender)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[70];
    WSAMSG Msg;
    DWORD Sent;
    INT Ret;
    int i;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;
    Buffers[2].buf = Hello; Buffers[2].len = 0;

    /* The buffers are gathered into one datagram; the flags of the message are ignored */
    InitMessage(&Msg, Buffers, 2);
    Msg.dwFlags = 0xFFFF;
    Send("two buffers", Sender, &Msg, 0);
    ExpectDatagram("two buffers", "helloworld", 10);
    ok(Msg.dwFlags == 0xFFFF, "The message flags changed to %lx\n", Msg.dwFlags);

    InitMessage(&Msg, Buffers, 3);
    Send("empty buffer last", Sender, &Msg, 0);
    ExpectDatagram("empty buffer last", "helloworld", 10);

    Buffers[0].len = 0;
    InitMessage(&Msg, Buffers, 1);
    Sent = NOT_SET;
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    ok(Ret == 0, "One empty buffer: returned %d, error %d\n", Ret, WSAGetLastError());
    ok(Sent == 0, "One empty buffer: sent %lu bytes\n", Sent);
    /* The TCP/IP driver of ReactOS sends nothing for an empty datagram */
    todo_if(is_reactos())
    {
        ExpectDatagram("empty datagram", "", 0);
    }
    Buffers[0].len = 5;

    /* A NULL buffer is fine when it is empty */
    Buffers[0].buf = NULL; Buffers[0].len = 0;
    InitMessage(&Msg, Buffers, 2);
    Sent = NOT_SET;
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    ok(Ret == 0, "NULL empty buffer: returned %d, error %d\n", Ret, WSAGetLastError());
    ok(Sent == 5, "NULL empty buffer: sent %lu bytes\n", Sent);
    ExpectDatagram("NULL empty buffer", "world", 5);

    Buffers[0].buf = NULL; Buffers[0].len = 5;
    InitMessage(&Msg, Buffers, 2);
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("NULL buffer with a length", Ret, WSAGetLastError(), WSAEFAULT, Sent);
    Buffers[0].buf = Hello;

    /* Many one-byte buffers */
    for (i = 0; i < 70; i++)
    {
        Buffers[i].buf = Hello;
        Buffers[i].len = 1;
    }
    InitMessage(&Msg, Buffers, 65);
    Sent = NOT_SET;
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    ok(Ret == 0, "65 buffers: returned %d, error %d\n", Ret, WSAGetLastError());
    ok(Sent == 65, "65 buffers: sent %lu bytes\n", Sent);
    {
        char Expected[65];

        memset(Expected, 'h', sizeof(Expected));
        ExpectDatagram("65 buffers", Expected, 65);
    }
}

static void TestArguments(SOCKET Sender)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[2];
    WSAMSG Msg;
    DWORD Sent;
    INT Ret;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;
    InitMessage(&Msg, Buffers, 2);

    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, NULL, 0, &Sent, NULL, NULL);
    CheckFailure("no message", Ret, WSAGetLastError(), WSAEFAULT, Sent);

    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(INVALID_SOCKET, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("invalid socket", Ret, WSAGetLastError(), WSAENOTSOCK, Sent);

    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg((SOCKET)0x1234, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("bogus socket", Ret, WSAGetLastError(), WSAENOTSOCK, Sent);

    InitMessage(&Msg, Buffers, 0);
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("no buffers", Ret, WSAGetLastError(), WSAEINVAL, Sent);

    InitMessage(&Msg, NULL, 2);
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("NULL buffer array", Ret, WSAGetLastError(), WSAEINVAL, Sent);

    /* The flags that a datagram send accepts: out of band, do not route and partial */
    InitMessage(&Msg, Buffers, 2);
    Send("MSG_OOB", Sender, &Msg, MSG_OOB);
    ExpectDatagram("MSG_OOB", "helloworld", 10);
    Send("MSG_DONTROUTE", Sender, &Msg, MSG_DONTROUTE);
    ExpectDatagram("MSG_DONTROUTE", "helloworld", 10);
    Send("MSG_PARTIAL", Sender, &Msg, MSG_PARTIAL);
    ExpectDatagram("MSG_PARTIAL", "helloworld", 10);
    Send("all three", Sender, &Msg, MSG_OOB | MSG_DONTROUTE | MSG_PARTIAL);
    ExpectDatagram("all three", "helloworld", 10);

    {
        static const DWORD Bad[] = { MSG_PEEK, 0x8, 0x10, 0x100, 0x1000, 0x4000, 0x10000, 0x80000000, 0xFFFFFFFF };
        unsigned i;

        for (i = 0; i < ARRAYSIZE(Bad); i++)
        {
            Sent = NOT_SET;
            WSASetLastError(0);
            Ret = pWSASendMsg(Sender, &Msg, Bad[i], &Sent, NULL, NULL);
            CheckFailure("bad flags", Ret, WSAGetLastError(), WSAEOPNOTSUPP, Sent);
        }
    }
    ExpectNothing("after the failures");
}

static void TestName(SOCKET Sender)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[2];
    WSAMSG Msg;
    DWORD Sent;
    INT Ret, Length;
    struct sockaddr_in Address;
    static const int BadLengths[] = { 2, 8, 14, 15, 17, 20, 24, 28, 32, 40, 100 };
    unsigned i;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;

    /* An unconnected datagram socket needs a destination */
    InitMessage(&Msg, Buffers, 2);
    Msg.name = NULL;
    Msg.namelen = 0;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("no name", Ret, WSAGetLastError(), WSAEINVAL, Sent);

    /* A length without a name, and a name of the wrong length */
    Msg.namelen = sizeof(Address);
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("length without a name", Ret, WSAGetLastError(), WSAEFAULT, Sent);

    for (i = 0; i < ARRAYSIZE(BadLengths); i++)
    {
        Length = BadLengths[i];
        Msg.name = (LPSOCKADDR)&g_ReceiverAddress;
        Msg.namelen = Length;
        Sent = NOT_SET;
        WSASetLastError(0);
        Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
        ok(Ret == SOCKET_ERROR, "Name length %d: returned %d\n", Length, Ret);
        ok(WSAGetLastError() == WSAEFAULT, "Name length %d: error %d\n", Length, WSAGetLastError());
    }

    /* Another address family */
    Address = g_ReceiverAddress;
    Address.sin_family = AF_UNSPEC;
    InitMessage(&Msg, Buffers, 2);
    Msg.name = (LPSOCKADDR)&Address;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("AF_UNSPEC", Ret, WSAGetLastError(), WSAEAFNOSUPPORT, Sent);

    Address.sin_family = AF_INET6;
    Sent = NOT_SET;
    WSASetLastError(0);
    Msg.namelen = sizeof(SOCKADDR_IN6);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("AF_INET6", Ret, WSAGetLastError(), WSAEAFNOSUPPORT, Sent);

    ExpectNothing("after the failures");

    /* Port 0 is accepted */
    Address = g_ReceiverAddress;
    Address.sin_port = 0;
    InitMessage(&Msg, Buffers, 2);
    Msg.name = (LPSOCKADDR)&Address;
    Send("port 0", Sender, &Msg, 0);
}

static void TestConnected(void)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[2];
    WSAMSG Msg;
    SOCKET Sender;
    DWORD Sent;
    INT Ret;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;

    Sender = socket(AF_INET, SOCK_DGRAM, 0);
    ok(Sender != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Sender == INVALID_SOCKET)
        return;
    ok(connect(Sender, (struct sockaddr *)&g_ReceiverAddress, sizeof(g_ReceiverAddress)) == 0, "connect failed: %d\n", WSAGetLastError());

    /* A connected socket needs no name; one given is used */
    InitMessage(&Msg, Buffers, 2);
    Msg.name = NULL;
    Msg.namelen = 0;
    Send("connected, no name", Sender, &Msg, 0);
    ExpectDatagram("connected, no name", "helloworld", 10);

    InitMessage(&Msg, Buffers, 2);
    Send("connected, with name", Sender, &Msg, 0);
    ExpectDatagram("connected, with name", "helloworld", 10);

    /* A name that differs from the peer's is the destination */
    {
        SOCKET Other = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in OtherAddress;
        int OtherLength = sizeof(OtherAddress);
        char Buffer[32];
        int Length;

        ZeroMemory(&OtherAddress, sizeof(OtherAddress));
        OtherAddress.sin_family = AF_INET;
        OtherAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (Other != INVALID_SOCKET &&
            bind(Other, (struct sockaddr *)&OtherAddress, sizeof(OtherAddress)) == 0 &&
            getsockname(Other, (struct sockaddr *)&OtherAddress, &OtherLength) == 0)
        {
            InitMessage(&Msg, Buffers, 2);
            Msg.name = (LPSOCKADDR)&OtherAddress;
            Send("connected, other name", Sender, &Msg, 0);
            Length = ReceiveOn(Other, Buffer, sizeof(Buffer), 2000);
            ok(Length == 10, "The other receiver got %d bytes\n", Length);
            ExpectNothing("the connected peer after a send to another name");
        }
        else
        {
            skip("Cannot set up a second receiver\n");
        }
        if (Other != INVALID_SOCKET)
            closesocket(Other);
    }

    closesocket(Sender);

    /* Stream sockets are not supported */
    {
        SOCKET Listener = socket(AF_INET, SOCK_STREAM, 0), Client = socket(AF_INET, SOCK_STREAM, 0), Accepted;
        struct sockaddr_in Address;
        int Length = sizeof(Address);

        ZeroMemory(&Address, sizeof(Address));
        Address.sin_family = AF_INET;
        Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ok(bind(Listener, (struct sockaddr *)&Address, sizeof(Address)) == 0, "bind failed: %d\n", WSAGetLastError());
        ok(getsockname(Listener, (struct sockaddr *)&Address, &Length) == 0, "getsockname failed\n");
        ok(listen(Listener, 1) == 0, "listen failed\n");

        InitMessage(&Msg, Buffers, 2);
        Msg.name = NULL;
        Msg.namelen = 0;
        Sent = NOT_SET;
        WSASetLastError(0);
        Ret = pWSASendMsg(Client, &Msg, 0, &Sent, NULL, NULL);
        CheckFailure("TCP, not connected", Ret, WSAGetLastError(), WSAEINVAL, Sent);

        if (connect(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0)
        {
            Accepted = accept(Listener, NULL, NULL);
            Sent = NOT_SET;
            WSASetLastError(0);
            Ret = pWSASendMsg(Client, &Msg, 0, &Sent, NULL, NULL);
            CheckFailure("TCP, connected", Ret, WSAGetLastError(), WSAEINVAL, Sent);
            closesocket(Accepted);
        }
        else
        {
            skip("Cannot connect over loopback: %d\n", WSAGetLastError());
        }
        closesocket(Client);
        closesocket(Listener);
    }
}

static void TestPacketInfo(WSABUF *Buffers);

/* A header of the type, with a zeroed payload, must fail */
static void ControlFails(PCSTR What, SOCKET Sender, WSAMSG *Msg, char *Control, int Level, int Type, ULONG Payload, INT Error)
{
    WSACMSGHDR *Header = (WSACMSGHDR *)Control;
    DWORD Sent = NOT_SET;
    INT Ret;

    ZeroMemory(Control, 64);
    Header->cmsg_len = sizeof(WSACMSGHDR) + Payload;
    Header->cmsg_level = Level;
    Header->cmsg_type = Type;
    Msg->Control.buf = Control;
    Msg->Control.len = WSA_CMSGDATA_ALIGN(Header->cmsg_len);
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, Msg, 0, &Sent, NULL, NULL);
    CheckFailure(What, Ret, WSAGetLastError(), Error, Sent);
}

static void TestControl(SOCKET Sender)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[2];
    char Control[64];
    WSACMSGHDR *Header = (WSACMSGHDR *)Control;
    WSAMSG Msg;
    DWORD Sent;
    INT Ret;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;

    /* A control buffer without length, or a length without buffer */
    InitMessage(&Msg, Buffers, 2);
    Msg.Control.buf = Control;
    Msg.Control.len = 0;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("control buffer, no length", Ret, WSAGetLastError(), WSAEFAULT, Sent);

    Msg.Control.buf = NULL;
    Msg.Control.len = 16;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("control length, no buffer", Ret, WSAGetLastError(), WSAEFAULT, Sent);

    /* Levels that are not an IP protocol */
    ZeroMemory(Control, sizeof(Control));
    Header->cmsg_len = sizeof(WSACMSGHDR) + 8;
    Header->cmsg_level = 0x7FFF;
    Header->cmsg_type = 99;
    Msg.Control.buf = Control;
    Msg.Control.len = Header->cmsg_len;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("bogus level", Ret, WSAGetLastError(), WSAEINVAL, Sent);

    Header->cmsg_level = SOL_SOCKET;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("SOL_SOCKET level", Ret, WSAGetLastError(), WSAEINVAL, Sent);

    /* A header that does not fit */
    Header->cmsg_level = IPPROTO_IP;
    Header->cmsg_type = IP_TOS;
    Header->cmsg_len = sizeof(WSACMSGHDR) + 4;
    Msg.Control.len = sizeof(WSACMSGHDR) + 2;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("length beyond the buffer", Ret, WSAGetLastError(), WSAEINVAL, Sent);

    Header->cmsg_len = 4;
    Msg.Control.len = 16;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("length below the header", Ret, WSAGetLastError(), WSAEINVAL, Sent);
    ExpectNothing("after the failures");

    /* The types and the sizes of payload that an IPv4 socket accepts: others are invalid, whatever else is in the buffer */
    ControlFails("unknown type", Sender, &Msg, Control, IPPROTO_IP, 99, 8, WSAEINVAL);
    ControlFails("type 0", Sender, &Msg, Control, IPPROTO_IP, 0, 8, WSAEINVAL);
    ControlFails("IPv6 level on an IPv4 socket", Sender, &Msg, Control, IPPROTO_IPV6, IPV6_PKTINFO, 20, WSAEINVAL);
    ControlFails("IP_TOS without payload", Sender, &Msg, Control, IPPROTO_IP, IP_TOS, 0, WSAEINVAL);
    ControlFails("IP_TOS with 2 bytes", Sender, &Msg, Control, IPPROTO_IP, IP_TOS, 2, WSAEINVAL);
    ControlFails("IP_DONTFRAGMENT with 3 bytes", Sender, &Msg, Control, IPPROTO_IP, IP_DONTFRAGMENT, 3, WSAEINVAL);
    ControlFails("IP_OPTIONS with 1 byte", Sender, &Msg, Control, IPPROTO_IP, IP_OPTIONS, 1, WSAEINVAL);
    ControlFails("IP_PKTINFO with 4 bytes", Sender, &Msg, Control, IPPROTO_IP, IP_PKTINFO, 4, WSAEINVAL);
    ControlFails("type 32 with a payload", Sender, &Msg, Control, IPPROTO_IP, 32, 4, WSAEINVAL);

    /* Packet information that asks for no source and no interface asks for nothing, which is refused */
    ZeroMemory(Control, sizeof(Control));
    Header->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
    Header->cmsg_level = IPPROTO_IP;
    Header->cmsg_type = IP_PKTINFO;
    Msg.Control.len = WSA_CMSGDATA_ALIGN(Header->cmsg_len);
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("IP_PKTINFO with nothing", Ret, WSAGetLastError(), WSAEINVAL, Sent);

    /* A malformed header after a good one is found before anything is refused for its effect */
    {
        IN_PKTINFO *Info;
        WSACMSGHDR *Second;
        ULONG First = WSA_CMSGDATA_ALIGN(WSA_CMSG_LEN(sizeof(IN_PKTINFO)));

        ZeroMemory(Control, sizeof(Control));
        Header->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
        Header->cmsg_level = IPPROTO_IP;
        Header->cmsg_type = IP_PKTINFO;
        Info = (IN_PKTINFO *)WSA_CMSG_DATA(Header);
        Info->ipi_addr.s_addr = htonl(INADDR_LOOPBACK);
        Second = (WSACMSGHDR *)(Control + First);
        Second->cmsg_len = sizeof(WSACMSGHDR);
        Second->cmsg_level = IPPROTO_IP;
        Second->cmsg_type = 99;
        Msg.Control.len = First + sizeof(WSACMSGHDR);
        Sent = NOT_SET;
        WSASetLastError(0);
        Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
        CheckFailure("IP_PKTINFO and an unknown type", Ret, WSAGetLastError(), WSAEINVAL, Sent);
    }
    ExpectNothing("after the control failures");

    /* Options that a datagram can carry on Windows; ReactOS has no way to apply them and says so rather than send without them */
    {
        INT Tos = 0x10;

        ZeroMemory(Control, sizeof(Control));
        Header->cmsg_len = WSA_CMSG_LEN(sizeof(Tos));
        Header->cmsg_level = IPPROTO_IP;
        Header->cmsg_type = IP_TOS;
        memcpy(WSA_CMSG_DATA(Header), &Tos, sizeof(Tos));
        Msg.Control.len = WSA_CMSGDATA_ALIGN(Header->cmsg_len);
        Sent = NOT_SET;
        WSASetLastError(0);
        Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
        todo_if(is_reactos())
        {
            ok(Ret == 0, "IP_TOS: returned %d, error %d\n", Ret, WSAGetLastError());
        }
        if (is_reactos())
        {
            ok(Ret == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP && Sent == NOT_SET, "IP_TOS: returned %d, error %d, sent %lu\n",
               Ret, WSAGetLastError(), Sent);
            ExpectNothing("IP_TOS refused");
        }
        else
        {
            ExpectDatagram("IP_TOS", "helloworld", 10);
        }
    }

    TestPacketInfo(Buffers);
}

/* Packet information for the address the socket is bound to: nothing to apply, so the datagram goes out from that address */
static void TestPacketInfo(WSABUF *Buffers)
{
    char Control[64];
    WSACMSGHDR *Header = (WSACMSGHDR *)Control;
    IN_PKTINFO *Info;
    WSAMSG Msg;
    SOCKET Bound = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in Local;
    char Data[64];
    int Length;
    DWORD Sent = NOT_SET;
    INT Ret;

    ok(Bound != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Bound == INVALID_SOCKET)
        return;
    ZeroMemory(&Local, sizeof(Local));
    Local.sin_family = AF_INET;
    Local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Bound, (struct sockaddr *)&Local, sizeof(Local)) == 0, "bind failed: %d\n", WSAGetLastError());

    InitMessage(&Msg, Buffers, 2);
    ZeroMemory(Control, sizeof(Control));
    Header->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
    Header->cmsg_level = IPPROTO_IP;
    Header->cmsg_type = IP_PKTINFO;
    Info = (IN_PKTINFO *)WSA_CMSG_DATA(Header);
    Info->ipi_addr.s_addr = htonl(INADDR_LOOPBACK);
    Msg.Control.buf = Control;
    Msg.Control.len = WSA_CMSGDATA_ALIGN(Header->cmsg_len);
    WSASetLastError(0);
    Ret = pWSASendMsg(Bound, &Msg, 0, &Sent, NULL, NULL);
    ok(Ret == 0, "IP_PKTINFO for the bound address: returned %d, error %d\n", Ret, WSAGetLastError());
    ok(Sent == 10, "IP_PKTINFO for the bound address: sent %lu\n", Sent);
    Length = Receive(Data, sizeof(Data), 2000);
    ok(Length == 10, "IP_PKTINFO for the bound address: received %d\n", Length);

    /* An interface cannot be selected: refused (Windows sends the datagram) */
    Info->ipi_ifindex = 1;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Bound, &Msg, 0, &Sent, NULL, NULL);
    if (is_reactos())
    {
        ok(Ret == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP && Sent == NOT_SET, "IP_PKTINFO with an interface: returned %d, error %d\n",
           Ret, WSAGetLastError());
        ExpectNothing("IP_PKTINFO with an interface refused");
    }
    else
    {
        ok(Ret == 0, "IP_PKTINFO with an interface: returned %d, error %d\n", Ret, WSAGetLastError());
        ExpectDatagram("IP_PKTINFO with an interface", "helloworld", 10);
    }

    /* Another local address cannot be selected either (the one given is not the one of the socket) */
    Info->ipi_ifindex = 0;
    Info->ipi_addr.s_addr = htonl(0x01020304); /* 1.2.3.4, not an address of this host */
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Bound, &Msg, 0, &Sent, NULL, NULL);
    if (is_reactos())
    {
        ok(Ret == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP && Sent == NOT_SET, "IP_PKTINFO for another address: returned %d, error %d\n",
           Ret, WSAGetLastError());
        ExpectNothing("IP_PKTINFO for another address refused");
    }
    else
    {
        /* Windows Server 2008 R2 and Windows 10 send the datagram whatever the address */
        ok(Ret == 0, "IP_PKTINFO for another address: returned %d, error %d\n", Ret, WSAGetLastError());
        ExpectDatagram("IP_PKTINFO for another address", "helloworld", 10);
    }

    closesocket(Bound);
}

static void TestRawSize(void);

static void TestTooBig(SOCKET Sender)
{
    static char Hello[] = "hello";
    WSABUF Buffers[1];
    WSAMSG Msg;
    char *Big = HeapAlloc(GetProcessHeap(), 0, 70000);
    DWORD Sent;
    INT Ret;

    ok(Big != NULL, "No memory\n");
    if (!Big)
        return;
    FillMemory(Big, 70000, 'x');
    Buffers[0].buf = Big;
    Buffers[0].len = 70000;
    InitMessage(&Msg, Buffers, 1);
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("70000 bytes", Ret, WSAGetLastError(), WSAEMSGSIZE, Sent);
    HeapFree(GetProcessHeap(), 0, Big);

    /* The socket still works */
    Buffers[0].buf = Hello;
    Buffers[0].len = 5;
    Sent = NOT_SET;
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, NULL);
    ok(Ret == 0, "Send after the failure: returned %d, error %d\n", Ret, WSAGetLastError());
    ExpectDatagram("after too big", "hello", 5);

    TestRawSize();
}

/* A raw socket gets an IPv4 header of 20 bytes in front of the data: 65515 bytes are the most it can carry */
static void TestRawSize(void)
{
    static const int Sizes[] = { 65515, 65516, 65520, 65535, 65536 };
    SOCKET Raw = WSASocketW(AF_INET, SOCK_RAW, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    struct sockaddr_in Destination = g_ReceiverAddress;
    WSABUF Buffers[1];
    WSAMSG Msg;
    char *Big;
    unsigned i;

    if (Raw == INVALID_SOCKET)
    {
        skip("No raw socket: %d\n", WSAGetLastError());
        return;
    }
    Big = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 70000);
    ok(Big != NULL, "No memory\n");
    if (!Big)
    {
        closesocket(Raw);
        return;
    }

    Destination.sin_port = 0;
    Buffers[0].buf = Big;
    for (i = 0; i < ARRAYSIZE(Sizes); i++)
    {
        DWORD Sent = NOT_SET;
        INT Ret;
        char What[32];

        InitMessage(&Msg, Buffers, 1);
        Msg.name = (LPSOCKADDR)&Destination;
        Msg.namelen = sizeof(Destination);
        Buffers[0].len = Sizes[i];
        sprintf(What, "raw, %d bytes", Sizes[i]);
        WSASetLastError(0);
        Ret = pWSASendMsg(Raw, &Msg, 0, &Sent, NULL, NULL);
        if (Sizes[i] > 65515)
            CheckFailure(What, Ret, WSAGetLastError(), WSAEMSGSIZE, Sent);
        else
            ok(Ret == 0 || WSAGetLastError() != WSAEMSGSIZE, "%s: returned %d, error %d\n", What, Ret, WSAGetLastError());
    }
    HeapFree(GetProcessHeap(), 0, Big);
    closesocket(Raw);
}

static int g_Completions;
static DWORD g_CompletionError, g_CompletionBytes;

static void CALLBACK Completion(DWORD Error, DWORD Bytes, LPWSAOVERLAPPED Overlapped, DWORD Flags)
{
    g_Completions++;
    g_CompletionError = Error;
    g_CompletionBytes = Bytes;
}

/* One overlapped send with an event and one with a completion routine, of Count buffers; FALSE if the socket cannot go on */
static BOOL OverlappedRounds(SOCKET Sender, WSABUF *Buffers, DWORD Count, const char *Expected, int ExpectedLength)
{
    WSAMSG Msg;
    /* Static: after a timeout the operation may still own the structure */
    static WSAOVERLAPPED Overlapped;
    DWORD Sent, Transferred, Flags;
    INT Ret;
    int i;
    char What[64];

    InitMessage(&Msg, Buffers, Count);

    /* With an event: done at once or pending, and then the result is the byte count */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = WSACreateEvent();
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, &Overlapped, NULL);
    ok(Ret == 0 || (Ret == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING), "%lu buffers, event: returned %d, error %d\n",
       Count, Ret, WSAGetLastError());
    if (Ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
    {
        WSACloseEvent(Overlapped.hEvent);
        return FALSE;
    }
    if (WSAWaitForMultipleEvents(1, &Overlapped.hEvent, TRUE, 2000, FALSE) != WSA_WAIT_EVENT_0)
    {
        ok(0, "%lu buffers: the event is not set\n", Count);
        /* Cancel what may still own the structure, and wait for it to end before the event goes */
        CancelIo((HANDLE)Sender);
        WaitForSingleObject(Overlapped.hEvent, 1000);
        WSACloseEvent(Overlapped.hEvent);
        return FALSE;
    }
    Transferred = 0;
    Flags = 0;
    ok(WSAGetOverlappedResult(Sender, &Overlapped, &Transferred, FALSE, &Flags), "No result: %d\n", WSAGetLastError());
    ok(Transferred == (DWORD)ExpectedLength, "%lu buffers: transferred %lu bytes\n", Count, Transferred);
    sprintf(What, "%lu buffers, overlapped event", Count);
    ExpectDatagram(What, Expected, ExpectedLength);
    WSACloseEvent(Overlapped.hEvent);

    /* With a completion routine */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    g_Completions = 0;
    Sent = NOT_SET;
    WSASetLastError(0);
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, &Overlapped, Completion);
    ok(Ret == 0 || (Ret == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING), "%lu buffers, routine: returned %d, error %d\n",
       Count, Ret, WSAGetLastError());
    for (i = 0; i < 30 && !g_Completions; i++)
        SleepEx(100, TRUE);
    ok(g_Completions == 1, "%lu buffers: the completion routine ran %d times\n", Count, g_Completions);
    if (g_Completions != 1)
        return FALSE;
    ok(g_CompletionError == 0, "Completion error %lu\n", g_CompletionError);
    ok(g_CompletionBytes == (DWORD)ExpectedLength, "%lu buffers: completion bytes %lu\n", Count, g_CompletionBytes);
    sprintf(What, "%lu buffers, overlapped routine", Count);
    ExpectDatagram(What, Expected, ExpectedLength);
    return TRUE;
}

/* Overlapped storage that cannot be written is refused before anything is sent, without keeping resources of the provider */
static void TestBadOverlapped(SOCKET Sender)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[2];
    WSAMSG Msg;
    void *ReadOnly = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    void *NoAccess = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    LPWSAOVERLAPPED Cases[3];
    static const char *Names[3] = { "bad pointer", "read-only page", "inaccessible page" };
    DWORD Handles, HandlesAfter, Sent;
    unsigned i, Count, Round;
    INT Ret;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;
    Cases[0] = (LPWSAOVERLAPPED)(ULONG_PTR)0x10;
    Cases[1] = ReadOnly;
    Cases[2] = NoAccess;

    for (i = 0; i < ARRAYSIZE(Cases); i++)
    {
        for (Count = 1; Count <= 2; Count++)
        {
            for (Round = 0; Round < 2; Round++)
            {
                char What[64];
                int k;

                InitMessage(&Msg, Buffers, Count);
                if (!GetProcessHandleCount(GetCurrentProcess(), &Handles))
                    Handles = 0;
                for (k = 0; k < 20; k++)
                {
                    Sent = NOT_SET;
                    WSASetLastError(0);
                    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, Cases[i], Round ? Completion : NULL);
                    if (Ret != SOCKET_ERROR || WSAGetLastError() != WSAEFAULT || Sent != NOT_SET)
                        break;
                }
                sprintf(What, "%s, %u buffers%s", Names[i], Count, Round ? ", routine" : "");
                CheckFailure(What, Ret, WSAGetLastError(), WSAEFAULT, Sent);
                if (GetProcessHandleCount(GetCurrentProcess(), &HandlesAfter))
                    ok(HandlesAfter <= Handles, "%s: %lu handles before, %lu after\n", What, Handles, HandlesAfter);
            }
        }
    }
    ExpectNothing("after the bad overlapped storage");
    if (ReadOnly)
        VirtualFree(ReadOnly, 0, MEM_RELEASE);
    if (NoAccess)
        VirtualFree(NoAccess, 0, MEM_RELEASE);
}

static BOOL IsWow64(void)
{
    typedef BOOL (WINAPI *FN_IsWow64Process)(HANDLE, PBOOL);
    FN_IsWow64Process pIsWow64Process = (FN_IsWow64Process)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    BOOL Wow64 = FALSE;

    return pIsWow64Process && pIsWow64Process(GetCurrentProcess(), &Wow64) && Wow64;
}

/* A socket that was not opened as overlapped sends at once, and the overlapped structure is completed as for an overlapped one */
static void TestNonOverlappedSocket(void)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[2];
    WSAMSG Msg;
    SOCKET Plain = WSASocketW(AF_INET, SOCK_DGRAM, 0, NULL, 0, 0);
    static WSAOVERLAPPED Overlapped;
    DWORD Sent;
    INT Ret;
    unsigned Count, Mode;
    int i;

    ok(Plain != INVALID_SOCKET, "WSASocket failed: %d\n", WSAGetLastError());
    if (Plain == INVALID_SOCKET)
        return;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;

    for (Count = 1; Count <= 2; Count++)
    {
        /* Mode 0: no event, 1: an event, 2: a completion routine */
        for (Mode = 0; Mode < 3; Mode++)
        {
            char What[64];
            int Expected = Count == 1 ? 5 : 10;

            sprintf(What, "non-overlapped socket, %u buffers, mode %u", Count, Mode);
            InitMessage(&Msg, Buffers, Count);
            ZeroMemory(&Overlapped, sizeof(Overlapped));
            Overlapped.Internal = 0xCCCCCCCC;
            Overlapped.hEvent = Mode == 1 ? WSACreateEvent() : NULL;
            g_Completions = 0;
            Sent = NOT_SET;
            WSASetLastError(0);
            Ret = pWSASendMsg(Plain, &Msg, 0, &Sent, &Overlapped, Mode == 2 ? Completion : NULL);
            ok(Ret == 0, "%s: returned %d, error %d\n", What, Ret, WSAGetLastError());
            ok(Sent == (DWORD)Expected, "%s: sent %lu bytes\n", What, Sent);
            /* the status is the low half: Windows Server 2008 R2 x64 leaves the other half of the field as it likes after two buffers */
            ok((ULONG)Overlapped.Internal == 0, "%s: Internal is 0x%I64x\n", What, (ULONGLONG)Overlapped.Internal);
            ok(Overlapped.InternalHigh == (ULONG_PTR)Expected, "%s: InternalHigh is %lu\n", What, (ULONG)Overlapped.InternalHigh);
            if (Mode == 1)
            {
                ok(WaitForSingleObject(Overlapped.hEvent, 0) == WAIT_OBJECT_0, "%s: the event is not set\n", What);
                WSACloseEvent(Overlapped.hEvent);
            }
            for (i = 0; i < 10 && Mode == 2 && !g_Completions; i++)
                SleepEx(50, TRUE);
            ok(g_Completions == (Mode == 2 ? 1 : 0), "%s: the completion routine ran %d times\n", What, g_Completions);
            if (Mode == 2 && g_Completions == 1)
            {
                /* A 32-bit process on 64-bit Windows is handed 0 bytes by this completion (measured on Windows Server 2008 R2 and Windows 10) */
                ok(g_CompletionError == 0 && (IsWow64() || g_CompletionBytes == (DWORD)Expected), "%s: completion error %lu, bytes %lu\n",
                   What, g_CompletionError, g_CompletionBytes);
            }
            ExpectDatagram(What, Count == 1 ? "hello" : "helloworld", Expected);
        }
    }

    closesocket(Plain);
}

static void TestOverlapped(void)
{
    static char Hello[] = "hello", World[] = "world";
    WSABUF Buffers[2];
    WSAMSG Msg;
    static WSAOVERLAPPED Overlapped;
    SOCKET Sender;
    DWORD Sent;
    INT Ret;

    Buffers[0].buf = Hello; Buffers[0].len = 5;
    Buffers[1].buf = World; Buffers[1].len = 5;

    Sender = WSASocketW(AF_INET, SOCK_DGRAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(Sender != INVALID_SOCKET, "WSASocket failed: %d\n", WSAGetLastError());
    if (Sender == INVALID_SOCKET)
        return;

    /* One buffer goes to the provider's own overlapped path */
    if (!OverlappedRounds(Sender, Buffers, 1, "hello", 5))
    {
        closesocket(Sender);
        return;
    }

    if (is_reactos())
    {
        /* AFD sends only the first buffer of a datagram; an overlapped send of several is refused rather than cut short */
        InitMessage(&Msg, Buffers, 2);
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Sent = NOT_SET;
        WSASetLastError(0);
        Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, &Overlapped, NULL);
        ok(Ret == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP, "Two buffers, overlapped: returned %d, error %d\n",
           Ret, WSAGetLastError());
        skip("An overlapped send of several buffers is not supported\n");
    }
    else if (!OverlappedRounds(Sender, Buffers, 2, "helloworld", 10))
    {
        closesocket(Sender);
        return;
    }

    TestBadOverlapped(Sender);
    TestNonOverlappedSocket();

    /* A routine without an overlapped structure is ignored */
    InitMessage(&Msg, Buffers, 2);
    g_Completions = 0;
    Sent = NOT_SET;
    Ret = pWSASendMsg(Sender, &Msg, 0, &Sent, NULL, Completion);
    ok(Ret == 0, "Routine only: returned %d, error %d\n", Ret, WSAGetLastError());
    ok(Sent == 10, "Routine only: sent %lu\n", Sent);
    SleepEx(100, TRUE);
    ok(g_Completions == 0, "The completion routine ran\n");
    ExpectDatagram("routine only", "helloworld", 10);

    closesocket(Sender);
}

START_TEST(WSASendMsg)
{
    WSADATA Data;
    SOCKET Sender;
    int Length = sizeof(g_ReceiverAddress);
    INT Ret;
    WSAMSG Msg;
    DWORD Sent = NOT_SET;

    pWSASendMsg = (FN_WSASendMsg)GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), "WSASendMsg");
    if (!pWSASendMsg)
    {
        skip("WSASendMsg is not available\n");
        return;
    }

    /* Before WSAStartup */
    ZeroMemory(&Msg, sizeof(Msg));
    WSASetLastError(0);
    Ret = pWSASendMsg(INVALID_SOCKET, &Msg, 0, &Sent, NULL, NULL);
    CheckFailure("before WSAStartup", Ret, WSAGetLastError(), WSANOTINITIALISED, Sent);

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");

    g_Receiver = socket(AF_INET, SOCK_DGRAM, 0);
    ok(g_Receiver != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    ZeroMemory(&g_ReceiverAddress, sizeof(g_ReceiverAddress));
    g_ReceiverAddress.sin_family = AF_INET;
    g_ReceiverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(g_Receiver, (struct sockaddr *)&g_ReceiverAddress, sizeof(g_ReceiverAddress)) == 0, "bind failed: %d\n", WSAGetLastError());
    ok(getsockname(g_Receiver, (struct sockaddr *)&g_ReceiverAddress, &Length) == 0, "getsockname failed: %d\n", WSAGetLastError());

    Sender = socket(AF_INET, SOCK_DGRAM, 0);
    ok(Sender != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    if (Sender == INVALID_SOCKET || g_Receiver == INVALID_SOCKET)
    {
        WSACleanup();
        return;
    }

    TestData(Sender);
    TestArguments(Sender);
    TestName(Sender);
    TestControl(Sender);
    TestTooBig(Sender);
    TestConnected();
    TestOverlapped();

    closesocket(Sender);
    closesocket(g_Receiver);
    WSACleanup();
}
