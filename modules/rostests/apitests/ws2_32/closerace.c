/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Close a TCP socket while the transport completes its receive
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

#define CLOSERACE_THREADS 4
#define CLOSERACE_SECONDS 20
#define CLOSERACE_MAX_ROUNDS 4000

typedef struct _CLOSERACE_RESULT
{
    LONG Rounds;
    LONG Failures;
    int LastError;
    const char *FailedCall;
} CLOSERACE_RESULT, *PCLOSERACE_RESULT;

/* Static: a thread that never finishes must not write to a returned stack */
static CLOSERACE_RESULT Results[CLOSERACE_THREADS];

static
void
Fail(PCLOSERACE_RESULT Result, const char *Call, int Error)
{
    Result->Failures++;
    Result->LastError = Error;
    Result->FailedCall = Call;
}

/* Each round: the client sends, so the transport takes the server's buffered
 * receive and completes it from a work item, while the server socket closes */
static
DWORD
WINAPI
CloseRaceThread(LPVOID Parameter)
{
    PCLOSERACE_RESULT Result = Parameter;
    SOCKET Listener, Client, Server;
    struct sockaddr_in Addr;
    int Len = sizeof(Addr);
    struct linger Linger;
    char Data[16] = "close race data";
    int Sent;
    DWORD Start = GetTickCount();
    ULONG Spin, i;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Listener == INVALID_SOCKET)
    {
        Fail(Result, "socket", WSAGetLastError());
        return 0;
    }

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR ||
        listen(Listener, 4) == SOCKET_ERROR ||
        getsockname(Listener, (struct sockaddr *)&Addr, &Len) == SOCKET_ERROR)
    {
        Fail(Result, "listen", WSAGetLastError());
        closesocket(Listener);
        return 0;
    }

    while (Result->Rounds < CLOSERACE_MAX_ROUNDS &&
           GetTickCount() - Start < CLOSERACE_SECONDS * 1000)
    {
        Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (Client == INVALID_SOCKET)
        {
            Fail(Result, "socket", WSAGetLastError());
            break;
        }

        if (connect(Client, (struct sockaddr *)&Addr, sizeof(Addr)) == SOCKET_ERROR)
        {
            Fail(Result, "connect", WSAGetLastError());
            closesocket(Client);
            break;
        }

        Server = accept(Listener, NULL, NULL);
        if (Server == INVALID_SOCKET)
        {
            Fail(Result, "accept", WSAGetLastError());
            closesocket(Client);
            break;
        }

        /* Odd rounds close the server abortively, even rounds gracefully */
        if (Result->Rounds & 1)
        {
            Linger.l_onoff = 1;
            Linger.l_linger = 0;
            if (setsockopt(Server, SOL_SOCKET, SO_LINGER, (char *)&Linger, sizeof(Linger)) == SOCKET_ERROR)
                Fail(Result, "setsockopt", WSAGetLastError());
        }

        Sent = send(Client, Data, sizeof(Data), 0);
        if (Sent != sizeof(Data))
            Fail(Result, "send", Sent == SOCKET_ERROR ? WSAGetLastError() : Sent);

        /* Move the close across the transport's completion */
        Spin = (Result->Rounds * 7919) % 2048;
        for (i = 0; i < Spin; i++)
            YieldProcessor();

        if (closesocket(Server) == SOCKET_ERROR)
            Fail(Result, "closesocket(server)", WSAGetLastError());

        /* The server closed with unread data, which resets the connection;
         * ReactOS reports that reset when the client closes */
        if (closesocket(Client) == SOCKET_ERROR && WSAGetLastError() != WSAECONNRESET)
            Fail(Result, "closesocket(client)", WSAGetLastError());
        Result->Rounds++;
    }

    if (closesocket(Listener) == SOCKET_ERROR)
        Fail(Result, "closesocket(listener)", WSAGetLastError());
    return 0;
}

START_TEST(closerace)
{
    WSADATA WsaData;
    HANDLE Threads[CLOSERACE_THREADS];
    LONG Rounds = 0;
    DWORD Wait = WAIT_FAILED;
    ULONG i, Started = 0;
    int Error;

    Error = WSAStartup(MAKEWORD(2, 2), &WsaData);
    if (Error)
    {
        skip("WSAStartup failed: %d\n", Error);
        return;
    }

    for (i = 0; i < CLOSERACE_THREADS; i++)
    {
        Threads[i] = CreateThread(NULL, 0, CloseRaceThread, &Results[i], 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!Threads[i])
            break;
        Started++;
    }

    if (Started)
    {
        Wait = WaitForMultipleObjects(Started, Threads, TRUE, (CLOSERACE_SECONDS + 60) * 1000);
        ok(Wait == WAIT_OBJECT_0, "The threads did not finish: %lu\n", Wait);
    }

    /* Threads still running keep their sockets; the process exit ends them */
    if (Wait != WAIT_OBJECT_0)
        return;

    for (i = 0; i < Started; i++)
    {
        ok(Results[i].Failures == 0, "Thread %lu: %ld failures, last %s error %d\n",
           i, Results[i].Failures, Results[i].FailedCall ? Results[i].FailedCall : "-", Results[i].LastError);
        ok(Results[i].Rounds > 0, "Thread %lu completed no round\n", i);
        Rounds += Results[i].Rounds;
        CloseHandle(Threads[i]);
    }

    trace("%ld rounds in %lu threads\n", Rounds, Started);

    WSACleanup();
}
