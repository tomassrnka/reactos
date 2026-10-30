/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for named pipe instance counting after the server end closed
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * A server handle that is closed while its client is still open no longer takes up
 * an instance (MaximumInstances counts open server handles), and the pipe name is
 * free for a new, independent pipe. Expected values were measured on Windows 10.
 */

#include "precomp.h"

static WCHAR g_PipeName[64];
static ULONG g_Counter;

static
VOID
NewName(VOID)
{
    _snwprintf(g_PipeName, _countof(g_PipeName), L"\\\\.\\pipe\\rostest_instreuse_%lu_%lu",
               GetCurrentProcessId(), ++g_Counter);
}

static
HANDLE
CreateServer(DWORD dwMaxInstances, DWORD dwType, DWORD *pdwError)
{
    HANDLE hPipe;

    SetLastError(0xdeadbeef);
    hPipe = CreateNamedPipeW(g_PipeName, PIPE_ACCESS_DUPLEX, dwType | PIPE_WAIT,
                             dwMaxInstances, 256, 256, 0, NULL);
    *pdwError = (hPipe == INVALID_HANDLE_VALUE) ? GetLastError() : ERROR_SUCCESS;
    return hPipe;
}

/* Create a server that has to succeed */
static
HANDLE
MustCreate(DWORD dwMax, DWORD dwType, PCSTR Where)
{
    DWORD dwError;
    HANDLE hPipe = CreateServer(dwMax, dwType, &dwError);

    ok(hPipe != INVALID_HANDLE_VALUE, "%s: CreateNamedPipeW(max %lu) failed, error %lu\n",
       Where, dwMax, dwError);
    return hPipe;
}

/* Create a server that has to fail with ERROR_PIPE_BUSY */
static
VOID
MustBeBusy(DWORD dwMax, PCSTR Where)
{
    DWORD dwError;
    HANDLE hPipe = CreateServer(dwMax, PIPE_TYPE_BYTE, &dwError);

    ok(hPipe == INVALID_HANDLE_VALUE, "%s: CreateNamedPipeW(max %lu) succeeded\n", Where, dwMax);
    if (hPipe != INVALID_HANDLE_VALUE)
        CloseHandle(hPipe);
    else
        ok(dwError == ERROR_PIPE_BUSY, "%s: error %lu, expected %lu\n", Where, dwError, (DWORD)ERROR_PIPE_BUSY);
}

static
HANDLE
OpenClient(PCSTR Where)
{
    HANDLE hClient = CreateFileW(g_PipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                 OPEN_EXISTING, 0, NULL);
    ok(hClient != INVALID_HANDLE_VALUE, "%s: CreateFileW failed, error %lu\n", Where, GetLastError());
    return hClient;
}

static
VOID
ConnectServer(HANDLE hServer, PCSTR Where)
{
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = ConnectNamedPipe(hServer, NULL);
    ok(!Ret, "%s: ConnectNamedPipe succeeded\n", Where);
    ok(GetLastError() == ERROR_PIPE_CONNECTED, "%s: error %lu, expected %lu\n",
       Where, GetLastError(), (DWORD)ERROR_PIPE_CONNECTED);
}

/* A pipe whose other end is gone: write is ERROR_NO_DATA, read and peek ERROR_BROKEN_PIPE */
static
VOID
ExpectBroken(HANDLE hPipe, PCSTR Where)
{
    CHAR Byte = 'x';
    DWORD cb, cbAvail;
    BOOL Ret;

    if (hPipe == INVALID_HANDLE_VALUE)
        return;

    SetLastError(0xdeadbeef);
    cb = 0xdeadbeef;
    Ret = WriteFile(hPipe, &Byte, 1, &cb, NULL);
    ok(!Ret, "%s: WriteFile succeeded\n", Where);
    ok(GetLastError() == ERROR_NO_DATA, "%s: WriteFile error %lu, expected %lu\n",
       Where, GetLastError(), (DWORD)ERROR_NO_DATA);

    SetLastError(0xdeadbeef);
    Ret = ReadFile(hPipe, &Byte, 1, &cb, NULL);
    ok(!Ret, "%s: ReadFile succeeded\n", Where);
    ok(GetLastError() == ERROR_BROKEN_PIPE, "%s: ReadFile error %lu, expected %lu\n",
       Where, GetLastError(), (DWORD)ERROR_BROKEN_PIPE);

    SetLastError(0xdeadbeef);
    Ret = PeekNamedPipe(hPipe, NULL, 0, NULL, &cbAvail, NULL);
    ok(!Ret, "%s: PeekNamedPipe succeeded\n", Where);
    ok(GetLastError() == ERROR_BROKEN_PIPE, "%s: PeekNamedPipe error %lu, expected %lu\n",
       Where, GetLastError(), (DWORD)ERROR_BROKEN_PIPE);
}

static
VOID
ExpectTransfer(HANDLE hFrom, HANDLE hTo, PCSTR Where)
{
    CHAR Byte = 'x', In = 0;
    DWORD cb = 0;
    BOOL Ret;

    if (hFrom == INVALID_HANDLE_VALUE || hTo == INVALID_HANDLE_VALUE)
        return;

    Ret = WriteFile(hFrom, &Byte, 1, &cb, NULL);
    ok(Ret && cb == 1, "%s: WriteFile failed, error %lu\n", Where, GetLastError());
    Ret = ReadFile(hTo, &In, 1, &cb, NULL);
    ok(Ret && cb == 1 && In == 'x', "%s: ReadFile failed, error %lu\n", Where, GetLastError());
}

static
VOID
ExpectInfo(HANDLE hPipe, DWORD dwFlags, DWORD dwMax, DWORD dwCurrent, PCSTR Where)
{
    DWORD dwActualFlags = 0xdeadbeef, dwActualMax = 0xdeadbeef, dwActualCurrent = 0xdeadbeef;
    BOOL Ret;

    if (hPipe == INVALID_HANDLE_VALUE)
        return;

    Ret = GetNamedPipeInfo(hPipe, &dwActualFlags, NULL, NULL, &dwActualMax);
    ok(Ret, "%s: GetNamedPipeInfo failed, error %lu\n", Where, GetLastError());
    ok(dwActualFlags == dwFlags, "%s: flags 0x%lx, expected 0x%lx\n", Where, dwActualFlags, dwFlags);
    ok(dwActualMax == dwMax, "%s: max %lu, expected %lu\n", Where, dwActualMax, dwMax);

    Ret = GetNamedPipeHandleStateW(hPipe, NULL, &dwActualCurrent, NULL, NULL, NULL, 0);
    ok(Ret, "%s: GetNamedPipeHandleStateW failed, error %lu\n", Where, GetLastError());
    ok(dwActualCurrent == dwCurrent, "%s: current instances %lu, expected %lu\n",
       Where, dwActualCurrent, dwCurrent);
}

static
VOID
Close(HANDLE h)
{
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

/* (a) (b) (c) (d) with MaximumInstances 1, the server closing first */
static
VOID
TestServerClosesFirst(VOID)
{
    HANDLE hServer1, hServer2, hClient1, hClient2, hClient3;

    NewName();
    hServer1 = MustCreate(1, PIPE_TYPE_BYTE, "server1");
    hClient1 = OpenClient("client1");
    ConnectServer(hServer1, "server1");
    ExpectTransfer(hClient1, hServer1, "client1->server1");
    ExpectInfo(hClient1, 0, 1, 1, "client1 while connected");

    Close(hServer1);

    /* The old client is only a stub now */
    ExpectBroken(hClient1, "client1 after server close");
    ExpectInfo(hClient1, 0, 1, 0, "client1 after server close");

    /* Nothing to connect to: the name is gone for new clients */
    SetLastError(0xdeadbeef);
    hClient2 = CreateFileW(g_PipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(hClient2 == INVALID_HANDLE_VALUE, "CreateFileW succeeded without a server\n");
    ok(GetLastError() == ERROR_FILE_NOT_FOUND, "CreateFileW error %lu, expected %lu\n",
       GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);
    Close(hClient2);

    /* (a) the dead server does not count against MaximumInstances */
    hServer2 = MustCreate(1, PIPE_TYPE_BYTE, "server2 (a)");
    MustBeBusy(1, "server3 while server2 is open");

    /* (b) the old client is unaffected by the new instance, and before and after it connects */
    ExpectBroken(hClient1, "client1 after server2 created");
    ExpectInfo(hServer2, 1, 1, 1, "server2");
    hClient3 = OpenClient("client3");
    ConnectServer(hServer2, "server2");
    ExpectTransfer(hClient3, hServer2, "client3->server2");
    ExpectTransfer(hServer2, hClient3, "server2->client3");
    ExpectBroken(hClient1, "client1 after client3 connected");
    ExpectInfo(hClient1, 0, 1, 0, "client1 after client3 connected");

    /* (c) the old client closes first: nothing changes for the new pipe */
    Close(hClient1);
    MustBeBusy(1, "server3 after client1 closed");
    ExpectTransfer(hClient3, hServer2, "client3->server2 after client1 closed");
    Close(hClient3);
    Close(hServer2);

    /* (d) everything closed, the name is free again */
    hServer1 = MustCreate(1, PIPE_TYPE_BYTE, "server (d)");
    Close(hServer1);
}

static
VOID
TestClientClosesFirst(VOID)
{
    HANDLE hServer1, hServer2, hClient1;

    NewName();
    hServer1 = MustCreate(1, PIPE_TYPE_BYTE, "server1");
    hClient1 = OpenClient("client1");
    ConnectServer(hServer1, "server1");
    Close(hClient1);

    ExpectBroken(hServer1, "server1 after client close");

    /* The server end still is an instance */
    MustBeBusy(1, "server2 while server1 is open");
    ExpectInfo(hServer1, 1, 1, 1, "server1 after client close");
    Close(hServer1);

    hServer2 = MustCreate(1, PIPE_TYPE_BYTE, "server2 after both closed");
    Close(hServer2);

    /* Same after a disconnect */
    NewName();
    hServer1 = MustCreate(1, PIPE_TYPE_BYTE, "server1 (disconnect)");
    hClient1 = OpenClient("client1 (disconnect)");
    ConnectServer(hServer1, "server1 (disconnect)");
    Close(hClient1);
    ok(DisconnectNamedPipe(hServer1), "DisconnectNamedPipe failed, error %lu\n", GetLastError());
    MustBeBusy(1, "server2 after disconnect");
    Close(hServer1);
    hServer2 = MustCreate(1, PIPE_TYPE_BYTE, "server2 after disconnect and close");
    Close(hServer2);

    /* A never connected, closed server */
    NewName();
    hServer1 = MustCreate(1, PIPE_TYPE_BYTE, "server1 (unused)");
    Close(hServer1);
    hServer2 = MustCreate(1, PIPE_TYPE_BYTE, "server2 (unused)");
    Close(hServer2);
}

/* A new pipe after the server closed is a different pipe: its own parameters count */
static
VOID
TestNewPipeIsIndependent(VOID)
{
    HANDLE hServer1, hClient1, hServer2, hServer3, hServer4, hClient2;

    NewName();
    hServer1 = MustCreate(1, PIPE_TYPE_BYTE, "server1");
    hClient1 = OpenClient("client1");
    ConnectServer(hServer1, "server1");
    Close(hServer1);

    hServer2 = MustCreate(3, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, "server2 (max 3)");
    hServer3 = MustCreate(3, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, "server3 (max 3)");
    hServer4 = MustCreate(3, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, "server4 (max 3)");
    MustBeBusy(3, "server5 over the limit");

    ExpectInfo(hServer2, PIPE_SERVER_END | PIPE_TYPE_MESSAGE, 3, 3, "server2");
    hClient2 = OpenClient("client2");
    ExpectInfo(hClient2, PIPE_TYPE_MESSAGE, 3, 3, "client2");
    ExpectInfo(hClient1, 0, 1, 0, "client1");

    Close(hClient2);
    Close(hClient1);
    Close(hServer2);
    Close(hServer3);
    Close(hServer4);
}

/* The count of open server handles, with a closed one whose client is still open */
static
VOID
TestMaxInstances(DWORD dwMax)
{
    HANDLE Servers[8], hClient;
    DWORD i;
    CHAR Name[32];

    NewName();
    for (i = 0; i < dwMax; i++)
    {
        StringCchPrintfA(Name, sizeof(Name), "server %lu/%lu", i, dwMax);
        Servers[i] = MustCreate(dwMax, PIPE_TYPE_BYTE, Name);
    }
    MustBeBusy(dwMax, "over the limit");

    hClient = OpenClient("client");
    ConnectServer(Servers[0], "server 0");

    Close(Servers[0]);
    Servers[0] = INVALID_HANDLE_VALUE;

    /* One slot is free again, exactly one */
    Servers[0] = MustCreate(dwMax, PIPE_TYPE_BYTE, "replacement");
    MustBeBusy(dwMax, "after replacement");
    ExpectBroken(hClient, "client of the closed server");

    Close(hClient);
    /* The client closing does not change the count */
    MustBeBusy(dwMax, "after client closed");

    for (i = 0; i < dwMax; i++)
    {
        Close(Servers[i]);
        Servers[i] = INVALID_HANDLE_VALUE;
    }
    Servers[0] = MustCreate(dwMax, PIPE_TYPE_BYTE, "fresh after all closed");
    Close(Servers[0]);
}

/* Two closed servers with open clients leave the pipe with no instance at all */
static
VOID
TestAllServersClosed(VOID)
{
    HANDLE hServer1, hServer2, hServer3, hServer4, hClient1, hClient2;

    NewName();
    hServer1 = MustCreate(2, PIPE_TYPE_BYTE, "server1");
    hServer2 = MustCreate(2, PIPE_TYPE_BYTE, "server2");
    hClient1 = OpenClient("client1");
    ConnectServer(hServer1, "server1");
    hClient2 = OpenClient("client2");
    ConnectServer(hServer2, "server2");

    Close(hServer1);
    Close(hServer2);
    ExpectBroken(hClient1, "client1");
    ExpectBroken(hClient2, "client2");

    hServer3 = MustCreate(2, PIPE_TYPE_BYTE, "server3");
    hServer4 = MustCreate(2, PIPE_TYPE_BYTE, "server4");
    MustBeBusy(2, "server5 over the limit");

    Close(hClient2);
    Close(hClient1);
    Close(hServer3);
    Close(hServer4);
}

START_TEST(NamedPipeInstanceReuse)
{
    TestServerClosesFirst();
    TestClientClosesFirst();
    TestNewPipeIsIndependent();
    TestMaxInstances(2);
    TestMaxInstances(3);
    TestAllServersClosed();
}
