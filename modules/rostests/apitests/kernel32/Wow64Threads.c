/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "precomp.h"
#include <pseh/pseh2.h>

static DWORD Tls;
static LONG Started, Raised;
static HANDLE StartEvent;
static BOOL Guest;
static DWORD Fls;
static LONG FlsFreed;
static VOID NTAPI FlsCleanup(void *Value)
{
    if (Value) InterlockedIncrement(&FlsFreed);
}

static DWORD WINAPI Worker(void *Parameter)
{
    ULONG_PTR Value = (ULONG_PTR)Parameter;
    volatile LONG Handled = 0;
    NT_TIB *Tib = (NT_TIB *)NtCurrentTeb();
    BYTE Local;
    ok(Tib->Self == Tib, "TIB self mismatch\n");
    ok((PVOID)&Local < Tib->StackBase && (PVOID)&Local >= Tib->StackLimit,
       "stack bounds do not contain local variable\n");
#ifdef _M_IX86
    ok(Tib->ExceptionList != NULL, "missing SEH chain\n");
#endif
    ok(TlsGetValue(Tls) == NULL, "TLS inherited another thread's value\n");
    ok(TlsSetValue(Tls, (PVOID)Value), "TlsSetValue failed\n");
    if (Fls != FLS_OUT_OF_INDEXES) ok(FlsSetValue(Fls, (PVOID)Value), "FlsSetValue failed\n");
    if (Guest)
    {
        PVOID Old, Nested;
        ok(Wow64DisableWow64FsRedirection(&Old), "disable failed\n");
        ok(Old == NULL, "new thread inherited redirection token %p\n", Old);
        ok(Wow64DisableWow64FsRedirection(&Nested), "nested disable failed\n");
        ok(Nested != NULL, "nested token is zero\n");
        ok(Wow64RevertWow64FsRedirection(Nested), "nested revert failed\n");
        ok(Wow64RevertWow64FsRedirection(Old), "revert failed\n");
    }
    InterlockedIncrement(&Started);
    ok(WaitForSingleObject(StartEvent, 10000) == WAIT_OBJECT_0, "start wait failed\n");
    _SEH2_TRY
    {
        RaiseException(0xe0424242, 0, 0, NULL);
    }
    _SEH2_EXCEPT(_SEH2_GetExceptionCode() == 0xe0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        Handled = 1;
    }
    _SEH2_END;
    ok(Handled, "secondary thread did not handle exception\n");
    if (Handled) InterlockedIncrement(&Raised);
    ok(TlsGetValue(Tls) == (PVOID)Value, "TLS lost across wait/exception\n");
    return (DWORD)Value;
}

static DWORD WINAPI SimpleWorker(void *Parameter)
{
    InterlockedIncrement(&Started);
    return (DWORD)(ULONG_PTR)Parameter;
}

START_TEST(Wow64Threads)
{
    HANDLE Threads[16], Process;
    DWORD Id, Exit, Count, Round, i;
    BOOL Result;
    PVOID OldRedirection = NULL;
    HMODULE Dll;
    VOID (WINAPI *Notifications)(PLONG, PLONG);
    LONG Attached, Detached;
    CONTEXT Context;
    IsWow64Process(GetCurrentProcess(), &Guest);
    Dll = LoadLibraryA("wow64_thread.dll");
    ok(Dll != NULL, "notification DLL missing: %lu\n", GetLastError());
    Notifications = Dll ? (void *)GetProcAddress(Dll, "ThreadNotifications") : NULL;
    Started = Raised = 0;
    Threads[0] = CreateThread(NULL, 0, SimpleWorker, (void *)0x1234, CREATE_SUSPENDED, &Id);
    ok(Threads[0] != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Threads[0]) return;
    ok(Started == 0, "suspended thread ran\n");
    ZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_FULL;
    Result = GetThreadContext(Threads[0], &Context);
    ok(Result, "GetThreadContext before resume: %lu\n", GetLastError());
#ifdef _M_IX86
    if (Result)
    {
        ok(Context.Eax == (DWORD)(ULONG_PTR)SimpleWorker && Context.Ebx == 0x1234,
           "initial start eax %lx ebx %lx\n", Context.Eax, Context.Ebx);
        Context.Ebx = 0x1235;
        ok(SetThreadContext(Threads[0], &Context), "SetThreadContext: %lu\n", GetLastError());
    }
#endif
    ok(ResumeThread(Threads[0]) == 1, "bad initial suspend count\n");
    ok(WaitForSingleObject(Threads[0], 10000) == WAIT_OBJECT_0, "thread did not finish\n");
    Result = GetExitCodeThread(Threads[0], &Exit);
    ok(Result && Exit ==
#ifdef _M_IX86
       0x1235
#else
       0x1234
#endif
       , "exit code %lu, error %lu\n", Exit, GetLastError());
    CloseHandle(Threads[0]);
    Process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId());
    ok(Process != NULL, "OpenProcess failed\n");
    if (Process)
    {
        Threads[0] = CreateRemoteThread(Process, NULL, 0, SimpleWorker, (void *)0x5678, 0, &Id);
        ok(Threads[0] != NULL, "CreateRemoteThread failed: %lu\n", GetLastError());
        if (Threads[0])
        {
            ok(WaitForSingleObject(Threads[0], 10000) == WAIT_OBJECT_0, "remote wait failed\n");
            GetExitCodeThread(Threads[0], &Exit);
            ok(Exit == 0x5678, "remote exit %lu\n", Exit);
            CloseHandle(Threads[0]);
        }
        CloseHandle(Process);
    }
    Tls = TlsAlloc();
    ok(Tls != TLS_OUT_OF_INDEXES, "TlsAlloc failed\n");
    if (Tls == TLS_OUT_OF_INDEXES) return;
    TlsSetValue(Tls, (void *)0xdeadbeef);
    StartEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(StartEvent != NULL, "CreateEvent failed\n");
    if (!StartEvent) { TlsFree(Tls); return; }
    Fls = FlsAlloc(FlsCleanup);
    ok(Fls != FLS_OUT_OF_INDEXES, "FlsAlloc failed\n");
    FlsFreed = 0;
    if (Guest) ok(Wow64DisableWow64FsRedirection(&OldRedirection), "parent disable failed\n");
    Started = 0;
    for (Round = 0; Round < 16; ++Round)
    {
        ResetEvent(StartEvent);
        for (Count = 0; Count < _countof(Threads); ++Count)
        {
            Threads[Count] = CreateThread(NULL, 0, Worker, (void *)(ULONG_PTR)(Count + 1), 0, NULL);
            ok(Threads[Count] != NULL, "round %lu thread %lu: %lu\n", Round, Count, GetLastError());
            if (!Threads[Count]) break;
        }
        SetEvent(StartEvent);
        if (Count) ok(WaitForMultipleObjects(Count, Threads, TRUE, 30000) == WAIT_OBJECT_0, "stress wait failed\n");
        for (i = 0; i < Count; ++i)
        {
            GetExitCodeThread(Threads[i], &Exit);
            ok(Exit == i + 1, "round %lu thread %lu exit %lu\n", Round, i, Exit);
            CloseHandle(Threads[i]);
        }
        if (Count != _countof(Threads)) break;
    }
    ok(Started == 256 && Raised == 256, "started %ld raised %ld\n", Started, Raised);
    ok(TlsGetValue(Tls) == (void *)0xdeadbeef, "parent TLS changed\n");
    if (Guest)
    {
        PVOID Old;
        ok(Wow64DisableWow64FsRedirection(&Old), "parent query failed\n");
        ok(Old != NULL, "worker reset parent redirection\n");
        Wow64RevertWow64FsRedirection(OldRedirection);
    }
    if (Notifications)
    {
        Notifications(&Attached, &Detached);
        ok(Attached == 258 && Detached == 258, "DLL notifications %ld/%ld\n", Attached, Detached);
    }
    ok(FlsFreed == 256, "FLS callbacks %ld\n", FlsFreed);
    if (Fls != FLS_OUT_OF_INDEXES) FlsFree(Fls);
    CloseHandle(StartEvent);
    TlsFree(Tls);
    if (Dll) FreeLibrary(Dll);
}
