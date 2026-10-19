/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     An SRW lock waiter must let a lower-priority owner run
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef VOID (NTAPI *FN_RtlSRWLock)(PRTL_SRWLOCK);

static FN_RtlSRWLock pRtlInitializeSRWLock, pRtlAcquireSRWLockExclusive, pRtlReleaseSRWLockExclusive;
static FN_RtlSRWLock pRtlAcquireSRWLockShared, pRtlReleaseSRWLockShared;
static RTL_SRWLOCK g_Lock;
static HANDLE g_hTrying;
static BOOL g_Shared;
static volatile ULONG g_Sink;
static ULONG g_WorkLoops;

/* Work that needs processor time, not wall time */
static VOID Work(ULONG Loops)
{
    ULONG i, Value = 0;

    for (i = 0; i < Loops; i++)
        Value += i ^ (Value >> 3);
    g_Sink = Value;
}

static ULONGLONG ThreadCpuMs(HANDLE hThread)
{
    FILETIME Create, Exit, Kernel, User;

    if (!GetThreadTimes(hThread, &Create, &Exit, &Kernel, &User))
        return 0;
    return ((((ULONGLONG)Kernel.dwHighDateTime << 32) | Kernel.dwLowDateTime) +
            (((ULONGLONG)User.dwHighDateTime << 32) | User.dwLowDateTime)) / 10000;
}

static DWORD WINAPI OwnerThread(LPVOID Parameter)
{
    /* Start the work only when the waiter is about to wait */
    WaitForSingleObject(g_hTrying, INFINITE);
    Work(g_WorkLoops);
    pRtlReleaseSRWLockExclusive(&g_Lock);
    return 0;
}

static DWORD WINAPI WaiterThread(LPVOID Parameter)
{
    SetEvent(g_hTrying);
    if (g_Shared)
    {
        pRtlAcquireSRWLockShared(&g_Lock);
        pRtlReleaseSRWLockShared(&g_Lock);
    }
    else
    {
        pRtlAcquireSRWLockExclusive(&g_Lock);
        pRtlReleaseSRWLockExclusive(&g_Lock);
    }
    return 0;
}

static BOOL ResolveFunctions(PCWSTR DllName)
{
    HMODULE hDll = LoadLibraryW(DllName);

    if (!hDll)
        return FALSE;
    pRtlInitializeSRWLock = (FN_RtlSRWLock)GetProcAddress(hDll, "RtlInitializeSRWLock");
    pRtlAcquireSRWLockExclusive = (FN_RtlSRWLock)GetProcAddress(hDll, "RtlAcquireSRWLockExclusive");
    pRtlReleaseSRWLockExclusive = (FN_RtlSRWLock)GetProcAddress(hDll, "RtlReleaseSRWLockExclusive");
    pRtlAcquireSRWLockShared = (FN_RtlSRWLock)GetProcAddress(hDll, "RtlAcquireSRWLockShared");
    pRtlReleaseSRWLockShared = (FN_RtlSRWLock)GetProcAddress(hDll, "RtlReleaseSRWLockShared");
    return pRtlInitializeSRWLock && pRtlAcquireSRWLockExclusive && pRtlReleaseSRWLockExclusive &&
           pRtlAcquireSRWLockShared && pRtlReleaseSRWLockShared;
}

/* Child process, one processor: the main thread holds the lock and hands it to an owner thread
   below normal priority that needs WorkMs of processor time; a normal-priority thread waits.
   The waiter must not use the processor the owner needs. */
static VOID RunChild(BOOL Shared)
{
    DWORD_PTR ProcessMask, SystemMask;
    HANDLE hOwner, hWaiter;
    ULONGLONG CpuStart, WorkMs, WaiterMs;
    DWORD Wait;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask) ||
        !SetProcessAffinityMask(GetCurrentProcess(), ProcessMask & (~ProcessMask + 1)))
    {
        skip("Cannot restrict the process to one processor\n");
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    /* Calibrate the owner's work to at least 50 ms of processor time */
    for (g_WorkLoops = 1000000; ; g_WorkLoops *= 2)
    {
        CpuStart = ThreadCpuMs(GetCurrentThread());
        Work(g_WorkLoops);
        WorkMs = ThreadCpuMs(GetCurrentThread()) - CpuStart;
        if (WorkMs >= 50 || g_WorkLoops >= 0x40000000)
            break;
    }

    g_Shared = Shared;
    g_hTrying = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(g_hTrying != NULL, "CreateEventW failed with %lu\n", GetLastError());
    if (!g_hTrying)
        return;
    pRtlInitializeSRWLock(&g_Lock);
    /* The owner thread releases a lock this thread took: SRW locks have no owner identity */
    pRtlAcquireSRWLockExclusive(&g_Lock);
    hOwner = CreateThread(NULL, 0, OwnerThread, NULL, CREATE_SUSPENDED, NULL);
    hWaiter = CreateThread(NULL, 0, WaiterThread, NULL, CREATE_SUSPENDED, NULL);
    if (!hOwner || !hWaiter ||
        !SetThreadPriority(hOwner, THREAD_PRIORITY_BELOW_NORMAL) ||
        !SetThreadPriority(hWaiter, THREAD_PRIORITY_NORMAL))
    {
        ok(0, "Thread setup failed with %lu\n", GetLastError());
        ExitProcess(1);
    }
    ResumeThread(hOwner);
    ResumeThread(hWaiter);

    Wait = WaitForSingleObject(hWaiter, 60000);
    ok(Wait == WAIT_OBJECT_0, "%s waiter still waiting after 60 s\n", Shared ? "Shared" : "Exclusive");
    if (Wait != WAIT_OBJECT_0)
        ExitProcess(1);
    WaiterMs = ThreadCpuMs(hWaiter);
    trace("%s: owner needs %I64u ms, waiter used %I64u ms of processor time\n",
          Shared ? "Shared" : "Exclusive", WorkMs, WaiterMs);
    ok(WaiterMs < WorkMs, "%s waiter used %I64u ms of processor time, the owner needs %I64u ms\n",
       Shared ? "Shared" : "Exclusive", WaiterMs, WorkMs);
    WaitForSingleObject(hOwner, 60000);
}

static VOID RunCase(PCSTR ExePath, PCSTR Case)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CHAR CmdLine[MAX_PATH + 64];

    StringCchPrintfA(CmdLine, _countof(CmdLine), "\"%s\" RtlSRWLockWait %s", ExePath, Case);
    if (!CreateProcessA(NULL, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessA failed with %lu\n", GetLastError());
        return;
    }
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0)
    {
        ok(0, "%s case did not finish in 120 s\n", Case);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    winetest_wait_child_process(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

START_TEST(RtlSRWLockWait)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    /* ReactOS exports them from ntdll only at NT 6 level; ntdll_vista always does */
    if (!ResolveFunctions(L"ntdll.dll") && !ResolveFunctions(L"ntdll_vista.dll"))
    {
        skip("SRW lock functions not exported\n");
        return;
    }
    if (argc >= 3)
    {
        RunChild(!strcmp(argv[2], "shared"));
        return;
    }
    RunCase(argv[0], "exclusive");
    RunCase(argv[0], "shared");
}
