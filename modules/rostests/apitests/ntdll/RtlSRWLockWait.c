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
static HANDLE g_hHeld;
static volatile LONG g_Sink;
static ULONG g_WorkLoops;

/* Work that needs processor time, not wall time */
static VOID Work(ULONG Loops)
{
    ULONG i;
    LONG Value = 0;

    for (i = 0; i < Loops; i++)
        Value += (LONG)(i ^ (Value >> 3));
    g_Sink = Value;
}

static DWORD WINAPI OwnerThread(LPVOID Parameter)
{
    pRtlAcquireSRWLockExclusive(&g_Lock);
    SetEvent(g_hHeld);
    Work(g_WorkLoops);
    pRtlReleaseSRWLockExclusive(&g_Lock);
    return 0;
}

static DWORD WINAPI WaiterThread(LPVOID Parameter)
{
    if (Parameter)
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

/* One processor: the owner runs below normal priority and needs about WorkMs of
   processor time while a normal-priority thread waits for the lock */
static VOID TestStarvation(BOOL Shared, DWORD WorkMs)
{
    HANDLE hOwner, hWaiter;
    DWORD Start, Elapsed, Wait;

    pRtlInitializeSRWLock(&g_Lock);
    ResetEvent(g_hHeld);
    hOwner = CreateThread(NULL, 0, OwnerThread, NULL, CREATE_SUSPENDED, NULL);
    hWaiter = CreateThread(NULL, 0, WaiterThread, (LPVOID)(ULONG_PTR)Shared, CREATE_SUSPENDED, NULL);
    ok(hOwner && hWaiter, "CreateThread failed with %lu\n", GetLastError());
    if (!hOwner || !hWaiter)
        return;
    SetThreadPriority(hOwner, THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadPriority(hWaiter, THREAD_PRIORITY_NORMAL);
    ResumeThread(hOwner);
    ok(WaitForSingleObject(g_hHeld, 10000) == WAIT_OBJECT_0, "Owner did not take the lock\n");

    Start = GetTickCount();
    ResumeThread(hWaiter);
    Wait = WaitForSingleObject(hWaiter, 15000);
    Elapsed = GetTickCount() - Start;
    ok(Wait == WAIT_OBJECT_0, "%s waiter still waiting after %lu ms (owner needs about %lu ms)\n",
       Shared ? "Shared" : "Exclusive", Elapsed, WorkMs);
    if (Wait == WAIT_OBJECT_0)
    {
        ok(Elapsed < WorkMs * 4 + 1000, "%s waiter took %lu ms (owner needs about %lu ms)\n",
           Shared ? "Shared" : "Exclusive", Elapsed, WorkMs);
    }
    else
    {
        /* Do not leave a thread spinning on the lock behind */
        Wait = WaitForSingleObject(hWaiter, 120000);
        trace("Waiter %s after %lu ms\n", Wait == WAIT_OBJECT_0 ? "finished" : "still waiting", GetTickCount() - Start);
    }
    WaitForSingleObject(hOwner, 120000);
    CloseHandle(hOwner);
    CloseHandle(hWaiter);
}

START_TEST(RtlSRWLockWait)
{
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    DWORD_PTR ProcessMask, SystemMask;
    DWORD Start, WorkMs;
    int OldPriority;

    pRtlInitializeSRWLock = (FN_RtlSRWLock)GetProcAddress(hNtdll, "RtlInitializeSRWLock");
    pRtlAcquireSRWLockExclusive = (FN_RtlSRWLock)GetProcAddress(hNtdll, "RtlAcquireSRWLockExclusive");
    pRtlReleaseSRWLockExclusive = (FN_RtlSRWLock)GetProcAddress(hNtdll, "RtlReleaseSRWLockExclusive");
    pRtlAcquireSRWLockShared = (FN_RtlSRWLock)GetProcAddress(hNtdll, "RtlAcquireSRWLockShared");
    pRtlReleaseSRWLockShared = (FN_RtlSRWLock)GetProcAddress(hNtdll, "RtlReleaseSRWLockShared");
    if (!pRtlInitializeSRWLock || !pRtlAcquireSRWLockExclusive || !pRtlReleaseSRWLockExclusive ||
        !pRtlAcquireSRWLockShared || !pRtlReleaseSRWLockShared)
    {
        skip("SRW lock functions not exported\n");
        return;
    }
    if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask) ||
        !SetProcessAffinityMask(GetCurrentProcess(), ProcessMask & (~ProcessMask + 1)))
    {
        skip("Cannot restrict the process to one processor\n");
        return;
    }
    /* This thread reports even while the others compete for the processor */
    OldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_hHeld = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* Calibrate the owner's work to about 100 ms */
    g_WorkLoops = 1000000;
    for (;;)
    {
        Start = GetTickCount();
        Work(g_WorkLoops);
        WorkMs = GetTickCount() - Start;
        if (WorkMs >= 50 || g_WorkLoops >= 0x40000000)
            break;
        g_WorkLoops *= 2;
    }
    trace("%lu loops take %lu ms\n", g_WorkLoops, WorkMs);

    TestStarvation(FALSE, WorkMs);
    TestStarvation(TRUE, WorkMs);

    CloseHandle(g_hHeld);
    SetThreadPriority(GetCurrentThread(), OldPriority);
    SetProcessAffinityMask(GetCurrentProcess(), ProcessMask);
}
