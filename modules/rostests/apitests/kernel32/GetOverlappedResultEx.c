/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for GetOverlappedResultEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_GetOverlappedResultEx)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
static FN_GetOverlappedResultEx pGetOverlappedResultEx;

static volatile LONG ApcCount;

static VOID CALLBACK Apc(ULONG_PTR Parameter)
{
    InterlockedIncrement(&ApcCount);
}

typedef struct
{
    HANDLE hClient;
    DWORD Delay;
    BOOL Close;
} WORKER;

/* Finishes the pending read of the test thread while that thread waits for it */
static DWORD WINAPI Worker(PVOID Parameter)
{
    WORKER *Work = Parameter;
    DWORD Written;

    Sleep(Work->Delay);
    if (Work->Close)
        CloseHandle(Work->hClient);
    else
        WriteFile(Work->hClient, "abcd", 4, &Written, NULL);
    return 0;
}

static HANDLE StartWorker(WORKER *Work, HANDLE hClient, BOOL Close)
{
    HANDLE hWorker;

    Work->hClient = hClient;
    Work->Delay = 100;
    Work->Close = Close;
    hWorker = CreateThread(NULL, 0, Worker, Work, 0, NULL);
    ok(hWorker != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!hWorker)
    {
        /* Do the work here: the test goes on and fails on its own checks */
        Worker(Work);
    }
    return hWorker;
}

/* The worker owns the context until it is done, so a worker that does not finish ends the test run */
static void JoinWorker(HANDLE hWorker)
{
    if (!hWorker)
        return;
    if (WaitForSingleObject(hWorker, 20000) != WAIT_OBJECT_0)
    {
        ok(0, "The worker did not finish\n");
        ExitProcess(1);
    }
    CloseHandle(hWorker);
}

static void TestFile(void)
{
    WCHAR Path[MAX_PATH];
    OVERLAPPED Overlapped;
    HANDLE hFile;
    DWORD Number, Written, Error;
    char Buffer[16];
    BOOL Ret;

    GetTempPathW(_countof(Path), Path);
    StringCchPrintfW(Path + wcslen(Path), _countof(Path) - wcslen(Path), L"GetOverlappedResultEx-%lu.tmp", GetCurrentProcessId());
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    WriteFile(hFile, "0123456789", 10, &Written, NULL);
    CloseHandle(hFile);

    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        goto Done;

    /* A read that is done, with its event */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ret = ReadFile(hFile, Buffer, 4, &Number, &Overlapped);
    Error = GetLastError();
    ok(Ret || Error == ERROR_IO_PENDING, "ReadFile failed with %lu\n", Error);
    ok(WaitForSingleObject(Overlapped.hEvent, 5000) == WAIT_OBJECT_0, "The read did not finish\n");

    Number = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(hFile, &Overlapped, &Number, 0, FALSE);
    ok(Ret == TRUE && Number == 4 && GetLastError() == ERROR_SUCCESS, "Ret = %d, %lu bytes, error %lu\n", Ret, Number, GetLastError());

    Number = 0xdeadbeef;
    Ret = pGetOverlappedResultEx(hFile, &Overlapped, &Number, 1000, TRUE);
    ok(Ret == TRUE && Number == 4, "Ret = %d, %lu bytes\n", Ret, Number);

    Number = 0xdeadbeef;
    Ret = pGetOverlappedResultEx(hFile, &Overlapped, &Number, INFINITE, FALSE);
    ok(Ret == TRUE && Number == 4, "Ret = %d, %lu bytes\n", Ret, Number);
    CloseHandle(Overlapped.hEvent);

    /* without an event, the handle is what is waited for */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.Offset = 2;
    Ret = ReadFile(hFile, Buffer, 4, &Number, &Overlapped);
    Error = GetLastError();
    ok(Ret || Error == ERROR_IO_PENDING, "ReadFile failed with %lu\n", Error);
    Number = 0xdeadbeef;
    Ret = pGetOverlappedResultEx(hFile, &Overlapped, &Number, 5000, FALSE);
    ok(Ret == TRUE && Number == 4, "Ret = %d, %lu bytes\n", Ret, Number);

    /* a read behind the end */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Overlapped.Offset = 100;
    Ret = ReadFile(hFile, Buffer, 4, &Number, &Overlapped);
    Error = GetLastError();
    if (is_reactos() && !Ret && Error == ERROR_HANDLE_EOF)
    {
        /* Windows reports the end of the file when the operation is waited for; ReactOS fails the read at once */
        skip("ReadFile fails at once behind the end of the file\n");
    }
    else
    {
        ok(!Ret && Error == ERROR_IO_PENDING, "ReadFile: %d, error %lu\n", Ret, Error);
        WaitForSingleObject(Overlapped.hEvent, 5000);
        Number = 0xdeadbeef;
        SetLastError(0xdeadbeef);
        Ret = pGetOverlappedResultEx(hFile, &Overlapped, &Number, 1000, FALSE);
        ok(Ret == FALSE && GetLastError() == ERROR_HANDLE_EOF && Number == 0,
           "Ret = %d, error %lu, %lu bytes\n", Ret, GetLastError(), Number);
    }
    CloseHandle(Overlapped.hEvent);
    CloseHandle(hFile);

Done:
    DeleteFileW(Path);
}

static void TestPipe(void)
{
    static const WCHAR PipeName[] = L"\\\\.\\pipe\\GetOverlappedResultEx-test";
    OVERLAPPED Overlapped;
    HANDLE hServer, hClient, hEvent;
    DWORD Number, Written, Error, Start, Elapsed;
    char Buffer[16];
    WORKER Work;
    HANDLE hWorker;
    BOOL Ret;

    hServer = CreateNamedPipeW(PipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    ok(hServer != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed with %lu\n", GetLastError());
    if (hServer == INVALID_HANDLE_VALUE)
        return;
    hClient = CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(hClient != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hClient == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hServer);
        return;
    }

    hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = hEvent;
    Ret = ReadFile(hServer, Buffer, 4, &Number, &Overlapped);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile: %d, error %lu\n", Ret, GetLastError());

    /* A read that has no data yet: no wait at all */
    Number = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 0, FALSE);
    ok(Ret == FALSE && GetLastError() == ERROR_IO_INCOMPLETE && Number == 0xdeadbeef,
       "Ret = %d, error %lu, bytes %#lx\n", Ret, GetLastError(), Number);

    /* a wait that runs out */
    SetLastError(0xdeadbeef);
    Start = GetTickCount();
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 100, FALSE);
    Error = GetLastError();
    Elapsed = GetTickCount() - Start;
    ok(Ret == FALSE && Error == WAIT_TIMEOUT && Number == 0xdeadbeef,
       "Ret = %d, error %lu, bytes %#lx\n", Ret, Error, Number);
    ok(Elapsed >= 50, "The wait took %lu ms\n", Elapsed);

    /* an alertable wait ends with a queued user APC and runs it; the last error is the result of the wait, not
       ERROR_USER_APC */
    ApcCount = 0;
    QueueUserAPC(Apc, GetCurrentThread(), 0);
    SetLastError(0xdeadbeef);
    Start = GetTickCount();
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 5000, TRUE);
    Error = GetLastError();
    Elapsed = GetTickCount() - Start;
    ok(Ret == FALSE && Error == WAIT_IO_COMPLETION, "Ret = %d, error %lu\n", Ret, Error);
    ok(ApcCount == 1, "The APC ran %ld times\n", ApcCount);
    ok(Number == 0xdeadbeef, "The byte count changed to %lu\n", Number);

    /* a wait that is not alertable does not */
    QueueUserAPC(Apc, GetCurrentThread(), 0);
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 100, FALSE);
    ok(Ret == FALSE && GetLastError() == WAIT_TIMEOUT, "Ret = %d, error %lu\n", Ret, GetLastError());
    ok(ApcCount == 1, "The APC ran %ld times\n", ApcCount);
    SleepEx(0, TRUE);
    ok(ApcCount == 2, "The APC ran %ld times after SleepEx\n", ApcCount);

    /* the data arrives while the call waits */
    hWorker = StartWorker(&Work, hClient, FALSE);
    Number = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 5000, FALSE);
    Error = GetLastError();
    ok(Ret == TRUE && Error == ERROR_SUCCESS && Number == 4, "Ret = %d, error %lu, %lu bytes\n", Ret, Error, Number);
    ok(!memcmp(Buffer, "abcd", 4), "The data is %.4s\n", Buffer);
    JoinWorker(hWorker);

    /* no event: the handle is waited for */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    ZeroMemory(Buffer, sizeof(Buffer));
    Ret = ReadFile(hServer, Buffer, 4, &Number, &Overlapped);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile: %d, error %lu\n", Ret, GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 100, FALSE);
    ok(Ret == FALSE && GetLastError() == WAIT_TIMEOUT, "Ret = %d, error %lu\n", Ret, GetLastError());
    hWorker = StartWorker(&Work, hClient, FALSE);
    Number = 0xdeadbeef;
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 5000, FALSE);
    ok(Ret == TRUE && Number == 4, "Ret = %d, %lu bytes\n", Ret, Number);
    ok(!memcmp(Buffer, "abcd", 4), "The data is %.4s\n", Buffer);
    JoinWorker(hWorker);

    /* the other end goes away while the call waits */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = hEvent;
    ResetEvent(hEvent);
    Ret = ReadFile(hServer, Buffer, 4, &Number, &Overlapped);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_IO_PENDING, "ReadFile: %d, error %lu\n", Ret, Error);
    hWorker = StartWorker(&Work, hClient, TRUE);
    Number = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = pGetOverlappedResultEx(hServer, &Overlapped, &Number, 5000, FALSE);
    Error = GetLastError();
    ok(Ret == FALSE && Error == ERROR_BROKEN_PIPE && Number == 0,
       "Ret = %d, error %lu, %lu bytes\n", Ret, Error, Number);
    JoinWorker(hWorker);

    CloseHandle(hEvent);
    CloseHandle(hServer);
}

START_TEST(GetOverlappedResultEx)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

    pGetOverlappedResultEx = (FN_GetOverlappedResultEx)GetProcAddress(hKernel32, "GetOverlappedResultEx");
    if (!pGetOverlappedResultEx)
    {
        ok(!is_reactos(), "GetOverlappedResultEx is missing\n");
        skip("GetOverlappedResultEx is not available\n");
        return;
    }

    TestFile();
    TestPipe();
}
