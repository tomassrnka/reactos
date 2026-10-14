/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "precomp.h"
#include <pseh/pseh2.h>

static LONG Completions;
static DWORD WorkerId;
static OVERLAPPED Io;

static VOID WINAPI Completed(DWORD Error, DWORD Bytes, LPOVERLAPPED Overlapped)
{
    ok(GetCurrentThreadId() == WorkerId, "completion on the wrong thread\n");
    ok(Overlapped == &Io, "overlapped pointer changed\n");
    ok(Error == ERROR_SUCCESS && Bytes == 16, "completion error %lu bytes %lu\n", Error, Bytes);
    InterlockedIncrement(&Completions);
}

static DWORD WINAPI IoWorker(void *Parameter)
{
    WCHAR Path[MAX_PATH], Name[MAX_PATH];
    HANDLE File;
    BYTE Input[16] = "wow64-thread-io", Output[16] = {0};
    DWORD Wait, Bytes;
    volatile LONG Faulted = 0;
    volatile ULONG_PTR BadAddress = 1;
    UNREFERENCED_PARAMETER(Parameter);
    WorkerId = GetCurrentThreadId();
    _SEH2_TRY { *(volatile BYTE *)BadAddress = 1; }
    _SEH2_EXCEPT(_SEH2_GetExceptionCode() == STATUS_ACCESS_VIOLATION ?
                 EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { Faulted = 1; }
    _SEH2_END;
    ok(Faulted, "hardware exception was not delivered to secondary thread\n");
    if (!GetTempPathW(_countof(Path), Path) || !GetTempFileNameW(Path, L"w3a", 0, Name))
    {
        ok(FALSE, "temp file setup: %lu\n", GetLastError());
        return 1;
    }
    File = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "open overlapped file: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) { DeleteFileW(Name); return 1; }
    ZeroMemory(&Io, sizeof(Io));
    Completions = 0;
    ok(WriteFileEx(File, Input, sizeof(Input), &Io, Completed), "WriteFileEx: %lu\n", GetLastError());
    Wait = SleepEx(10000, TRUE);
    ok(Wait == WAIT_IO_COMPLETION && Completions == 1, "write APC wait %lu callbacks %ld\n", Wait, Completions);
    ZeroMemory(&Io, sizeof(Io));
    ok(ReadFileEx(File, Output, sizeof(Output), &Io, Completed), "ReadFileEx: %lu\n", GetLastError());
    Wait = SleepEx(10000, TRUE);
    ok(Wait == WAIT_IO_COMPLETION && Completions == 2, "read APC wait %lu callbacks %ld\n", Wait, Completions);
    ok(!memcmp(Input, Output, sizeof(Input)), "completion read returned different data\n");
    ZeroMemory(&Io, sizeof(Io));
    Io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Io.hEvent != NULL, "completion event creation failed\n");
    if (Io.hEvent)
    {
        BOOL Result = ReadFile(File, Output, sizeof(Output), NULL, &Io);
        ok(Result || GetLastError() == ERROR_IO_PENDING, "event read: %lu\n", GetLastError());
        ok(WaitForSingleObject(Io.hEvent, 10000) == WAIT_OBJECT_0, "event was not signaled\n");
        ok(GetOverlappedResult(File, &Io, &Bytes, FALSE) && Bytes == sizeof(Input),
           "event became visible before IOSB completion\n");
        CloseHandle(Io.hEvent);
    }
    CloseHandle(File);
    return 0x3a;
}

START_TEST(Wow64ThreadApc)
{
    HANDLE Thread;
    DWORD Exit;
    Thread = CreateThread(NULL, 0, IoWorker, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread: %lu\n", GetLastError());
    if (!Thread) return;
    ok(WaitForSingleObject(Thread, 45000) == WAIT_OBJECT_0, "I/O worker timed out\n");
    GetExitCodeThread(Thread, &Exit);
    ok(Exit == 0x3a, "I/O worker exit %lu\n", Exit);
    CloseHandle(Thread);
}

static HANDLE LifetimeReady, LifetimeWait;

static DWORD WINAPI LifetimeWorker(void *Parameter)
{
    SetEvent(LifetimeReady);
    WaitForSingleObject(LifetimeWait, INFINITE);
    return (DWORD)(ULONG_PTR)Parameter;
}

static DWORD NTAPI RtlWorker(void *Parameter)
{
    RtlExitUserThread((NTSTATUS)(ULONG_PTR)Parameter);
    return 0;
}

START_TEST(Wow64ThreadLifetime)
{
    HANDLE Thread;
    DWORD Exit;
    CONTEXT Context;
    NTSTATUS Status;
    BOOL Guest = FALSE;
    CLIENT_ID Id;
    NTSTATUS (NTAPI *CreateEx)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE,
                              PTHREAD_START_ROUTINE, PVOID, ULONG, ULONG_PTR,
                              SIZE_T, SIZE_T, PVOID);
    BOOL (WINAPI *WowGet)(HANDLE, PWOW64_CONTEXT);
    BOOL (WINAPI *WowSet)(HANDLE, const WOW64_CONTEXT *);

    Status = RtlCreateUserThread(GetCurrentProcess(), NULL, TRUE, 0, 0, 0,
                                 RtlWorker, (void *)0x3141, &Thread, &Id);
    ok(Status == STATUS_SUCCESS, "RtlCreateUserThread: %lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        ok(ResumeThread(Thread) == 1, "Rtl thread was not suspended\n");
        ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "Rtl thread wait failed\n");
        GetExitCodeThread(Thread, &Exit);
        ok(Exit == 0x3141, "Rtl thread exit %lx\n", Exit);
        CloseHandle(Thread);
    }

    LifetimeReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    LifetimeWait = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(LifetimeReady && LifetimeWait, "lifetime events failed\n");
    if (!LifetimeReady || !LifetimeWait) return;
    Thread = CreateThread(NULL, 0, LifetimeWorker, NULL, 0, NULL);
    ok(Thread != NULL, "lifetime worker creation failed\n");
    if (Thread)
    {
        ok(WaitForSingleObject(LifetimeReady, 10000) == WAIT_OBJECT_0, "worker not ready\n");
        ok(SuspendThread(Thread) == 0, "initial SuspendThread count\n");
        ok(SuspendThread(Thread) == 1, "nested SuspendThread count\n");
        ZeroMemory(&Context, sizeof(Context));
        Context.ContextFlags = CONTEXT_FULL;
        ok(GetThreadContext(Thread, &Context), "running thread context: %lu\n", GetLastError());
        ok(SetThreadContext(Thread, &Context), "restore running context: %lu\n", GetLastError());
        IsWow64Process(GetCurrentProcess(), &Guest);
        if (Guest)
        {
            WOW64_CONTEXT WowContext;
            WowGet = (void *)GetProcAddress(GetModuleHandleW(L"kernel32"), "Wow64GetThreadContext");
            WowSet = (void *)GetProcAddress(GetModuleHandleW(L"kernel32"), "Wow64SetThreadContext");
            ok(WowGet && WowSet, "WoW64 context exports missing\n");
            if (WowGet && WowSet)
            {
                ZeroMemory(&WowContext, sizeof(WowContext));
                WowContext.ContextFlags = WOW64_CONTEXT_FULL;
                ok(WowGet(Thread, &WowContext), "Wow64GetThreadContext: %lu\n", GetLastError());
                ok(WowSet(Thread, &WowContext), "Wow64SetThreadContext: %lu\n", GetLastError());
            }
        }
        ok(ResumeThread(Thread) == 2, "first ResumeThread count\n");
        ok(ResumeThread(Thread) == 1, "second ResumeThread count\n");
        ok(TerminateThread(Thread, 0x2718), "TerminateThread failed\n");
        ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "terminated thread wait failed\n");
        GetExitCodeThread(Thread, &Exit);
        ok(Exit == 0x2718, "terminated thread exit %lx\n", Exit);
        CloseHandle(Thread);
    }
    if (Guest)
    {
        CreateEx = (void *)GetProcAddress(GetModuleHandleW(L"ntdll"), "NtCreateThreadEx");
        ok(CreateEx != NULL, "NtCreateThreadEx export missing\n");
        if (CreateEx)
        {
            SetEvent(LifetimeWait);
            Status = CreateEx(&Thread, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(),
                               LifetimeWorker, (void *)0x1618, 1, 0, 0, 0, NULL);
            ok(Status == STATUS_SUCCESS, "NtCreateThreadEx: %lx\n", Status);
            if (NT_SUCCESS(Status))
            {
                ok(ResumeThread(Thread) == 1, "NtCreateThreadEx suspend count\n");
                ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "NtCreateThreadEx wait failed\n");
                GetExitCodeThread(Thread, &Exit);
                ok(Exit == 0x1618, "NtCreateThreadEx exit %lx\n", Exit);
                CloseHandle(Thread);
            }
            Status = CreateEx(&Thread, SYNCHRONIZE, NULL, GetCurrentProcess(),
                               LifetimeWorker, (void *)0x1619, 0, 0, 0, 0, NULL);
            ok(Status == STATUS_SUCCESS, "NtCreateThreadEx with wait-only access: %lx\n", Status);
            if (NT_SUCCESS(Status))
            {
                ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "wait-only thread did not run\n");
                SetLastError(0xdeadbeef);
                ok(!GetExitCodeThread(Thread, &Exit) && GetLastError() == ERROR_ACCESS_DENIED,
                   "creation granted unrequested query access: %lu\n", GetLastError());
                CloseHandle(Thread);
            }
        }
    }
    CloseHandle(LifetimeReady);
    CloseHandle(LifetimeWait);
}

static LONG ExitWorkers;

static DWORD WINAPI ExitWorker(void *Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    InterlockedIncrement(&ExitWorkers);
    Sleep(INFINITE);
    return 1;
}

START_TEST(Wow64ThreadExitProcess)
{
    HANDLE Threads[16];
    DWORD i, Start = GetTickCount();
    ExitWorkers = 0;
    for (i = 0; i < _countof(Threads); ++i)
    {
        Threads[i] = CreateThread(NULL, 0, ExitWorker, NULL, 0, NULL);
        ok(Threads[i] != NULL, "ExitProcess worker %lu creation failed\n", i);
        if (!Threads[i]) ExitProcess(1);
    }
    while (ExitWorkers != _countof(Threads) && GetTickCount() - Start < 10000) Sleep(1);
    ok(ExitWorkers == _countof(Threads), "ExitProcess workers did not start\n");
    if (ExitWorkers != _countof(Threads)) ExitProcess(1);
    /* The external harness checks this marker and process exit code zero.
     * Returning would not exercise process shutdown with live guest threads. */
    printf("Wow64ThreadExitProcess: exiting with 16 live workers\n");
    fflush(stdout);
    ExitProcess(0);
}
