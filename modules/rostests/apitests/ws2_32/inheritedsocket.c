/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     A child process uses the socket it inherited through CreateProcess, as OpenSSH's sshd-session
 *              does with its connection: the socket calls must work on a handle this process did not create
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does. The child is this program, started with the test
 * name inheritedsocketchild and the socket handle in the environment; it reports through its exit code (a bit per failed check).
 */

#include "ws2_32.h"
#include <winuser.h>

#define CHECK_GETSOCKOPT 0x01
#define CHECK_GETPEERNAME 0x02
#define CHECK_SEND 0x04
#define CHECK_RECV 0x08
#define CHECK_FIONBIO 0x10
#define CHECK_WOULDBLOCK 0x20
#define CHECK_EVENTSELECT 0x40
#define CHECK_CLOSE 0x80
#define CHECK_STARTUP 0x100

/* The child: argv[2] is the handle of the connected socket */
START_TEST(inheritedsocketchild)
{
    WCHAR Text[32];
    WSADATA Data;
    SOCKET s;
    int Type = 0, Length = sizeof(Type), Result;
    struct sockaddr_in Peer;
    char Buffer[16];
    u_long NonBlocking = 1;
    WSAEVENT Event;
    DWORD Failed = 0;
    HANDLE Go;

    /* The handle comes in the environment: the test runner takes the test name as its only argument */
    if (!GetEnvironmentVariableW(L"WS2_32_INHERITED_SOCKET", Text, RTL_NUMBER_OF(Text)))
        ExitProcess(CHECK_STARTUP);
    s = (SOCKET)(ULONG_PTR)wcstoul(Text, NULL, 16);
    /* The parent closes its own handle of the connection, then signals: the connection must outlive that */
    if (GetEnvironmentVariableW(L"WS2_32_INHERITED_GO", Text, RTL_NUMBER_OF(Text)))
    {
        Go = (HANDLE)(ULONG_PTR)wcstoul(Text, NULL, 16);
        if (WaitForSingleObject(Go, 30000) != WAIT_OBJECT_0)
            ExitProcess(CHECK_STARTUP);
    }
    if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
        ExitProcess(CHECK_STARTUP);

    Result = getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&Type, &Length);
    if (Result != 0 || Type != SOCK_STREAM)
        Failed |= CHECK_GETSOCKOPT;

    Length = sizeof(Peer);
    Result = getpeername(s, (struct sockaddr *)&Peer, &Length);
    if (Result != 0 || Peer.sin_family != AF_INET || Peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        Failed |= CHECK_GETPEERNAME;

    if (send(s, "hello", 5, 0) != 5)
        Failed |= CHECK_SEND;
    if (recv(s, Buffer, sizeof(Buffer), 0) != 1 || Buffer[0] != 'x')
        Failed |= CHECK_RECV;

    Event = WSACreateEvent();
    if (WSAEventSelect(s, Event, FD_READ | FD_CLOSE) != 0)
        Failed |= CHECK_EVENTSELECT;
    if (ioctlsocket(s, FIONBIO, &NonBlocking) != 0)
        Failed |= CHECK_FIONBIO;
    if (recv(s, Buffer, sizeof(Buffer), 0) != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK)
        Failed |= CHECK_WOULDBLOCK;

    if (closesocket(s) != 0)
        Failed |= CHECK_CLOSE;
    WSACleanup();
    ExitProcess(Failed);
}

START_TEST(inheritedsocket)
{
    WSADATA Data;
    SOCKET Listener, Client, Server;
    struct sockaddr_in Addr;
    int Length = sizeof(Addr);
    WCHAR Image[MAX_PATH], Command[MAX_PATH + 64], Text[32];
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    char Buffer[16];
    DWORD Exit = 0xdead;
    int Received, Timeout;
    SECURITY_ATTRIBUTES Inheritable = {sizeof(Inheritable), NULL, TRUE};
    HANDLE Go;

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");
    Listener = socket(AF_INET, SOCK_STREAM, 0);
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed: %d\n", WSAGetLastError());
    ok(getsockname(Listener, (struct sockaddr *)&Addr, &Length) == 0, "getsockname failed\n");
    ok(listen(Listener, 1) == 0, "listen failed\n");
    Client = socket(AF_INET, SOCK_STREAM, 0);
    ok(connect(Client, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "connect failed: %d\n", WSAGetLastError());
    Server = accept(Listener, NULL, NULL);
    ok(Server != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    if (Server == INVALID_SOCKET)
        return;

    GetModuleFileNameW(NULL, Image, RTL_NUMBER_OF(Image));
    wsprintfW(Command, L"\"%s\" inheritedsocketchild", Image);
    wsprintfW(Text, L"%lx", (ULONG)(ULONG_PTR)Server);
    SetEnvironmentVariableW(L"WS2_32_INHERITED_SOCKET", Text);
    Go = CreateEventW(&Inheritable, TRUE, FALSE, NULL);
    ok(Go != NULL, "CreateEvent failed: %lu\n", GetLastError());
    wsprintfW(Text, L"%lx", (ULONG)(ULONG_PTR)Go);
    SetEnvironmentVariableW(L"WS2_32_INHERITED_GO", Text);
    ZeroMemory(&Si, sizeof(Si));
    ZeroMemory(&Pi, sizeof(Pi));
    Si.cb = sizeof(Si);
    ok(CreateProcessW(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Si, &Pi),
       "CreateProcess failed: %lu\n", GetLastError());
    SetEnvironmentVariableW(L"WS2_32_INHERITED_GO", NULL);
    if (Pi.hProcess)
    {
        /* Give up this process's handle first: the child owns the connection now */
        ok(closesocket(Server) == 0, "closesocket failed: %d\n", WSAGetLastError());
        Server = INVALID_SOCKET;
        SetEvent(Go);
        /* The child sends hello, then waits for one byte from us. A child that cannot use the socket
         * sends nothing: do not wait for it forever. */
        Timeout = 20000;
        setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Timeout, sizeof(Timeout));
        Received = recv(Client, Buffer, sizeof(Buffer), 0);
        ok(Received == 5 && !memcmp(Buffer, "hello", 5), "received %d bytes from the child\n", Received);
        ok(send(Client, "x", 1, 0) == 1, "send failed: %d\n", WSAGetLastError());
        ok(WaitForSingleObject(Pi.hProcess, 60000) == WAIT_OBJECT_0, "the child did not finish\n");
        GetExitCodeProcess(Pi.hProcess, &Exit);
        ok(Exit == 0, "the child's checks failed: 0x%lx (1 getsockopt, 2 getpeername, 4 send, 8 recv, "
           "0x10 FIONBIO, 0x20 would block, 0x40 WSAEventSelect, 0x80 closesocket, 0x100 startup)\n", Exit);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
    }
    if (Server != INVALID_SOCKET)
        closesocket(Server);
    closesocket(Client);
    closesocket(Listener);
    WSACleanup();
}
