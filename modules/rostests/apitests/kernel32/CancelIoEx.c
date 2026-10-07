/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for CancelIoEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL WINAPI FN_CancelIoEx(_In_ HANDLE hFile, _In_opt_ LPOVERLAPPED lpOverlapped);

static FN_CancelIoEx *pCancelIoEx;

static
BOOL
CreatePipePair(_Out_ PHANDLE Server, _Out_ PHANDLE Client)
{
    static const WCHAR Name[] = L"\\\\.\\pipe\\rostest_cancelioex";

    *Server = CreateNamedPipeW(Name,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_BYTE | PIPE_WAIT,
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
TestSameThread(VOID)
{
    HANDLE Server, Client;
    OVERLAPPED Ov = { 0 };
    CHAR Buffer[16];
    DWORD Bytes;
    BOOL Ret;

    if (!CreatePipePair(&Server, &Client))
    {
        skip("Pipe creation failed: %lu\n", GetLastError());
        return;
    }
    Ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* Nothing is pending yet */
    SetLastError(0xdeadbeef);
    Ret = pCancelIoEx(Server, NULL);
    ok(!Ret && GetLastError() == ERROR_NOT_FOUND, "Ret %d, error %lu\n", Ret, GetLastError());

    Ret = ReadFile(Server, Buffer, sizeof(Buffer), NULL, &Ov);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile: Ret %d, error %lu\n", Ret, GetLastError());

    Ret = pCancelIoEx(Server, &Ov);
    ok(Ret, "CancelIoEx failed: %lu\n", GetLastError());
    Ret = GetOverlappedResult(Server, &Ov, &Bytes, TRUE);
    ok(!Ret && GetLastError() == ERROR_OPERATION_ABORTED, "Ret %d, error %lu\n", Ret, GetLastError());

    /* The request is gone */
    SetLastError(0xdeadbeef);
    Ret = pCancelIoEx(Server, &Ov);
    ok(!Ret && GetLastError() == ERROR_NOT_FOUND, "Ret %d, error %lu\n", Ret, GetLastError());

    CloseHandle(Ov.hEvent);
    CloseHandle(Client);
    CloseHandle(Server);
}

typedef struct _READER
{
    HANDLE Server;
    OVERLAPPED Ov;
    CHAR Buffer[16];
    HANDLE Started;
    HANDLE Done;
} READER, *PREADER;

static
DWORD
WINAPI
ReaderThread(_In_ PVOID Parameter)
{
    PREADER Reader = Parameter;
    BOOL Ret;

    Ret = ReadFile(Reader->Server, Reader->Buffer, sizeof(Reader->Buffer), NULL, &Reader->Ov);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile: Ret %d, error %lu\n", Ret, GetLastError());
    SetEvent(Reader->Started);

    /* Stay alive: a thread's requests are cancelled when it exits */
    WaitForSingleObject(Reader->Done, INFINITE);
    return 0;
}

static
VOID
TestOtherThread(VOID)
{
    READER Reader = { 0 };
    HANDLE Client, Thread;
    DWORD Bytes;
    BOOL Ret;

    if (!CreatePipePair(&Reader.Server, &Client))
    {
        skip("Pipe creation failed: %lu\n", GetLastError());
        return;
    }
    Reader.Ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Reader.Started = CreateEventW(NULL, TRUE, FALSE, NULL);
    Reader.Done = CreateEventW(NULL, TRUE, FALSE, NULL);

    Thread = CreateThread(NULL, 0, ReaderThread, &Reader, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        goto Cleanup;
    ok(WaitForSingleObject(Reader.Started, 5000) == WAIT_OBJECT_0, "Reader did not start\n");

    /* The request of the other thread is found and cancelled */
    Ret = pCancelIoEx(Reader.Server, NULL);
    ok(Ret, "CancelIoEx failed: %lu\n", GetLastError());
    ok(WaitForSingleObject(Reader.Ov.hEvent, 5000) == WAIT_OBJECT_0, "Request was not cancelled\n");
    Ret = GetOverlappedResult(Reader.Server, &Reader.Ov, &Bytes, FALSE);
    ok(!Ret && GetLastError() == ERROR_OPERATION_ABORTED, "Ret %d, error %lu\n", Ret, GetLastError());

    SetEvent(Reader.Done);
    WaitForSingleObject(Thread, 5000);
    CloseHandle(Thread);

Cleanup:
    CloseHandle(Reader.Done);
    CloseHandle(Reader.Started);
    CloseHandle(Reader.Ov.hEvent);
    CloseHandle(Client);
    CloseHandle(Reader.Server);
}

START_TEST(CancelIoEx)
{
    pCancelIoEx = (FN_CancelIoEx *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CancelIoEx");
    if (!pCancelIoEx)
    {
        skip("CancelIoEx is not available\n");
        return;
    }

    TestSameThread();
    TestOtherThread();
}
