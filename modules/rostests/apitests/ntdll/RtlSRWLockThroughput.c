/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SRW lock throughput under contention, and waiters that must not starve the lock owner
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* Several threads take one lock in a tight loop. A lock that hands the lock over in FIFO order to a
   waiter that has gone to sleep pays one wake-up per operation, a convoy that drops the rate by orders
   of magnitude without any thread being idle for long: ReactOS with sleeps in timer ticks reached 1000 to
   65000 operations per second for three to six threads. The floors below are far under what a lock that
   spins and then blocks reaches (ReactOS: 1 to 17 million operations per second; Windows Server 2008 R2
   and Windows 10: 40 to 240 million) and far over such a convoy. The lock is measured in three windows
   and the best one counts, so that a stall of the virtual machine does not fail the test, while a convoy,
   which lasts, does. A thread that is starved shows as a count of (nearly) zero in that window. Each case
   runs in a child process, so that a case that hangs cannot take the rest of the test with it. */

#define WINDOW_MS               400
#define WINDOWS                 3
#define MAX_THREADS             8

/* Total operations per second for all threads, in the best window */
#define FLOOR_EXCLUSIVE         100000
#define FLOOR_MIXED             100000
/* Operations that every single thread must complete in that window */
#define FLOOR_PER_THREAD        50

typedef VOID (NTAPI *FN_RtlSRWLock)(PRTL_SRWLOCK);

static FN_RtlSRWLock pRtlInitializeSRWLock, pRtlAcquireSRWLockExclusive, pRtlReleaseSRWLockExclusive;
static FN_RtlSRWLock pRtlAcquireSRWLockShared, pRtlReleaseSRWLockShared;

static RTL_SRWLOCK g_Lock;
static volatile LONG g_Go, g_Stop, g_Ready;
static volatile LONG g_Data;
static BOOL g_SharedReaders;
static struct { volatile LONG Ops; CHAR Pad[60]; } g_Count[MAX_THREADS];

static DWORD WINAPI ContendThread(LPVOID Parameter)
{
    ULONG Id = (ULONG)(ULONG_PTR)Parameter;

    InterlockedIncrement(&g_Ready);
    while (!g_Go)
        YieldProcessor();

    while (!g_Stop)
    {
        if (g_SharedReaders && Id != 0)
        {
            pRtlAcquireSRWLockShared(&g_Lock);
            (void)g_Data;
            pRtlReleaseSRWLockShared(&g_Lock);
        }
        else
        {
            pRtlAcquireSRWLockExclusive(&g_Lock);
            g_Data++;
            pRtlReleaseSRWLockExclusive(&g_Lock);
        }
        g_Count[Id].Ops++;
    }
    return 0;
}

static VOID RunThroughput(PCSTR Name, ULONG Threads, BOOL SharedReaders, ULONG Floor)
{
    HANDLE hThreads[MAX_THREADS];
    LONG Before[MAX_THREADS];
    ULONG i, w, Start, End;
    ULONGLONG Total, PerSecond, BestPerSecond = 0;
    LONG Delta, Min, BestMin = 0;

    pRtlInitializeSRWLock(&g_Lock);
    g_Go = g_Stop = g_Ready = g_Data = 0;
    g_SharedReaders = SharedReaders;
    RtlZeroMemory(g_Count, sizeof(g_Count));

    for (i = 0; i < Threads; i++)
    {
        hThreads[i] = CreateThread(NULL, 0, ContendThread, (LPVOID)(ULONG_PTR)i, 0, NULL);
        if (!hThreads[i])
        {
            ok(0, "%s: CreateThread failed with %lu\n", Name, GetLastError());
            ExitProcess(1);
        }
    }
    while (g_Ready < (LONG)Threads)
        Sleep(1);

    g_Go = 1;
    for (w = 0; w < WINDOWS; w++)
    {
        for (i = 0; i < Threads; i++)
            Before[i] = g_Count[i].Ops;
        Start = GetTickCount();
        Sleep(WINDOW_MS);
        End = GetTickCount();

        Total = 0;
        Min = MAXLONG;
        for (i = 0; i < Threads; i++)
        {
            Delta = g_Count[i].Ops - Before[i];
            Total += Delta;
            if (Delta < Min)
                Min = Delta;
        }
        PerSecond = Total * 1000 / (End - Start);
        if (PerSecond >= BestPerSecond)
        {
            BestPerSecond = PerSecond;
            BestMin = Min;
        }
    }
    g_Stop = 1;

    if (WaitForMultipleObjects(Threads, hThreads, TRUE, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "%s: threads did not stop\n", Name);
        ExitProcess(1);
    }
    for (i = 0; i < Threads; i++)
        CloseHandle(hThreads[i]);

    trace("%s: %lu threads, %I64u operations per second, least busy thread %ld operations in %d ms\n",
          Name, Threads, BestPerSecond, BestMin, WINDOW_MS);
    ok(BestPerSecond >= Floor, "%s: %I64u operations per second, expected at least %lu\n", Name, BestPerSecond, Floor);
    ok(BestMin >= FLOOR_PER_THREAD, "%s: a thread completed only %ld operations in %d ms\n", Name, BestMin, WINDOW_MS);
}

/* Work that needs processor time, not wall time */
static volatile ULONG g_Sink;

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

#define WAITERS     4

static HANDLE g_hTrying;
static volatile LONG g_Trying;
static ULONG g_WorkLoops;

static DWORD WINAPI OwnerThread(LPVOID Parameter)
{
    /* Start the work only when all the waiters are about to wait */
    WaitForSingleObject(g_hTrying, INFINITE);
    Work(g_WorkLoops);
    pRtlReleaseSRWLockExclusive(&g_Lock);
    return 0;
}

static DWORD WINAPI WaiterThread(LPVOID Parameter)
{
    if (InterlockedIncrement(&g_Trying) == WAITERS)
        SetEvent(g_hTrying);

    if ((ULONG_PTR)Parameter & 1)
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

/* The lowest Count processors of the process affinity mask, or 0 if there are fewer */
static DWORD_PTR LowestProcessors(ULONG Count)
{
    DWORD_PTR ProcessMask, SystemMask, Bit, Picked = 0;
    ULONG Found = 0;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask))
        return 0;
    for (Bit = 1; Bit != 0 && Found < Count; Bit <<= 1)
    {
        if (ProcessMask & Bit)
        {
            Picked |= Bit;
            Found++;
        }
    }
    return Found == Count ? Picked : 0;
}

/* Child process, one processor: the main thread holds the lock and hands it to an owner thread below
   normal priority that needs WorkMs of processor time; WAITERS normal-priority threads (exclusive and
   shared mixed) wait. Together they must use less processor time than the owner needs. */
static VOID RunStarve(VOID)
{
    DWORD_PTR Mask = LowestProcessors(1);
    HANDLE hOwner, hWaiters[WAITERS];
    ULONGLONG CpuStart, WorkMs, WaitersMs = 0;
    ULONG i;

    if (!Mask || !SetProcessAffinityMask(GetCurrentProcess(), Mask))
    {
        skip("Cannot restrict the process to one processor\n");
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    for (g_WorkLoops = 1000000; ; g_WorkLoops *= 2)
    {
        CpuStart = ThreadCpuMs(GetCurrentThread());
        Work(g_WorkLoops);
        WorkMs = ThreadCpuMs(GetCurrentThread()) - CpuStart;
        if (WorkMs >= 50 || g_WorkLoops >= 0x40000000)
            break;
    }

    g_hTrying = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_hTrying)
    {
        ok(0, "CreateEventW failed with %lu\n", GetLastError());
        ExitProcess(1);
    }
    pRtlInitializeSRWLock(&g_Lock);
    pRtlAcquireSRWLockExclusive(&g_Lock);
    hOwner = CreateThread(NULL, 0, OwnerThread, NULL, CREATE_SUSPENDED, NULL);
    if (!hOwner || !SetThreadPriority(hOwner, THREAD_PRIORITY_BELOW_NORMAL))
    {
        ok(0, "Owner setup failed with %lu\n", GetLastError());
        ExitProcess(1);
    }
    for (i = 0; i < WAITERS; i++)
    {
        hWaiters[i] = CreateThread(NULL, 0, WaiterThread, (LPVOID)(ULONG_PTR)(i & 1), CREATE_SUSPENDED, NULL);
        if (!hWaiters[i] || !SetThreadPriority(hWaiters[i], THREAD_PRIORITY_NORMAL))
        {
            ok(0, "Waiter setup failed with %lu\n", GetLastError());
            ExitProcess(1);
        }
    }
    ResumeThread(hOwner);
    for (i = 0; i < WAITERS; i++)
        ResumeThread(hWaiters[i]);

    if (WaitForMultipleObjects(WAITERS, hWaiters, TRUE, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "Waiters still waiting after 60 s\n");
        ExitProcess(1);
    }
    for (i = 0; i < WAITERS; i++)
        WaitersMs += ThreadCpuMs(hWaiters[i]);
    trace("owner needs %I64u ms, %d waiters used %I64u ms of processor time together\n", WorkMs, WAITERS, WaitersMs);
    ok(WaitersMs < WorkMs, "%d waiters used %I64u ms of processor time, the owner needs %I64u ms\n",
       WAITERS, WaitersMs, WorkMs);
    if (WaitForSingleObject(hOwner, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "The owner did not finish in 60 s\n");
        ExitProcess(1);
    }
    for (i = 0; i < WAITERS; i++)
        CloseHandle(hWaiters[i]);
    CloseHandle(hOwner);
    CloseHandle(g_hTrying);
}

/* Child process on the lowest Processors processors (at most four): threads as many as them, then one more */
static VOID RunContend(PCSTR Case)
{
    DWORD_PTR Mask;
    ULONG Processors = 4;

    while (Processors >= 2 && !(Mask = LowestProcessors(Processors)))
        Processors--;
    if (Processors < 2 || !SetProcessAffinityMask(GetCurrentProcess(), Mask))
    {
        skip("Contention needs at least two processors\n");
        return;
    }

    if (!strcmp(Case, "exclusive"))
        RunThroughput("exclusive", Processors, FALSE, FLOOR_EXCLUSIVE);
    else if (!strcmp(Case, "oversubscribed"))
        RunThroughput("exclusive, one more thread than processors", Processors + 1, FALSE, FLOOR_EXCLUSIVE / 4);
    else if (!strcmp(Case, "readers"))
        RunThroughput("one writer, shared readers", Processors, TRUE, FLOOR_MIXED);
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
    if (pRtlInitializeSRWLock && pRtlAcquireSRWLockExclusive && pRtlReleaseSRWLockExclusive &&
        pRtlAcquireSRWLockShared && pRtlReleaseSRWLockShared)
    {
        return TRUE;
    }
    FreeLibrary(hDll);
    return FALSE;
}

static VOID RunCase(PCSTR ExePath, PCSTR Case)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CHAR CmdLine[MAX_PATH + 64];

    StringCchPrintfA(CmdLine, _countof(CmdLine), "\"%s\" RtlSRWLockThroughput %s", ExePath, Case);
    if (!CreateProcessA(NULL, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessA failed with %lu\n", GetLastError());
        return;
    }
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0)
    {
        ok(0, "The %s case did not finish in 120 s\n", Case);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    winetest_wait_child_process(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

START_TEST(RtlSRWLockThroughput)
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
        if (!strcmp(argv[2], "starve"))
            RunStarve();
        else
            RunContend(argv[2]);
        return;
    }

    RunCase(argv[0], "exclusive");
    RunCase(argv[0], "oversubscribed");
    RunCase(argv[0], "readers");
    RunCase(argv[0], "starve");
}
