/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for thread exit cleanup of the fiber data in LdrShutdownThread
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define FIBER_PARAM ((PVOID)(ULONG_PTR)0x12345678)
#define NOT_REACHED_EXIT_CODE 0xBAD

static DWORD WatchedThreadId;
static LONG AccessViolationCount;
static ULONG_PTR LastFaultAddress;

static LONG CALLBACK
CountAccessViolations(_In_ PEXCEPTION_POINTERS ExceptionInfo)
{
    PEXCEPTION_RECORD Record = ExceptionInfo->ExceptionRecord;

    if (Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        GetCurrentThreadId() == WatchedThreadId)
    {
        InterlockedIncrement(&AccessViolationCount);
        if (Record->NumberParameters >= 2)
            LastFaultAddress = Record->ExceptionInformation[1];
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI
PlainThread(_In_ PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    return 11;
}

static DWORD WINAPI
FiberThread(_In_ PVOID Parameter)
{
    PVOID Fiber;

    UNREFERENCED_PARAMETER(Parameter);
    Fiber = ConvertThreadToFiber(FIBER_PARAM);
    if (!Fiber)
        return 0xF1;
    if (GetCurrentFiber() != Fiber || GetFiberData() != FIBER_PARAM)
        return 0xF2;
    return 22;
}

static DWORD WINAPI
FiberToThreadThread(_In_ PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    if (!ConvertThreadToFiber(FIBER_PARAM))
        return 0xF1;
    if (!ConvertFiberToThread())
        return 0xF3;
    return 33;
}

static VOID CALLBACK
UnusedFiberProc(_In_ PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
}

static DWORD WINAPI
DeleteOtherFiberThread(_In_ PVOID Parameter)
{
    PVOID Fiber;

    UNREFERENCED_PARAMETER(Parameter);
    Fiber = CreateFiber(0, UnusedFiberProc, NULL);
    if (!Fiber)
        return 0xF5;
    DeleteFiber(Fiber);
    return 44;
}

static DWORD WINAPI
DeleteCurrentFiberThread(_In_ PVOID Parameter)
{
    PVOID Fiber;

    UNREFERENCED_PARAMETER(Parameter);
    Fiber = ConvertThreadToFiber(FIBER_PARAM);
    if (!Fiber)
        return 0xF1;
    DeleteFiber(Fiber);
    return NOT_REACHED_EXIT_CODE;
}

/* No ok() here: the child process uses it too and reports through its exit code */
static BOOL
RunThread(_In_ LPTHREAD_START_ROUTINE StartRoutine, _Out_ PDWORD ExitCode, _Out_ PCSTR *Failure)
{
    HANDLE Thread;

    *ExitCode = 0xFFFFFFFF;
    Thread = CreateThread(NULL, 0, StartRoutine, NULL, CREATE_SUSPENDED, &WatchedThreadId);
    if (!Thread)
    {
        *Failure = "CreateThread";
        return FALSE;
    }
    if (ResumeThread(Thread) == (DWORD)-1)
    {
        *Failure = "ResumeThread";
        TerminateThread(Thread, 0xDEAD);
        CloseHandle(Thread);
        return FALSE;
    }
    if (WaitForSingleObject(Thread, 10000) != WAIT_OBJECT_0)
    {
        /* Leave the thread alone: it may hold the loader lock */
        *Failure = "WaitForSingleObject";
        return FALSE;
    }
    if (!GetExitCodeThread(Thread, ExitCode))
    {
        *Failure = "GetExitCodeThread";
        CloseHandle(Thread);
        return FALSE;
    }
    CloseHandle(Thread);
    WatchedThreadId = 0;
    return TRUE;
}

static BOOL
TestThreadExit(_In_ LPTHREAD_START_ROUTINE StartRoutine, _In_ DWORD ExpectedExitCode, _In_ PCSTR Name)
{
    ULONG i;
    DWORD ExitCode;
    PCSTR Failure = "";

    for (i = 0; i < 3; i++)
    {
        AccessViolationCount = 0;
        LastFaultAddress = 0;
        if (!RunThread(StartRoutine, &ExitCode, &Failure))
        {
            ok(0, "%s %lu: %s failed with %lu, stopping\n", Name, i, Failure, GetLastError());
            return FALSE;
        }
        ok(ExitCode == ExpectedExitCode, "%s %lu: exit code 0x%lx, expected 0x%lx\n",
           Name, i, ExitCode, ExpectedExitCode);
        ok(AccessViolationCount == 0, "%s %lu: %ld access violations in the exiting thread, last at %p\n",
           Name, i, AccessViolationCount, (PVOID)LastFaultAddress);
    }
    return TRUE;
}

#define CHILD_NO_HANDLER 0xE1
#define CHILD_THREAD_FAILED 0xE2
#define CHILD_ACCESS_VIOLATION 0xE3

static VOID
RunDeleteCurrentFiberChild(VOID)
{
    DWORD ExitCode;
    PVOID Handler;
    PCSTR Failure;

    Handler = AddVectoredExceptionHandler(1, CountAccessViolations);
    if (!Handler)
        ExitProcess(CHILD_NO_HANDLER);
    AccessViolationCount = 0;
    if (!RunThread(DeleteCurrentFiberThread, &ExitCode, &Failure))
        ExitProcess(CHILD_THREAD_FAILED);
    RemoveVectoredExceptionHandler(Handler);
    if (AccessViolationCount)
        ExitProcess(CHILD_ACCESS_VIOLATION);
    ExitProcess(ExitCode);
}

static VOID
TestDeleteCurrentFiber(VOID)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 64];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DWORD ExitCode = 0xFFFFFFFF;
    DWORD Wait;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" LdrShutdownThread deletefiber", FileName);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);
    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "WaitForSingleObject returned %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 0xDEAD);
        WaitForSingleObject(ProcessInfo.hProcess, 10000);
    }
    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed with %lu\n", GetLastError());
    CloseHandle(ProcessInfo.hProcess);

    /* The thread exits through ExitThread(1) inside DeleteFiber */
    ok(ExitCode == 1, "DeleteFiber of the current fiber: child exit code 0x%lx, expected 1\n", ExitCode);
}

START_TEST(LdrShutdownThread)
{
    PVOID Handler;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "deletefiber"))
    {
        RunDeleteCurrentFiberChild();
        return;
    }

    Handler = AddVectoredExceptionHandler(1, CountAccessViolations);
    ok(Handler != NULL, "AddVectoredExceptionHandler failed\n");
    if (!Handler)
        return;

    if (!TestThreadExit(PlainThread, 11, "plain thread") ||
        !TestThreadExit(FiberThread, 22, "fiber thread") ||
        !TestThreadExit(FiberToThreadThread, 33, "fiber converted back") ||
        !TestThreadExit(DeleteOtherFiberThread, 44, "delete other fiber"))
    {
        return;
    }

    RemoveVectoredExceptionHandler(Handler);

    TestDeleteCurrentFiber();
}
