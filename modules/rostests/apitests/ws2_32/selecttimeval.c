/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for select() and WSAPoll time-out values (negative, overflowing, tv_usec >= 1000000)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Windows computes the select() time-out as the signed 64-bit sum tv_sec * 1000000 + tv_usec
 * microseconds and never rejects it: a sum <= 0 does not wait (curl's "no time-out" value
 * {-1, 807000} is such a sum), and a large sum waits for practically ever. Every case runs on a
 * connected loopback pair; a helper thread makes the selected socket readable after
 * DATA_DELAY ms, which bounds the test if a time-out is wrongly treated as infinite.
 */

#include "ws2_32.h"
#include <limits.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef POLLRDNORM
#define POLLRDNORM  0x0100
#define POLLRDBAND  0x0200
#define POLLIN      (POLLRDNORM | POLLRDBAND)
#define POLLPRI     0x0400
#define POLLWRNORM  0x0010
#define POLLOUT     (POLLWRNORM)
#define POLLWRBAND  0x0020
#define POLLERR     0x0001
#define POLLHUP     0x0002
#define POLLNVAL    0x0004
typedef struct pollfd
{
    SOCKET fd;
    SHORT events;
    SHORT revents;
} WSAPOLLFD, *PWSAPOLLFD, *LPWSAPOLLFD;
#endif

#define DATA_DELAY   2000   /* the helper makes the socket readable after this long */
#define EARLY_MAX    1000   /* "returns at once" must take less than this */
#define SLACK_BELOW  100    /* timer granularity, plus GetTickCount's */

typedef enum
{
    Immediate,  /* sum <= 0: polls, returns 0 at once */
    Short,      /* waits Ms (< DATA_DELAY) and returns 0 */
    Long        /* waits until the data arrives (DATA_DELAY) and returns 1 */
} EXPECT;

typedef struct
{
    const char *Name;
    long Sec;
    long Usec;
    EXPECT Expect;
    DWORD Ms;
} CASE;

static const CASE Cases[] =
{
    { "{0,0}",                    0,           0,          Immediate, 0 },
    { "{0,1000}",                 0,           1000,       Immediate, 0 }, /* 1 ms: below the timer tick */
    { "{-1,807000}",              -1,          807000,     Immediate, 0 }, /* curl: no time-out */
    { "{-1,0}",                   -1,          0,          Immediate, 0 },
    { "{-1,1000000}",             -1,          1000000,    Immediate, 0 },
    { "{-1511828489,807000}",     -1511828489, 807000,     Immediate, 0 }, /* (2^63-1)/1000 as a long */
    { "{INT_MIN,0}",              INT_MIN,     0,          Immediate, 0 },
    { "{INT_MIN,807000}",         INT_MIN,     807000,     Immediate, 0 },
    { "{0,-1}",                   0,           -1,         Immediate, 0 },
    { "{0,-1000000}",             0,           -1000000,   Immediate, 0 },
    { "{1,-2000000}",             1,           -2000000,   Immediate, 0 },
    { "{0,INT_MIN}",              0,           INT_MIN,    Immediate, 0 },
    { "{2,INT_MIN}",              2,           INT_MIN,    Immediate, 0 },
    { "{0,700000}",               0,           700000,     Short,     700 },
    { "{1,0}",                    1,           0,          Short,     1000 },
    { "{0,1000000}",              0,           1000000,    Short,     1000 }, /* tv_usec >= 1 s adds up */
    { "{-1,1700000}",             -1,          1700000,    Short,     700 },
    { "{1,-300000}",              1,           -300000,    Short,     700 },
    { "{2,-1000000}",             2,           -1000000,   Short,     1000 },
    { "{0,2000000}",              0,           2000000,    Long,      0 },
    { "{0,INT_MAX}",              0,           INT_MAX,    Long,      0 },
    { "{5,-1}",                   5,           -1,         Long,      0 },
    { "{2148,0}",                 2148,        0,          Long,      0 },
    { "{214748,0}",               214748,      0,          Long,      0 },
    { "{4294967,0}",              4294967,     0,          Long,      0 },
    { "{INT_MAX,0}",              INT_MAX,     0,          Long,      0 },
    { "{INT_MAX,807000}",         INT_MAX,     807000,     Long,      0 },
    { "{INT_MAX,999999}",         INT_MAX,     999999,     Long,      0 },
    { "{INT_MAX,INT_MAX}",        INT_MAX,     INT_MAX,    Long,      0 },
};

typedef struct
{
    SOCKET Peer;
    HANDLE Cancel;
} HELPER;

static
DWORD
WINAPI
HelperThread(LPVOID Context)
{
    HELPER *Helper = Context;

    if (WaitForSingleObject(Helper->Cancel, DATA_DELAY) == WAIT_TIMEOUT)
        send(Helper->Peer, "x", 1, 0);
    return 0;
}

static
void
SocketPair(SOCKET *Client, SOCKET *Server)
{
    SOCKET Listener;
    struct sockaddr_in Addr;
    int Len = sizeof(Addr);
    int Ret;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(Listener != INVALID_SOCKET && *Client != INVALID_SOCKET, "socket failed, %d\n", WSAGetLastError());
    memset(&Addr, 0, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Ret = bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Ret == 0, "bind returned %d, %d\n", Ret, WSAGetLastError());
    Ret = getsockname(Listener, (struct sockaddr *)&Addr, &Len);
    ok(Ret == 0, "getsockname returned %d, %d\n", Ret, WSAGetLastError());
    Ret = listen(Listener, 1);
    ok(Ret == 0, "listen returned %d, %d\n", Ret, WSAGetLastError());
    Ret = connect(*Client, (struct sockaddr *)&Addr, sizeof(Addr));
    ok(Ret == 0, "connect returned %d, %d\n", Ret, WSAGetLastError());
    *Server = accept(Listener, NULL, NULL);
    ok(*Server != INVALID_SOCKET, "accept failed, %d\n", WSAGetLastError());
    closesocket(Listener);
}

/* Consume what the helper sent, if it did */
static
void
Drain(SOCKET Socket)
{
    u_long Avail = 0;
    char Byte;

    Sleep(20);
    while (ioctlsocket(Socket, FIONREAD, &Avail) == 0 && Avail != 0)
        recv(Socket, &Byte, 1, 0);
}

static
void
CheckTiming(const char *Name, EXPECT Expect, DWORD Ms, int Ret, int Error, DWORD Elapsed)
{
    ok(Ret != SOCKET_ERROR, "%s: failed with error %d\n", Name, Error);
    if (Ret == SOCKET_ERROR)
        return;

    switch (Expect)
    {
        case Immediate:
            ok(Ret == 0, "%s: returned %d, expected a poll that finds nothing\n", Name, Ret);
            ok(Elapsed < EARLY_MAX, "%s: waited %lu ms, expected no wait\n", Name, Elapsed);
            break;
        case Short:
            ok(Ret == 0, "%s: returned %d, expected a time-out\n", Name, Ret);
            ok(Elapsed + SLACK_BELOW >= Ms, "%s: waited only %lu ms, expected %lu\n", Name, Elapsed, Ms);
            break;
        case Long:
            ok(Ret == 1, "%s: returned %d, expected 1 (data after %d ms)\n", Name, Ret, DATA_DELAY);
            ok(Elapsed + SLACK_BELOW >= DATA_DELAY, "%s: returned after %lu ms, before the data\n", Name, Elapsed);
            break;
    }
}

static
void
Test_NotReadable(void)
{
    SOCKET Client, Server;
    unsigned i;

    SocketPair(&Client, &Server);

    for (i = 0; i < ARRAY_SIZE(Cases) + 1; i++)
    {
        const CASE *Case = i < ARRAY_SIZE(Cases) ? &Cases[i] : NULL;
        struct timeval Tv;
        HELPER Helper;
        HANDLE Thread;
        fd_set Set;
        DWORD Start, Elapsed;
        int Ret, Error;

        Helper.Peer = Server;
        Helper.Cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
        Thread = CreateThread(NULL, 0, HelperThread, &Helper, 0, NULL);
        FD_ZERO(&Set);
        FD_SET(Client, &Set);
        if (Case)
        {
            Tv.tv_sec = Case->Sec;
            Tv.tv_usec = Case->Usec;
        }
        Sleep(30);

        Start = GetTickCount();
        Ret = select(0, &Set, NULL, NULL, Case ? &Tv : NULL);
        Error = Ret == SOCKET_ERROR ? WSAGetLastError() : 0;
        Elapsed = GetTickCount() - Start;

        if (Case)
            CheckTiming(Case->Name, Case->Expect, Case->Ms, Ret, Error, Elapsed);
        else
            CheckTiming("NULL", Long, 0, Ret, Error, Elapsed);
        if (Ret == 1)
            ok(FD_ISSET(Client, &Set), "%s: socket not in the set\n", Case ? Case->Name : "NULL");

        SetEvent(Helper.Cancel);
        WaitForSingleObject(Thread, INFINITE);
        CloseHandle(Thread);
        CloseHandle(Helper.Cancel);
        Drain(Client);
    }

    closesocket(Client);
    closesocket(Server);
}

static
void
Test_Ready(void)
{
    SOCKET Client, Server;
    unsigned i;

    SocketPair(&Client, &Server);

    /* Readable: every time-out finds the socket at once, with or without a valid value */
    ok(send(Server, "x", 1, 0) == 1, "send failed, %d\n", WSAGetLastError());
    Sleep(30);
    for (i = 0; i < ARRAY_SIZE(Cases); i++)
    {
        struct timeval Tv;
        fd_set Set;
        DWORD Start, Elapsed;
        int Ret, Error;

        Tv.tv_sec = Cases[i].Sec;
        Tv.tv_usec = Cases[i].Usec;
        FD_ZERO(&Set);
        FD_SET(Client, &Set);
        Start = GetTickCount();
        Ret = select(0, &Set, NULL, NULL, &Tv);
        Error = Ret == SOCKET_ERROR ? WSAGetLastError() : 0;
        Elapsed = GetTickCount() - Start;
        ok(Ret == 1, "readable %s: returned %d, error %d\n", Cases[i].Name, Ret, Error);
        ok(Elapsed < EARLY_MAX, "readable %s: took %lu ms\n", Cases[i].Name, Elapsed);
    }
    Drain(Client);

    /* Writable, which is what curl asks before it sends */
    for (i = 0; i < ARRAY_SIZE(Cases); i++)
    {
        struct timeval Tv;
        fd_set Set;
        DWORD Start, Elapsed;
        int Ret, Error;

        Tv.tv_sec = Cases[i].Sec;
        Tv.tv_usec = Cases[i].Usec;
        FD_ZERO(&Set);
        FD_SET(Client, &Set);
        Start = GetTickCount();
        Ret = select(0, NULL, &Set, NULL, &Tv);
        Error = Ret == SOCKET_ERROR ? WSAGetLastError() : 0;
        Elapsed = GetTickCount() - Start;
        ok(Ret == 1, "writable %s: returned %d, error %d\n", Cases[i].Name, Ret, Error);
        ok(Elapsed < EARLY_MAX, "writable %s: took %lu ms\n", Cases[i].Name, Elapsed);
    }

    closesocket(Client);
    closesocket(Server);
}

static
void
Test_Poll(void)
{
    static const struct { INT Timeout; EXPECT Expect; } PollCases[] =
    {
        { 0,        Immediate },
        { -1,       Long },
        { -2,       Long },
        { INT_MIN,  Long },
        { INT_MAX,  Long },
    };
    int (WINAPI *pWSAPoll)(WSAPOLLFD *, ULONG, INT);
    SOCKET Client, Server;
    unsigned i;

    pWSAPoll = (void *)GetProcAddress(GetModuleHandleW(L"ws2_32.dll"), "WSAPoll");
    if (!pWSAPoll)
    {
        skip("WSAPoll is not available\n");
        return;
    }

    SocketPair(&Client, &Server);

    for (i = 0; i < ARRAY_SIZE(PollCases); i++)
    {
        WSAPOLLFD Fd;
        HELPER Helper;
        HANDLE Thread;
        DWORD Start, Elapsed;
        int Ret, Error;
        char Name[32];

        sprintf(Name, "WSAPoll(%d)", PollCases[i].Timeout);
        Helper.Peer = Server;
        Helper.Cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
        Thread = CreateThread(NULL, 0, HelperThread, &Helper, 0, NULL);
        Fd.fd = Client;
        Fd.events = POLLRDNORM;
        Fd.revents = 0;
        Sleep(30);

        Start = GetTickCount();
        Ret = pWSAPoll(&Fd, 1, PollCases[i].Timeout);
        Error = Ret == SOCKET_ERROR ? WSAGetLastError() : 0;
        Elapsed = GetTickCount() - Start;

        CheckTiming(Name, PollCases[i].Expect, 0, Ret, Error, Elapsed);
        if (Ret == 1)
            ok(Fd.revents == POLLRDNORM, "%s: revents %x\n", Name, Fd.revents);

        SetEvent(Helper.Cancel);
        WaitForSingleObject(Thread, INFINITE);
        CloseHandle(Thread);
        CloseHandle(Helper.Cancel);
        Drain(Client);
    }

    closesocket(Client);
    closesocket(Server);
}

START_TEST(selecttimeval)
{
    WSADATA Data;

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");

    Test_NotReadable();
    Test_Ready();
    Test_Poll();

    WSACleanup();
}
