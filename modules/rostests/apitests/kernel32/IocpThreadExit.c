/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What happens to a pending request when the thread that issued it exits
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expectations were taken from Windows 10 22H2 (x64, and x86 code under WoW64). Since Vista a
 * request on a handle associated with a completion port that only the port reports (no event, no APC
 * routine, an overlapped context) is not cancelled when its thread exits: it stays pending and completes
 * through the port as usual, with its data. CancelIoEx from another thread still cancels it, CancelIo
 * from another thread does not, closing the handle completes it, and the exit of the process cancels it.
 * A request with an event (even on a handle with a port), with an APC routine, or on a handle without
 * a port is cancelled when its thread exits. Thread pools (.NET, libuv, Go) retire threads with such
 * requests in flight.
 */

#include "precomp.h"
#include <winsock2.h>

#define ST_PENDING      0x00000103UL
#define ST_CANCELLED    0xC0000120UL
#define ST_PIPE_BROKEN  0xC000014BUL

#ifndef FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#endif

#define KEY             0x77
#define DATA            "hello-data"
#define DATA_LEN        10

typedef BOOL WINAPI FN_CancelIoEx(_In_ HANDLE hFile, _In_opt_ LPOVERLAPPED lpOverlapped);
typedef BOOL WINAPI FN_SetFileCompletionNotificationModes(_In_ HANDLE FileHandle, _In_ UCHAR Flags);
typedef BOOL WINAPI FN_IsWow64Process(_In_ HANDLE hProcess, _Out_ PBOOL Wow64Process);

static FN_CancelIoEx *pCancelIoEx;
static FN_SetFileCompletionNotificationModes *pSetFileCompletionNotificationModes;
static BOOL IsWow64;
static int PipeCount;

/* The issuing thread's request; static, it must outlive the thread */
static OVERLAPPED Ov;
static CHAR Buffer[64];
static HANDLE IoHandle;
static volatile LONG ApcCount;

enum { ISSUE_READ, ISSUE_READEX, ISSUE_WSARECV };
enum { AFTER_DATA, AFTER_CANCELIOEX, AFTER_CANCELIOEX_ALL, AFTER_CANCELIO, AFTER_CLOSE };

typedef int (WSAAPI *FN_WSARecv)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static FN_WSARecv pWSARecv;

static
VOID
CALLBACK
ReadApc(_In_ DWORD Error, _In_ DWORD Bytes, _In_ LPOVERLAPPED Overlapped)
{
    InterlockedIncrement(&ApcCount);
}

static
DWORD
WINAPI
IssuerThread(_In_ LPVOID Param)
{
    WSABUF Buf = { sizeof(Buffer), Buffer };
    DWORD Flags = 0;
    BOOL Ret;

    /* Leave with the request pending; returns the error of the call */
    switch ((ULONG_PTR)Param)
    {
        case ISSUE_READEX:
            Ret = ReadFileEx(IoHandle, Buffer, sizeof(Buffer), &Ov, ReadApc);
            return Ret ? ERROR_IO_PENDING : GetLastError();
        case ISSUE_WSARECV:
            if (pWSARecv((SOCKET)IoHandle, &Buf, 1, NULL, &Flags, &Ov, NULL) == 0)
                return ERROR_SUCCESS;
            return GetLastError();
        default:
            Ret = ReadFile(IoHandle, Buffer, sizeof(Buffer), NULL, &Ov);
            return Ret ? ERROR_SUCCESS : GetLastError();
    }
}

static
DWORD
IssueAndExit(_In_ HANDLE Handle, _In_ ULONG_PTR How, _In_opt_ HANDLE Event)
{
    HANDLE Thread;
    DWORD Code = 0xdeadbeef;

    IoHandle = Handle;
    ApcCount = 0;
    memset(Buffer, 0, sizeof(Buffer));
    memset(&Ov, 0, sizeof(Ov));
    Ov.hEvent = Event;
    Thread = CreateThread(NULL, 0, IssuerThread, (PVOID)How, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return Code;
    WaitForSingleObject(Thread, INFINITE);
    GetExitCodeThread(Thread, &Code);
    CloseHandle(Thread);
    /* Let a cancellation, if any, complete */
    Sleep(100);
    return Code;
}

static
BOOL
CreatePipePair(_Out_ PHANDLE Server, _Out_ PHANDLE Client, _In_ BOOL InheritClient)
{
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, TRUE };
    WCHAR Name[64];

    StringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"\\\\.\\pipe\\rostest_iocpexit_%lu_%d", GetCurrentProcessId(), PipeCount++);
    *Server = CreateNamedPipeW(Name,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_BYTE | PIPE_WAIT,
                               1, 4096, 4096, 0, NULL);
    if (*Server == INVALID_HANDLE_VALUE)
        return FALSE;

    *Client = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, InheritClient ? &Sa : NULL,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (*Client == INVALID_HANDLE_VALUE)
    {
        CloseHandle(*Server);
        return FALSE;
    }
    return TRUE;
}

static
VOID
WritePipe(_In_ HANDLE Pipe)
{
    OVERLAPPED WriteOv = { 0 };
    DWORD Written = 0;

    WriteOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!WriteFile(Pipe, DATA, DATA_LEN, &Written, &WriteOv))
        GetOverlappedResult(Pipe, &WriteOv, &Written, TRUE);
    ok(Written == DATA_LEN, "Wrote %lu bytes\n", Written);
    CloseHandle(WriteOv.hEvent);
}

static
DWORD
PipeBytesLeft(_In_ HANDLE Pipe)
{
    DWORD Available = 0xdeadbeef;

    ok(PeekNamedPipe(Pipe, NULL, 0, NULL, &Available, NULL), "PeekNamedPipe failed: %lu\n", GetLastError());
    return Available;
}

/* Nothing was reported yet and the request is still pending */
static
VOID
CheckStillPending(_In_ const char *Label, _In_ HANDLE Port)
{
    DWORD Bytes, Error;
    ULONG_PTR Key;
    OVERLAPPED *Got = NULL;
    BOOL Ret;

    ok((ULONG)Ov.Internal == ST_PENDING, "%s: Internal is %08lx\n", Label, (ULONG)Ov.Internal);
    ok(!HasOverlappedIoCompleted(&Ov), "%s: HasOverlappedIoCompleted is TRUE\n", Label);
    SetLastError(0xdeadbeef);
    Ret = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Got, 0);
    Error = GetLastError();
    ok(!Ret && Got == NULL && Error == WAIT_TIMEOUT, "%s: GetQueuedCompletionStatus %d, %p, error %lu\n", Label, Ret, Got, Error);
}

/* The packet of the request is in the port */
static
VOID
CheckPacket(_In_ const char *Label, _In_ HANDLE Port, _In_ ULONG_PTR ExpectedKey,
            _In_ DWORD ExpectedError, _In_ DWORD ExpectedBytes, _In_ ULONG ExpectedStatus)
{
    DWORD Bytes = 0xdeadbeef, Error;
    ULONG_PTR Key = 0;
    OVERLAPPED *Got = NULL;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Got, 2000);
    Error = Ret ? ERROR_SUCCESS : GetLastError();
    ok(Got == &Ov, "%s: got %p, expected %p (error %lu)\n", Label, Got, &Ov, Error);
    ok(Ret == (ExpectedError == ERROR_SUCCESS), "%s: GetQueuedCompletionStatus returned %d\n", Label, Ret);
    ok(Error == ExpectedError, "%s: error %lu, expected %lu\n", Label, Error, ExpectedError);
    ok(Key == ExpectedKey, "%s: key %Iu\n", Label, Key);
    ok(Bytes == ExpectedBytes, "%s: bytes %lu, expected %lu\n", Label, Bytes, ExpectedBytes);
    ok((ULONG)Ov.Internal == ExpectedStatus, "%s: Internal is %08lx, expected %08lx\n", Label, (ULONG)Ov.Internal, ExpectedStatus);
    ok(Ov.InternalHigh == ExpectedBytes, "%s: InternalHigh is %Iu\n", Label, Ov.InternalHigh);
}

static
VOID
CheckNoPacket(_In_ const char *Label, _In_ HANDLE Port, _In_ DWORD Timeout)
{
    DWORD Bytes, Error;
    ULONG_PTR Key;
    OVERLAPPED *Got = NULL;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Got, Timeout);
    Error = GetLastError();
    ok(!Ret && Got == NULL && Error == WAIT_TIMEOUT, "%s: GetQueuedCompletionStatus %d, %p, error %lu\n", Label, Ret, Got, Error);
}

/* A port-only read on a pipe outlives its thread */
static
VOID
TestPipePortOnly(_In_ const char *Label, _In_ int After, _In_ BOOL SkipOnSuccess)
{
    HANDLE Server, Client, Port;
    DWORD Code;
    BOOL Ret;

    if (!CreatePipePair(&Server, &Client, FALSE))
    {
        skip("%s: pipe creation failed: %lu\n", Label, GetLastError());
        return;
    }
    Port = CreateIoCompletionPort(Server, NULL, KEY, 0);
    ok(Port != NULL, "%s: CreateIoCompletionPort failed: %lu\n", Label, GetLastError());
    if (SkipOnSuccess)
    {
        Ret = pSetFileCompletionNotificationModes(Server, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
        ok(Ret, "%s: SetFileCompletionNotificationModes failed: %lu\n", Label, GetLastError());
    }

    Code = IssueAndExit(Server, ISSUE_READ, NULL);
    ok(Code == ERROR_IO_PENDING, "%s: ReadFile error %lu\n", Label, Code);
    CheckStillPending(Label, Port);

    switch (After)
    {
        case AFTER_DATA:
            WritePipe(Client);
            CheckPacket(Label, Port, KEY, ERROR_SUCCESS, DATA_LEN, 0);
            ok(!memcmp(Buffer, DATA, DATA_LEN), "%s: buffer is '%.10s'\n", Label, Buffer);
            break;

        case AFTER_CANCELIOEX:
        case AFTER_CANCELIOEX_ALL:
            Ret = pCancelIoEx(Server, After == AFTER_CANCELIOEX ? &Ov : NULL);
            ok(Ret, "%s: CancelIoEx failed: %lu\n", Label, GetLastError());
            if (!Ret)
                break;
            CheckPacket(Label, Port, KEY, ERROR_OPERATION_ABORTED, 0, ST_CANCELLED);
            break;

        case AFTER_CANCELIO:
            /* CancelIo cancels the requests of the calling thread only */
            ok(CancelIo(Server), "%s: CancelIo failed: %lu\n", Label, GetLastError());
            CheckNoPacket(Label, Port, 200);
            CheckStillPending(Label, Port);
            WritePipe(Client);
            CheckPacket(Label, Port, KEY, ERROR_SUCCESS, DATA_LEN, 0);
            break;

        case AFTER_CLOSE:
            CloseHandle(Server);
            Server = NULL;
            CheckPacket(Label, Port, KEY, ERROR_BROKEN_PIPE, 0, ST_PIPE_BROKEN);
            break;
    }

    CloseHandle(Port);
    CloseHandle(Client);
    if (Server)
        CloseHandle(Server);
}

/* With an event (also on a handle with a port), the read is cancelled when its thread exits */
static
VOID
TestPipePortEvent(_In_ BOOL NoPacket)
{
    const char *Label = NoPacket ? "port, event|1" : "port, event";
    HANDLE Server, Client, Port, Event;
    DWORD Code;

    if (!CreatePipePair(&Server, &Client, FALSE))
    {
        skip("%s: pipe creation failed: %lu\n", Label, GetLastError());
        return;
    }
    Port = CreateIoCompletionPort(Server, NULL, KEY, 0);
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);

    Code = IssueAndExit(Server, ISSUE_READ, NoPacket ? (HANDLE)((ULONG_PTR)Event | 1) : Event);
    ok(Code == ERROR_IO_PENDING, "%s: ReadFile error %lu\n", Label, Code);
    ok(WaitForSingleObject(Event, 1000) == WAIT_OBJECT_0, "%s: the event is not signalled\n", Label);
    if (NoPacket)
    {
        ok((ULONG)Ov.Internal == ST_CANCELLED, "%s: Internal is %08lx\n", Label, (ULONG)Ov.Internal);
        CheckNoPacket(Label, Port, 0);
    }
    else
    {
        CheckPacket(Label, Port, KEY, ERROR_OPERATION_ABORTED, 0, ST_CANCELLED);
    }

    /* The data is left for the next reader */
    WritePipe(Client);
    Sleep(50);
    ok(PipeBytesLeft(Server) == DATA_LEN, "%s: the data was consumed\n", Label);
    CheckNoPacket(Label, Port, 0);

    CloseHandle(Event);
    CloseHandle(Port);
    CloseHandle(Client);
    CloseHandle(Server);
}

/* Without a port, the read is cancelled when its thread exits */
static
VOID
TestPipeNoPort(_In_ const char *Label, _In_ ULONG_PTR How, _In_ BOOL WithEvent)
{
    HANDLE Server, Client, Event = NULL;
    DWORD Code;

    if (!CreatePipePair(&Server, &Client, FALSE))
    {
        skip("%s: pipe creation failed: %lu\n", Label, GetLastError());
        return;
    }
    if (WithEvent)
        Event = CreateEventW(NULL, TRUE, FALSE, NULL);

    Code = IssueAndExit(Server, How, Event);
    ok(Code == ERROR_IO_PENDING, "%s: read error %lu\n", Label, Code);
    if (Event)
        ok(WaitForSingleObject(Event, 1000) == WAIT_OBJECT_0, "%s: the event is not signalled\n", Label);
    ok((ULONG)Ov.Internal == ST_CANCELLED, "%s: Internal is %08lx\n", Label, (ULONG)Ov.Internal);
    ok(ApcCount == 0, "%s: %ld APCs ran\n", Label, ApcCount);

    WritePipe(Client);
    Sleep(50);
    ok(PipeBytesLeft(Server) == DATA_LEN, "%s: the data was consumed\n", Label);
    ok(Buffer[0] == 0, "%s: the buffer was written\n", Label);

    if (Event)
        CloseHandle(Event);
    CloseHandle(Client);
    CloseHandle(Server);
}

/* A port-only WSARecv on a TCP socket outlives its thread */
static
VOID
TestTcp(_In_ const char *Label, _In_ int After)
{
    HMODULE Ws2 = GetModuleHandleW(L"ws2_32.dll");
    SOCKET (WSAAPI *pWSASocketW)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
    int (WSAAPI *pbind)(SOCKET, const struct sockaddr *, int);
    int (WSAAPI *plisten)(SOCKET, int);
    int (WSAAPI *pgetsockname)(SOCKET, struct sockaddr *, int *);
    int (WSAAPI *pconnect)(SOCKET, const struct sockaddr *, int);
    SOCKET (WSAAPI *paccept)(SOCKET, struct sockaddr *, int *);
    int (WSAAPI *psend)(SOCKET, const char *, int, int);
    int (WSAAPI *pclosesocket)(SOCKET);
    SOCKET Listener, Peer, Sock;
    struct sockaddr_in Addr;
    int Len = sizeof(Addr);
    HANDLE Port;
    DWORD Code;
    BOOL Ret;

    pWSASocketW = (PVOID)GetProcAddress(Ws2, "WSASocketW");
    pbind = (PVOID)GetProcAddress(Ws2, "bind");
    plisten = (PVOID)GetProcAddress(Ws2, "listen");
    pgetsockname = (PVOID)GetProcAddress(Ws2, "getsockname");
    pconnect = (PVOID)GetProcAddress(Ws2, "connect");
    paccept = (PVOID)GetProcAddress(Ws2, "accept");
    psend = (PVOID)GetProcAddress(Ws2, "send");
    pclosesocket = (PVOID)GetProcAddress(Ws2, "closesocket");

    Listener = pWSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    memset(&Addr, 0, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = 0x0100007f; /* 127.0.0.1 */
    if (Listener == INVALID_SOCKET ||
        pbind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) ||
        plisten(Listener, 1) ||
        pgetsockname(Listener, (struct sockaddr *)&Addr, &Len))
    {
        skip("%s: listening socket failed\n", Label);
        if (Listener != INVALID_SOCKET)
            pclosesocket(Listener);
        return;
    }
    Peer = pWSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(pconnect(Peer, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "%s: connect failed\n", Label);
    Sock = paccept(Listener, NULL, NULL);
    pclosesocket(Listener);
    ok(Sock != INVALID_SOCKET, "%s: accept failed\n", Label);
    Port = CreateIoCompletionPort((HANDLE)Sock, NULL, KEY, 0);
    ok(Port != NULL, "%s: CreateIoCompletionPort failed: %lu\n", Label, GetLastError());

    Code = IssueAndExit((HANDLE)Sock, ISSUE_WSARECV, NULL);
    ok(Code == WSA_IO_PENDING, "%s: WSARecv error %lu\n", Label, Code);
    CheckStillPending(Label, Port);

    if (After == AFTER_DATA)
    {
        ok(psend(Peer, DATA, DATA_LEN, 0) == DATA_LEN, "%s: send failed\n", Label);
        CheckPacket(Label, Port, KEY, ERROR_SUCCESS, DATA_LEN, 0);
        ok(!memcmp(Buffer, DATA, DATA_LEN), "%s: buffer is '%.10s'\n", Label, Buffer);
    }
    else
    {
        Ret = pCancelIoEx((HANDLE)Sock, &Ov);
        ok(Ret, "%s: CancelIoEx failed: %lu\n", Label, GetLastError());
        if (Ret)
            CheckPacket(Label, Port, KEY, ERROR_OPERATION_ABORTED, 0, ST_CANCELLED);
    }

    CloseHandle(Port);
    pclosesocket(Sock);
    pclosesocket(Peer);
}

/* ReadDirectoryChangesW: the notification outlives its thread and its records are copied back */
static DWORD DirBuffer[256];

static
DWORD
WINAPI
DirIssuerThread(_In_ LPVOID Param)
{
    if (!ReadDirectoryChangesW((HANDLE)Param, DirBuffer, sizeof(DirBuffer), FALSE,
                               FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &Ov, NULL))
    {
        return GetLastError();
    }
    return ERROR_IO_PENDING;
}

static
VOID
TestDirectoryNotify(VOID)
{
    WCHAR Dir[MAX_PATH], File[MAX_PATH];
    FILE_NOTIFY_INFORMATION *Info = (FILE_NOTIFY_INFORMATION *)DirBuffer;
    HANDLE DirHandle, Port, Thread, Handle;
    DWORD Code = 0xdeadbeef;

    GetTempPathW(RTL_NUMBER_OF(Dir), Dir);
    StringCchCatW(Dir, RTL_NUMBER_OF(Dir), L"rostest_iocpexit");
    CreateDirectoryW(Dir, NULL);
    StringCchPrintfW(File, RTL_NUMBER_OF(File), L"%s\\newfile.txt", Dir);
    DeleteFileW(File);
    DirHandle = CreateFileW(Dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (DirHandle == INVALID_HANDLE_VALUE)
    {
        skip("Directory open failed: %lu\n", GetLastError());
        RemoveDirectoryW(Dir);
        return;
    }
    Port = CreateIoCompletionPort(DirHandle, NULL, KEY, 0);

    memset(&Ov, 0, sizeof(Ov));
    memset(DirBuffer, 0xcc, sizeof(DirBuffer));
    Thread = CreateThread(NULL, 0, DirIssuerThread, DirHandle, 0, NULL);
    WaitForSingleObject(Thread, INFINITE);
    GetExitCodeThread(Thread, &Code);
    CloseHandle(Thread);
    ok(Code == ERROR_IO_PENDING, "ReadDirectoryChangesW error %lu\n", Code);
    if (Code == ERROR_IO_PENDING)
    {
        Sleep(100);
        CheckStillPending("directory", Port);

        Handle = CreateFileW(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        ok(Handle != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError());
        CloseHandle(Handle);

        CheckPacket("directory", Port, KEY, ERROR_SUCCESS, FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName) + 11 * sizeof(WCHAR), 0);
        ok(Info->Action == FILE_ACTION_ADDED, "directory: action %lu\n", Info->Action);
        ok(Info->FileNameLength == 11 * sizeof(WCHAR) && !memcmp(Info->FileName, L"newfile.txt", 11 * sizeof(WCHAR)),
           "directory: name %.*S\n", (int)(Info->FileNameLength / sizeof(WCHAR)), Info->FileName);
    }

    CloseHandle(Port);
    CloseHandle(DirHandle);
    DeleteFileW(File);
    RemoveDirectoryW(Dir);
}

/* Child: bind the inherited pipe end to a port, leave a port-only read behind a thread, exit */
static
VOID
ChildProcess(_In_ HANDLE Client)
{
    HANDLE Port = CreateIoCompletionPort(Client, NULL, KEY, 0);
    DWORD Code = IssueAndExit(Client, ISSUE_READ, NULL);

    /* Exit code 0: the read was pending when the process exited */
    ExitProcess((Port && Code == ERROR_IO_PENDING && (ULONG)Ov.Internal == ST_PENDING) ? 0 : 1);
}

/* The exit of the process cancels its port-only requests, even with the file still open elsewhere */
static
VOID
TestProcessExit(_In_ const char *Self)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    HANDLE Server, Client;
    CHAR CommandLine[MAX_PATH + 64];
    DWORD ExitCode = 0xdeadbeef;

    if (!CreatePipePair(&Server, &Client, TRUE))
    {
        skip("Pipe creation failed: %lu\n", GetLastError());
        return;
    }
    StringCchPrintfA(CommandLine, RTL_NUMBER_OF(CommandLine), "\"%s\" IocpThreadExit child %Iu", Self, (ULONG_PTR)Client);
    if (!CreateProcessA(NULL, CommandLine, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed: %lu\n", GetLastError());
        CloseHandle(Client);
        CloseHandle(Server);
        return;
    }
    ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "The child did not exit\n");
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    ok(ExitCode == 0, "The child's read was not pending when it exited: %lu\n", ExitCode);

    /* The child's read is gone: the data stays in the pipe */
    WritePipe(Server);
    Sleep(100);
    ok(PipeBytesLeft(Client) == DATA_LEN, "The data was consumed by the child's read\n");

    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(Client);
    CloseHandle(Server);
}

START_TEST(IocpThreadExit)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    FN_IsWow64Process *pIsWow64Process;
    WSADATA WsaData;
    HMODULE Ws2;
    char **Argv;
    int Argc;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 4 && !strcmp(Argv[2], "child"))
    {
        ChildProcess((HANDLE)(ULONG_PTR)_strtoui64(Argv[3], NULL, 10));
        return;
    }

    pCancelIoEx = (FN_CancelIoEx *)GetProcAddress(Kernel32, "CancelIoEx");
    pSetFileCompletionNotificationModes =
        (FN_SetFileCompletionNotificationModes *)GetProcAddress(Kernel32, "SetFileCompletionNotificationModes");
    if (!pCancelIoEx || !pSetFileCompletionNotificationModes)
    {
        skip("CancelIoEx or SetFileCompletionNotificationModes is not available\n");
        return;
    }
    pIsWow64Process = (FN_IsWow64Process *)GetProcAddress(Kernel32, "IsWow64Process");
    if (pIsWow64Process)
        pIsWow64Process(GetCurrentProcess(), &IsWow64);

    TestPipePortOnly("port, data", AFTER_DATA, FALSE);
    TestPipePortOnly("port, skip on success, data", AFTER_DATA, TRUE);
    TestPipePortOnly("port, CancelIoEx", AFTER_CANCELIOEX, FALSE);
    TestPipePortOnly("port, CancelIoEx all", AFTER_CANCELIOEX_ALL, FALSE);
    TestPipePortOnly("port, CancelIo", AFTER_CANCELIO, FALSE);
    TestPipePortOnly("port, close", AFTER_CLOSE, FALSE);
    TestPipePortEvent(FALSE);
    TestPipePortEvent(TRUE);
    TestPipeNoPort("no port", ISSUE_READ, FALSE);
    TestPipeNoPort("no port, event", ISSUE_READ, TRUE);
    TestPipeNoPort("no port, APC", ISSUE_READEX, FALSE);
    TestDirectoryNotify();
    TestProcessExit(Argv[0]);

    Ws2 = LoadLibraryW(L"ws2_32.dll");
    pWSARecv = Ws2 ? (FN_WSARecv)GetProcAddress(Ws2, "WSARecv") : NULL;
    if (!pWSARecv)
    {
        skip("ws2_32 is not available\n");
        return;
    }
    ((int (WSAAPI *)(WORD, LPWSADATA))GetProcAddress(Ws2, "WSAStartup"))(MAKEWORD(2, 2), &WsaData);
    TestTcp("tcp, data", AFTER_DATA);
    TestTcp("tcp, CancelIoEx", AFTER_CANCELIOEX);
    ((int (WSAAPI *)(void))GetProcAddress(Ws2, "WSACleanup"))();
}
