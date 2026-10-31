/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     When the status block of a cancelled completion-port request is written
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expectations were taken from Windows 10 22H2 (x86 and x64). A request that has no event, no APC
 * routine and an overlapped context, on a handle associated with a completion port, is "port only".
 * Windows writes the status block of such a request that was cancelled (CancelIo, CancelIoEx, on a pipe
 * and on an AFD socket) when the completion packet is removed from the port, not when it is cancelled:
 * until then OVERLAPPED.Internal is still STATUS_PENDING and HasOverlappedIoCompleted is FALSE.
 * A request on a handle without a port is cancelled, and its status block written, when its thread exits.
 *
 * A port only request is not cancelled when the thread that issued it exits: see IocpThreadExit.
 *
 * ReadFileEx and WriteFileEx leave the OVERLAPPED as it was when the request is refused before it is
 * queued: on a handle associated with a completion port (an APC routine cannot be used with one,
 * ERROR_INVALID_PARAMETER) and on an invalid handle. Only the plain ReadFile writes STATUS_PENDING first.
 */

#include "precomp.h"
#include <winsock2.h>

#define ST_PENDING      0x00000103UL
#define ST_CANCELLED    0xC0000120UL

typedef BOOL WINAPI FN_CancelIoEx(_In_ HANDLE hFile, _In_opt_ LPOVERLAPPED lpOverlapped);

static FN_CancelIoEx *pCancelIoEx;

enum { CANCEL_IOEX, CANCEL_IO };

static int PipeCount;

static
BOOL
CreatePipePair(_Out_ PHANDLE Server, _Out_ PHANDLE Client)
{
    WCHAR Name[64];

    StringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"\\\\.\\pipe\\rostest_iocpcancel_%lu_%d", GetCurrentProcessId(), PipeCount++);
    *Server = CreateNamedPipeW(Name,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                               1, 512, 512, 0, NULL);
    if (*Server == INVALID_HANDLE_VALUE)
        return FALSE;

    *Client = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
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
InitOverlapped(_Out_ OVERLAPPED *Ov, _In_opt_ HANDLE Event)
{
    /* Poison Internal and InternalHigh so that "not written" is visible; the request resets Internal.
       Only the low half is poisoned: HasOverlappedIoCompleted compares all of Internal here. */
    memset(Ov, 0, sizeof(*Ov));
    Ov->Internal = 0xcccccccc;
    Ov->InternalHigh = 0xcccccccc;
    Ov->Offset = 0;
    Ov->OffsetHigh = 0;
    Ov->hEvent = Event;
}

/* The request is pending and nothing is reported */
static
VOID
CheckPending(_In_ const char *Label, _In_ OVERLAPPED *Ov)
{
    DWORD Bytes = 0xdeadbeef, Error;
    BOOL Ret;

    ok((ULONG)Ov->Internal == ST_PENDING, "%s: Internal is %08lx\n", Label, (ULONG)Ov->Internal);
    ok(!HasOverlappedIoCompleted(Ov), "%s: HasOverlappedIoCompleted is TRUE\n", Label);
    SetLastError(0xdeadbeef);
    Ret = GetOverlappedResult(NULL, Ov, &Bytes, FALSE);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_IO_INCOMPLETE, "%s: GetOverlappedResult %d, error %lu\n", Label, Ret, Error);
}

static
VOID
CheckCancelled(_In_ const char *Label, _In_ OVERLAPPED *Ov)
{
    DWORD Bytes = 0xdeadbeef, Error;
    BOOL Ret;

    ok((ULONG)Ov->Internal == ST_CANCELLED, "%s: Internal is %08lx\n", Label, (ULONG)Ov->Internal);
    ok(HasOverlappedIoCompleted(Ov), "%s: HasOverlappedIoCompleted is FALSE\n", Label);
    SetLastError(0xdeadbeef);
    Ret = GetOverlappedResult(NULL, Ov, &Bytes, FALSE);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_OPERATION_ABORTED, "%s: GetOverlappedResult %d, error %lu\n", Label, Ret, Error);
}

/* Cancel, look before the packet is removed, remove it, look again */
static
VOID
CheckCancelThenDequeue(_In_ const char *Label, _In_ HANDLE Port, _In_ OVERLAPPED *Ov, _In_ ULONG_PTR ExpectedKey)
{
    DWORD Bytes = 0xdeadbeef, Error;
    ULONG_PTR Key = 0;
    OVERLAPPED *Got = NULL;
    BOOL Ret;

    Sleep(50);
    CheckPending(Label, Ov);

    SetLastError(0xdeadbeef);
    Ret = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Got, 1000);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_OPERATION_ABORTED, "%s: GetQueuedCompletionStatus %d, error %lu\n", Label, Ret, Error);
    ok(Got == Ov, "%s: got %p, expected %p\n", Label, Got, Ov);
    ok(Key == ExpectedKey, "%s: key %Iu\n", Label, Key);
    ok(Bytes == 0, "%s: bytes %lu\n", Label, Bytes);
    CheckCancelled(Label, Ov);
}

static
VOID
TestPipeCancel(_In_ const char *Label, _In_ int How)
{
    HANDLE Server, Client, Port;
    OVERLAPPED Ov;
    CHAR Buffer[16];
    BOOL Ret;

    if (!CreatePipePair(&Server, &Client))
    {
        skip("%s: pipe creation failed: %lu\n", Label, GetLastError());
        return;
    }
    Port = CreateIoCompletionPort(Server, NULL, 0x1234, 0);
    ok(Port != NULL, "%s: CreateIoCompletionPort failed: %lu\n", Label, GetLastError());

    InitOverlapped(&Ov, NULL);
    SetLastError(0xdeadbeef);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), NULL, &Ov);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "%s: ReadFile %d, error %lu\n", Label, Ret, GetLastError());
    CheckPending(Label, &Ov);

    if (How == CANCEL_IOEX)
        Ret = pCancelIoEx(Server, &Ov);
    else
        Ret = CancelIo(Server);
    ok(Ret, "%s: cancel failed: %lu\n", Label, GetLastError());

    CheckCancelThenDequeue(Label, Port, &Ov, 0x1234);

    CloseHandle(Port);
    CloseHandle(Client);
    CloseHandle(Server);
}

typedef struct _READER
{
    HANDLE Handle;
    OVERLAPPED *Ov;
} READER;

static CHAR ReaderBuffer[16];

static
DWORD
WINAPI
ReaderThread(_In_ LPVOID Param)
{
    READER *Reader = Param;

    /* Leave with the read pending. The buffer is static: it must outlive this thread. */
    ReadFile(Reader->Handle, ReaderBuffer, sizeof(ReaderBuffer), NULL, Reader->Ov);
    return 0;
}

/* A request with an event and no port is cancelled when its thread exits, and its status block is written then */
static
VOID
TestThreadExitEvent(VOID)
{
    HANDLE Server, Client, Thread, Event;
    OVERLAPPED Ov;
    READER Reader;

    if (!CreatePipePair(&Server, &Client))
    {
        skip("Pipe creation failed: %lu\n", GetLastError());
        return;
    }
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitOverlapped(&Ov, Event);
    Reader.Handle = Server;
    Reader.Ov = &Ov;
    Thread = CreateThread(NULL, 0, ReaderThread, &Reader, 0, NULL);
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);

    ok(WaitForSingleObject(Event, 1000) == WAIT_OBJECT_0, "The event is not signalled\n");
    CheckCancelled("thread exit, event", &Ov);

    CloseHandle(Event);
    CloseHandle(Client);
    CloseHandle(Server);
}

typedef int (WSAAPI *FN_WSAStartup)(WORD, LPWSADATA);
typedef int (WSAAPI *FN_WSACleanup)(void);
typedef SOCKET (WSAAPI *FN_WSASocketW)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
typedef int (WSAAPI *FN_bind)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *FN_WSARecv)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *FN_closesocket)(SOCKET);

static
VOID
TestUdpCancel(_In_ const char *Label, _In_ int How)
{
    HMODULE Ws2 = LoadLibraryW(L"ws2_32.dll");
    FN_WSAStartup pWSAStartup;
    FN_WSACleanup pWSACleanup;
    FN_WSASocketW pWSASocketW;
    FN_bind pbind;
    FN_WSARecv pWSARecv;
    FN_closesocket pclosesocket;
    WSADATA Data;
    SOCKET Sock;
    struct sockaddr_in Addr;
    HANDLE Port;
    OVERLAPPED Ov;
    CHAR Buffer[16];
    WSABUF Buf = { sizeof(Buffer), Buffer };
    DWORD Flags = 0;
    int Ret;

    if (!Ws2)
    {
        skip("ws2_32 is not available\n");
        return;
    }
    pWSAStartup = (FN_WSAStartup)GetProcAddress(Ws2, "WSAStartup");
    pWSACleanup = (FN_WSACleanup)GetProcAddress(Ws2, "WSACleanup");
    pWSASocketW = (FN_WSASocketW)GetProcAddress(Ws2, "WSASocketW");
    pbind = (FN_bind)GetProcAddress(Ws2, "bind");
    pWSARecv = (FN_WSARecv)GetProcAddress(Ws2, "WSARecv");
    pclosesocket = (FN_closesocket)GetProcAddress(Ws2, "closesocket");
    if (!pWSAStartup || !pWSACleanup || !pWSASocketW || !pbind || !pWSARecv || !pclosesocket)
    {
        skip("Winsock functions are not available\n");
        return;
    }

    ok(pWSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");
    Sock = pWSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (Sock == INVALID_SOCKET)
    {
        skip("Socket creation failed\n");
        pWSACleanup();
        return;
    }
    memset(&Addr, 0, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = 0x0100007f; /* 127.0.0.1 */
    ok(pbind(Sock, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed\n");
    Port = CreateIoCompletionPort((HANDLE)Sock, NULL, 0x5678, 0);
    ok(Port != NULL, "%s: CreateIoCompletionPort failed: %lu\n", Label, GetLastError());

    InitOverlapped(&Ov, NULL);
    Ret = pWSARecv(Sock, &Buf, 1, NULL, &Flags, (LPWSAOVERLAPPED)&Ov, NULL);
    ok(Ret == SOCKET_ERROR && GetLastError() == WSA_IO_PENDING, "%s: WSARecv %d, error %lu\n", Label, Ret, GetLastError());
    CheckPending(Label, &Ov);

    if (How == CANCEL_IOEX)
        Ret = pCancelIoEx((HANDLE)Sock, &Ov);
    else
        Ret = CancelIo((HANDLE)Sock);
    ok(Ret, "%s: cancel failed: %lu\n", Label, GetLastError());

    CheckCancelThenDequeue(Label, Port, &Ov, 0x5678);

    CloseHandle(Port);
    pclosesocket(Sock);
    pWSACleanup();
}

static
VOID CALLBACK
ExApc(_In_ DWORD Error, _In_ DWORD Bytes, _In_ LPOVERLAPPED Ov)
{
}

static
VOID
CheckExRefused(_In_ const char *Label, _In_ BOOL Ret, _In_ DWORD Error, _In_ DWORD Expected, _In_ OVERLAPPED *Ov)
{
    ok(!Ret, "%s: succeeded\n", Label);
    ok(Error == Expected, "%s: error %lu, expected %lu\n", Label, Error, Expected);
    ok(Ov->Internal == (ULONG_PTR)0xcccccccc, "%s: Internal is %Ix\n", Label, Ov->Internal);
    ok(Ov->InternalHigh == (ULONG_PTR)0xcccccccc, "%s: InternalHigh is %Ix\n", Label, Ov->InternalHigh);
}

static
VOID
TestExRefusedLeavesOverlapped(VOID)
{
    HANDLE Server, Client, Port;
    OVERLAPPED Ov;
    CHAR Buffer[16] = { 0 };
    BOOL Ret;
    DWORD Error;

    if (!CreatePipePair(&Server, &Client))
    {
        skip("Pipe creation failed: %lu\n", GetLastError());
        return;
    }
    Port = CreateIoCompletionPort(Server, NULL, 0x4321, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed: %lu\n", GetLastError());

    InitOverlapped(&Ov, NULL);
    SetLastError(0xdeadbeef);
    Ret = ReadFileEx(Server, Buffer, sizeof(Buffer), &Ov, ExApc);
    Error = GetLastError();
    CheckExRefused("ReadFileEx, port", Ret, Error, ERROR_INVALID_PARAMETER, &Ov);

    InitOverlapped(&Ov, NULL);
    SetLastError(0xdeadbeef);
    Ret = WriteFileEx(Server, Buffer, sizeof(Buffer), &Ov, ExApc);
    Error = GetLastError();
    CheckExRefused("WriteFileEx, port", Ret, Error, ERROR_INVALID_PARAMETER, &Ov);

    InitOverlapped(&Ov, NULL);
    SetLastError(0xdeadbeef);
    Ret = ReadFileEx((HANDLE)(ULONG_PTR)0x1234, Buffer, sizeof(Buffer), &Ov, ExApc);
    Error = GetLastError();
    CheckExRefused("ReadFileEx, bad handle", Ret, Error, ERROR_INVALID_HANDLE, &Ov);

    InitOverlapped(&Ov, NULL);
    SetLastError(0xdeadbeef);
    Ret = WriteFileEx((HANDLE)(ULONG_PTR)0x1234, Buffer, sizeof(Buffer), &Ov, ExApc);
    Error = GetLastError();
    CheckExRefused("WriteFileEx, bad handle", Ret, Error, ERROR_INVALID_HANDLE, &Ov);

    /* The plain ReadFile does write the status first */
    InitOverlapped(&Ov, NULL);
    SetLastError(0xdeadbeef);
    Ret = ReadFile((HANDLE)(ULONG_PTR)0x1234, Buffer, sizeof(Buffer), NULL, &Ov);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_INVALID_HANDLE, "ReadFile, bad handle: %d, error %lu\n", Ret, Error);
    ok((ULONG)Ov.Internal == ST_PENDING, "ReadFile, bad handle: Internal is %Ix\n", Ov.Internal);

    CloseHandle(Port);
    CloseHandle(Client);
    CloseHandle(Server);
}

START_TEST(IocpCancelStatus)
{
    pCancelIoEx = (FN_CancelIoEx *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CancelIoEx");
    if (!pCancelIoEx)
    {
        skip("CancelIoEx is not available\n");
        return;
    }

    TestPipeCancel("pipe CancelIoEx", CANCEL_IOEX);
    TestPipeCancel("pipe CancelIo", CANCEL_IO);
    TestThreadExitEvent();
    TestUdpCancel("udp CancelIoEx", CANCEL_IOEX);
    TestUdpCancel("udp CancelIo", CANCEL_IO);
    TestExRefusedLeavesOverlapped();
}
