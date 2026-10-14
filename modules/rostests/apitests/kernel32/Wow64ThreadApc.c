/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "precomp.h"
#include <pseh/pseh2.h>
#include <float.h>

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
    HANDLE File, Port;
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
    Port = CreateIoCompletionPort(File, NULL, (ULONG_PTR)0x89abcdef, 1);
    ok(Port != NULL, "completion port creation/association: %lu\n", GetLastError());
    if (Port)
    {
        ULONG_PTR Key;
        LPOVERLAPPED CompletedIo;
        BOOL Result;
        ok(PostQueuedCompletionStatus(Port, 17, (ULONG_PTR)0x87654321, &Io), "post completion failed\n");
        Result = GetQueuedCompletionStatus(Port, &Bytes, &Key, &CompletedIo, 10000);
        ok(Result && Bytes == 17 && Key == (ULONG_PTR)0x87654321 && CompletedIo == &Io,
           "posted packet fields changed: %lu %Iu %p\n", Bytes, Key, CompletedIo);
        ZeroMemory(&Io, sizeof(Io));
        Result = ReadFile(File, Output, sizeof(Output), NULL, &Io);
        ok(Result || GetLastError() == ERROR_IO_PENDING, "completion port read: %lu\n", GetLastError());
        Result = GetQueuedCompletionStatus(Port, &Bytes, &Key, &CompletedIo, 10000);
        ok(Result && Bytes == sizeof(Input) && Key == (ULONG_PTR)0x89abcdef && CompletedIo == &Io,
           "I/O packet fields changed: %lu %Iu %p\n", Bytes, Key, CompletedIo);
        ok(Io.Internal == STATUS_SUCCESS && Io.InternalHigh == sizeof(Input),
           "packet became visible before IOSB completion\n");
        CloseHandle(Port);
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

#ifdef _M_IX86
typedef struct
{
    PVOID Parent;
    DWORD Fls, Tls;
    LONG Cleanups;
    ULONG Stage;
} FIBER_TEST;

static VOID NTAPI FiberCleanup(PVOID Value)
{
    FIBER_TEST *Test = Value;
    InterlockedIncrement(&Test->Cleanups);
}

static VOID WINAPI FiberWorker(PVOID Parameter)
{
    FIBER_TEST *Test = Parameter;
    volatile LONG Handled = 0;
    BYTE Local;
    NT_TIB *Tib = (NT_TIB *)NtCurrentTeb();
    ok(GetFiberData() == Test, "fiber parameter changed\n");
    ok((PVOID)&Local >= Tib->StackLimit && (PVOID)&Local < Tib->StackBase,
       "fiber stack bounds exclude local variable\n");
    ok(FlsGetValue(Test->Fls) == NULL, "fiber inherited parent's FLS\n");
    ok(FlsSetValue(Test->Fls, Test), "fiber FLS assignment failed\n");
    ok(TlsGetValue(Test->Tls) == Test, "fiber lost thread TLS\n");
    _SEH2_TRY { RaiseException(0xe0424243, 0, 0, NULL); }
    _SEH2_EXCEPT(_SEH2_GetExceptionCode() == 0xe0424243 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { Handled = 1; }
    _SEH2_END;
    ok(Handled, "fiber SEH did not handle exception\n");
    Test->Stage = 1;
    SwitchToFiber(Test->Parent);
    ok(Test->Stage == 2 && FlsGetValue(Test->Fls) == Test, "fiber state lost across switch\n");
    Test->Stage = 3;
    SwitchToFiber(Test->Parent);
}

static DWORD WINAPI FiberThread(PVOID Parameter)
{
    FIBER_TEST Test = {0};
    PVOID Fiber;
    UNREFERENCED_PARAMETER(Parameter);
    Test.Fls = FlsAlloc(FiberCleanup);
    Test.Tls = TlsAlloc();
    ok(Test.Fls != FLS_OUT_OF_INDEXES && Test.Tls != TLS_OUT_OF_INDEXES,
       "fiber storage allocation failed\n");
    if (Test.Fls == FLS_OUT_OF_INDEXES || Test.Tls == TLS_OUT_OF_INDEXES) goto Done;
    ok(FlsSetValue(Test.Fls, &Test), "parent FLS assignment failed\n");
    ok(TlsSetValue(Test.Tls, &Test), "parent TLS assignment failed\n");
    Test.Parent = ConvertThreadToFiber(&Test);
    ok(Test.Parent != NULL, "convert thread to fiber: %lu\n", GetLastError());
    if (!Test.Parent) goto Done;
    Fiber = CreateFiber(0, FiberWorker, &Test);
    ok(Fiber != NULL, "create fiber: %lu\n", GetLastError());
    if (Fiber)
    {
        SwitchToFiber(Fiber);
        ok(Test.Stage == 1, "fiber did not yield\n");
        ok(FlsGetValue(Test.Fls) == &Test, "parent FLS changed\n");
        Test.Stage = 2;
        SwitchToFiber(Fiber);
        ok(Test.Stage == 3, "fiber did not resume\n");
        DeleteFiber(Fiber);
        ok(Test.Cleanups == 1, "deleted fiber cleanup count %ld\n", Test.Cleanups);
    }
    ok(ConvertFiberToThread(), "convert fiber to thread: %lu\n", GetLastError());
Done:
    if (Test.Fls != FLS_OUT_OF_INDEXES) FlsFree(Test.Fls);
    if (Test.Tls != TLS_OUT_OF_INDEXES) TlsFree(Test.Tls);
    return 0;
}

static VOID TestFiberContext(VOID)
{
    HANDLE Thread = CreateThread(NULL, 0, FiberThread, NULL, 0, NULL);
    DWORD Exit = ~0;
    ok(Thread != NULL, "fiber thread creation failed\n");
    if (!Thread) return;
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "fiber thread wait failed\n");
    GetExitCodeThread(Thread, &Exit);
    ok(Exit == 0, "fiber thread exit %lx\n", Exit);
    CloseHandle(Thread);
}

static LONG FloatingReady, FloatingStop;
static unsigned int FloatingResult;

static DWORD WINAPI FloatingWorker(void *Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    _control87(_PC_64, _MCW_PC);
    InterlockedExchange(&FloatingReady, 1);
    /* Suspend while executing guest instructions, rather than in a thunk. */
    while (!InterlockedCompareExchange(&FloatingStop, 0, 0)) { }
    FloatingResult = _control87(0, 0) & _MCW_PC;
    return 0;
}

static VOID TestFloatingContext(VOID)
{
    HANDLE Thread;
    CONTEXT Context;
    DWORD Start = GetTickCount();
    FloatingReady = FloatingStop = 0;
    Thread = CreateThread(NULL, 0, FloatingWorker, NULL, 0, NULL);
    ok(Thread != NULL, "floating worker creation failed\n");
    if (!Thread) return;
    while (!InterlockedCompareExchange(&FloatingReady, 0, 0) && GetTickCount() - Start < 10000) Sleep(1);
    ok(FloatingReady, "floating worker did not start\n");
    ok(SuspendThread(Thread) == 0, "floating worker suspension failed\n");
    ZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_FLOATING_POINT;
    ok(GetThreadContext(Thread, &Context), "get floating context: %lu\n", GetLastError());
    ok((Context.FloatSave.ControlWord & 0x300) == 0x300,
       "x87 precision was not captured: %lx\n", Context.FloatSave.ControlWord);
    Context.FloatSave.ControlWord = (Context.FloatSave.ControlWord & ~0x300) | 0x200;
    ok(SetThreadContext(Thread, &Context), "set floating context: %lu\n", GetLastError());
    InterlockedExchange(&FloatingStop, 1);
    ok(ResumeThread(Thread) == 1, "floating worker resume failed\n");
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "floating worker wait failed\n");
    ok(FloatingResult == _PC_53, "x87 precision was not restored: %x\n", FloatingResult);
    CloseHandle(Thread);
}
#endif

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
#ifdef _M_IX86
    TestFloatingContext();
    TestFiberContext();
#endif
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
