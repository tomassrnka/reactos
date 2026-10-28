/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Terminate processes that have named pipe, socket and I/O completion port I/O pending
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"

/*
 * The child opens named pipe pairs with overlapped reads and a listen pending,
 * loopback TCP connections with overlapped receives re-armed from an I/O
 * completion port while the parent streams data into them, threads blocked in
 * recv, and a UDP socket with a receive pending. The parent kills children
 * after a random delay, several at a time, and requires each to be gone in time:
 * process rundown must cancel and complete all of that I/O.
 */

#define KPI_ITERATIONS 60
#define KPI_CONCURRENT 3
#define KPI_MAX_DELAY_MS 300
#define KPI_PIPES 8
#define KPI_SOCKETS 8

static HANDLE ChildPort;
static SOCKET ListenSocket = INVALID_SOCKET;
static volatile LONG StopFeeding;

static
DWORD
WINAPI
ChildDrainThread(LPVOID Parameter)
{
    DWORD Bytes;
    ULONG_PTR Key;
    OVERLAPPED *Overlapped;

    for (;;)
    {
        Overlapped = NULL;
        if (!GetQueuedCompletionStatus(ChildPort, &Bytes, &Key, &Overlapped, INFINITE) && !Overlapped)
            Sleep(1);
        if (Overlapped && Key)
        {
            /* Re-arm a receive on the socket */
            WSABUF Buffer;
            DWORD Flags = 0;
            Buffer.buf = (char *)(Overlapped + 1);
            Buffer.len = 4096;
            ZeroMemory(Overlapped, sizeof(*Overlapped));
            WSARecv((SOCKET)Key, &Buffer, 1, NULL, &Flags, Overlapped, NULL);
        }
    }
    return 0;
}

static
DWORD
WINAPI
ChildBlockingRecvThread(LPVOID Parameter)
{
    char Buffer[4096];
    while (recv((SOCKET)Parameter, Buffer, sizeof(Buffer), 0) > 0);
    return 0;
}

static
DWORD
WINAPI
ChildPipeWriterThread(LPVOID Parameter)
{
    char Buffer[2048];
    DWORD Written;

    memset(Buffer, 'p', sizeof(Buffer));
    for (;;)
    {
        OVERLAPPED Overlapped = { 0 };
        Overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!WriteFile((HANDLE)Parameter, Buffer, sizeof(Buffer), &Written, &Overlapped) &&
            GetLastError() == ERROR_IO_PENDING)
        {
            WaitForSingleObject(Overlapped.hEvent, 50);
        }
        CloseHandle(Overlapped.hEvent);
    }
    return 0;
}

static
OVERLAPPED *
AllocOverlapped(void)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(OVERLAPPED) + 4096);
}

static
void
RunChild(int Port, int Tag)
{
    WSADATA WsaData;
    struct sockaddr_in Address = { 0 };
    SOCKET Udp;
    WSABUF Buffer;
    DWORD Flags;
    OVERLAPPED *Overlapped;
    int i, j;

    WSAStartup(MAKEWORD(2, 2), &WsaData);
    ChildPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    for (i = 0; i < 2; i++)
        CloseHandle(CreateThread(NULL, 0, ChildDrainThread, NULL, 0, NULL));

    for (i = 0; i < KPI_PIPES; i++)
    {
        char Name[128];
        HANDLE Server, Listener, Client;

        sprintf(Name, "\\\\.\\pipe\\ws2_32_apitest_kpi_%lu_%d_%d", GetCurrentProcessId(), Tag, i);
        Server = CreateNamedPipeA(Name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE,
                                  PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
        Listener = CreateNamedPipeA(Name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE,
                                    PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
        /* Stays pending: nobody connects to this instance */
        ConnectNamedPipe(Listener, AllocOverlapped());
        Client = CreateFileA(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (Server == INVALID_HANDLE_VALUE || Client == INVALID_HANDLE_VALUE)
            continue;
        CreateIoCompletionPort(Server, ChildPort, 0, 0);
        CreateIoCompletionPort(Client, ChildPort, 0, 0);
        for (j = 0; j < 4; j++)
        {
            Overlapped = AllocOverlapped();
            ReadFile(Server, Overlapped + 1, 4096, NULL, Overlapped);
            Overlapped = AllocOverlapped();
            ReadFile(Client, Overlapped + 1, 4096, NULL, Overlapped);
        }
        if (i & 1)
            CloseHandle(CreateThread(NULL, 0, ChildPipeWriterThread, Client, 0, NULL));
    }

    Address.sin_family = AF_INET;
    Address.sin_port = htons((USHORT)Port);
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (i = 0; i < KPI_SOCKETS; i++)
    {
        char Data[1024];
        SOCKET Socket = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

        if (connect(Socket, (struct sockaddr *)&Address, sizeof(Address)))
        {
            closesocket(Socket);
            continue;
        }
        if (i % 4 == 3)
        {
            CloseHandle(CreateThread(NULL, 0, ChildBlockingRecvThread, (LPVOID)Socket, 0, NULL));
            continue;
        }
        CreateIoCompletionPort((HANDLE)Socket, ChildPort, (ULONG_PTR)Socket, 0);
        for (j = 0; j < 3; j++)
        {
            Overlapped = AllocOverlapped();
            Buffer.buf = (char *)(Overlapped + 1);
            Buffer.len = 4096;
            Flags = 0;
            WSARecv(Socket, &Buffer, 1, NULL, &Flags, Overlapped, NULL);
        }
        /* The parent reads little of this: its receive window fills */
        memset(Data, 'c', sizeof(Data));
        for (j = 0; j < 8; j++)
            send(Socket, Data, sizeof(Data), 0);
    }

    Udp = WSASocketA(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    Address.sin_port = 0;
    bind(Udp, (struct sockaddr *)&Address, sizeof(Address));
    Overlapped = AllocOverlapped();
    Buffer.buf = (char *)(Overlapped + 1);
    Buffer.len = 4096;
    Flags = 0;
    WSARecvFrom(Udp, &Buffer, 1, NULL, &Flags, NULL, NULL, Overlapped, NULL);

    Sleep(INFINITE);
}

static
DWORD
WINAPI
FeederThread(LPVOID Parameter)
{
    SOCKET Socket = (SOCKET)Parameter;
    char Buffer[4096];
    u_long NonBlocking = 1;

    memset(Buffer, 'f', sizeof(Buffer));
    ioctlsocket(Socket, FIONBIO, &NonBlocking);
    while (!StopFeeding)
    {
        if (send(Socket, Buffer, sizeof(Buffer), 0) < 0)
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK)
                break;
            Sleep(1);
            recv(Socket, Buffer, sizeof(Buffer), 0);
        }
    }
    closesocket(Socket);
    return 0;
}

static
DWORD
WINAPI
AcceptorThread(LPVOID Parameter)
{
    for (;;)
    {
        HANDLE Thread;
        SOCKET Socket = accept(ListenSocket, NULL, NULL);

        if (Socket == INVALID_SOCKET)
        {
            if (StopFeeding)
                break;
            continue;
        }
        Thread = CreateThread(NULL, 0, FeederThread, (LPVOID)Socket, 0, NULL);
        if (Thread)
            CloseHandle(Thread);
        else
            closesocket(Socket);
    }
    return 0;
}

static
void
KillChild(PROCESS_INFORMATION *Child, int *Killed)
{
    DWORD Wait;

    ok(TerminateProcess(Child->hProcess, 1), "TerminateProcess failed: %lu\n", GetLastError());
    Wait = WaitForSingleObject(Child->hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "Child %lu did not exit: %lu\n", Child->dwProcessId, Wait);
    CloseHandle(Child->hProcess);
    CloseHandle(Child->hThread);
    Child->hProcess = NULL;
    (*Killed)++;
}

START_TEST(killpendingio)
{
    WSADATA WsaData;
    struct sockaddr_in Address = { 0 };
    int AddressLength = sizeof(Address);
    PROCESS_INFORMATION Children[KPI_CONCURRENT] = { { 0 } };
    char **Argv, CommandLine[MAX_PATH + 64];
    HANDLE Acceptor;
    DWORD Start;
    int Argc, i, Slot, Killed = 0, Started = 0;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 5 && !strcmp(Argv[2], "child"))
    {
        RunChild(atoi(Argv[3]), atoi(Argv[4]));
        return;
    }

    ok_int(WSAStartup(MAKEWORD(2, 2), &WsaData), 0);
    ListenSocket = socket(AF_INET, SOCK_STREAM, 0);
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ListenSocket == INVALID_SOCKET ||
        bind(ListenSocket, (struct sockaddr *)&Address, sizeof(Address)) ||
        listen(ListenSocket, 64) ||
        getsockname(ListenSocket, (struct sockaddr *)&Address, &AddressLength))
    {
        skip("Listening on loopback failed: %d\n", WSAGetLastError());
        if (ListenSocket != INVALID_SOCKET)
            closesocket(ListenSocket);
        WSACleanup();
        return;
    }
    Acceptor = CreateThread(NULL, 0, AcceptorThread, NULL, 0, NULL);
    ok(Acceptor != NULL, "CreateThread failed: %lu\n", GetLastError());

    srand(GetTickCount());
    Start = GetTickCount();
    for (i = 0; i < KPI_ITERATIONS; i++)
    {
        STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };

        Slot = i % KPI_CONCURRENT;
        if (Children[Slot].hProcess)
            KillChild(&Children[Slot], &Killed);

        sprintf(CommandLine, "\"%s\" killpendingio child %d %d", Argv[0], ntohs(Address.sin_port), i);
        if (!CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                            &StartupInfo, &Children[Slot]))
        {
            ok(0, "CreateProcess failed: %lu\n", GetLastError());
            Children[Slot].hProcess = NULL;
            break;
        }
        Started++;
        Sleep(20 + rand() % (KPI_MAX_DELAY_MS / KPI_CONCURRENT + 1));
    }
    for (Slot = 0; Slot < KPI_CONCURRENT; Slot++)
    {
        if (Children[Slot].hProcess)
            KillChild(&Children[Slot], &Killed);
    }

    ok_int(Killed, Started);
    trace("%d children killed in %lu ms\n", Killed, GetTickCount() - Start);

    StopFeeding = 1;
    closesocket(ListenSocket);
    if (Acceptor)
    {
        ok(WaitForSingleObject(Acceptor, 10000) == WAIT_OBJECT_0, "Acceptor thread did not exit\n");
        CloseHandle(Acceptor);
    }
    WSACleanup();
}
