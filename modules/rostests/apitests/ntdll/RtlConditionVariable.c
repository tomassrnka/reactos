/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Condition variables: no lost wake-ups, no stockpiled wake-ups, timeouts, lock state
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* A thread that sleeps on a condition variable has registered itself before it gives up the lock, so a
   thread that changes the state under the same lock and then wakes the variable must reach it, even
   when the sleeper has not yet blocked. The hand-over cases pass one item (or one epoch) at a time:
   the waker goes on only after the item was taken, so a wake-up that goes missing leaves every sleeper
   asleep with the item available, and the waker sees the item not being taken. The sleepers wait without
   a timeout; the waker's progress limit is the assertion. Windows loses none. The cases run in child
   processes, so that a case that hangs cannot take the rest of the test with it, and cannot go on
   using state that the next case would reset. */

#define ITEMS               1500
#define ACK_LIMIT_MS        20000
#define JOIN_LIMIT_MS       30000
#define SHORT_RUN_MS        2500
#define MAX_THREADS         8

typedef VOID (NTAPI *FN_SRW)(PRTL_SRWLOCK);
typedef VOID (NTAPI *FN_WAKE_CV)(PRTL_CONDITION_VARIABLE);
typedef NTSTATUS (NTAPI *FN_SLEEP_CV_SRW)(PRTL_CONDITION_VARIABLE, PRTL_SRWLOCK, PLARGE_INTEGER, ULONG);
typedef NTSTATUS (NTAPI *FN_SLEEP_CV_CS)(PRTL_CONDITION_VARIABLE, PRTL_CRITICAL_SECTION, PLARGE_INTEGER);

static FN_SRW pRtlAcquireSRWLockShared, pRtlReleaseSRWLockShared;
static FN_SRW pRtlAcquireSRWLockExclusive, pRtlReleaseSRWLockExclusive;
static FN_WAKE_CV pRtlWakeConditionVariable, pRtlWakeAllConditionVariable;
static FN_SLEEP_CV_SRW pRtlSleepConditionVariableSRW;
static FN_SLEEP_CV_CS pRtlSleepConditionVariableCS;

enum { LOCK_SRW_EXCLUSIVE, LOCK_SRW_SHARED, LOCK_CS };

static const PCSTR g_LockNames[] = { "SRW exclusive", "SRW shared", "critical section" };

static RTL_CONDITION_VARIABLE g_Cv;
static RTL_SRWLOCK g_Srw;
static RTL_CRITICAL_SECTION g_Cs;
static ULONG g_LockKind;

static volatile LONG g_Tokens, g_Epoch, g_Quit;
static volatile LONG g_Ack;
static HMODULE g_hDll;
static volatile LONG g_Stop, g_InLock, g_Violations;
static volatile LONG g_Success, g_Timeout, g_Unexpected, g_Wakes;

static VOID Lock(VOID)
{
    if (g_LockKind == LOCK_CS)
        RtlEnterCriticalSection(&g_Cs);
    else if (g_LockKind == LOCK_SRW_SHARED)
        pRtlAcquireSRWLockShared(&g_Srw);
    else
        pRtlAcquireSRWLockExclusive(&g_Srw);
}

static VOID Unlock(VOID)
{
    if (g_LockKind == LOCK_CS)
        RtlLeaveCriticalSection(&g_Cs);
    else if (g_LockKind == LOCK_SRW_SHARED)
        pRtlReleaseSRWLockShared(&g_Srw);
    else
        pRtlReleaseSRWLockExclusive(&g_Srw);
}

/* The waker changes the state under the exclusive lock, whatever the sleepers hold */
static VOID LockForChange(VOID)
{
    if (g_LockKind == LOCK_CS)
        RtlEnterCriticalSection(&g_Cs);
    else
        pRtlAcquireSRWLockExclusive(&g_Srw);
}

static VOID UnlockForChange(VOID)
{
    if (g_LockKind == LOCK_CS)
        RtlLeaveCriticalSection(&g_Cs);
    else
        pRtlReleaseSRWLockExclusive(&g_Srw);
}

/* Milliseconds == 0 is no timeout */
static NTSTATUS SleepFor(ULONG Milliseconds)
{
    LARGE_INTEGER Timeout;
    PLARGE_INTEGER pTimeout = NULL;

    if (Milliseconds != 0)
    {
        Timeout.QuadPart = -10000LL * Milliseconds;
        pTimeout = &Timeout;
    }
    if (g_LockKind == LOCK_CS)
        return pRtlSleepConditionVariableCS(&g_Cv, &g_Cs, pTimeout);
    return pRtlSleepConditionVariableSRW(&g_Cv, &g_Srw, pTimeout,
                                         g_LockKind == LOCK_SRW_SHARED ? RTL_CONDITION_VARIABLE_LOCKMODE_SHARED : 0);
}

/* Takes one token at a time; the wake-up is for one sleeper */
static DWORD WINAPI TokenThread(LPVOID Parameter)
{
    for (;;)
    {
        Lock();
        while (g_Tokens == 0 && !g_Quit)
            SleepFor(0);
        if (g_Tokens == 0)
        {
            Unlock();
            return 0;
        }
        InterlockedDecrement(&g_Tokens);
        InterlockedIncrement(&g_Ack);
        Unlock();
    }
}

/* Waits for every epoch; the wake-up is for all sleepers */
static DWORD WINAPI EpochThread(LPVOID Parameter)
{
    LONG Seen = 0;

    for (;;)
    {
        Lock();
        while (g_Epoch == Seen && !g_Quit)
            SleepFor(0);
        if (g_Epoch == Seen)
        {
            Unlock();
            return 0;
        }
        Seen = g_Epoch;
        InterlockedIncrement(&g_Ack);
        Unlock();
    }
}

/* Sleeps with timeouts of a few milliseconds, so that the timeouts run into the wake-ups */
static DWORD WINAPI ShortSleepThread(LPVOID Parameter)
{
    ULONG Seed = (ULONG)(ULONG_PTR)Parameter * 2654435761u + 1;
    NTSTATUS Status;

    while (!g_Stop)
    {
        Seed = Seed * 1664525u + 1013904223u;
        Lock();
        if (InterlockedIncrement(&g_InLock) != 1)
            InterlockedIncrement(&g_Violations);
        InterlockedDecrement(&g_InLock);
        Status = SleepFor(1 + (Seed >> 24) % 3);
        if (InterlockedIncrement(&g_InLock) != 1)
            InterlockedIncrement(&g_Violations);
        InterlockedDecrement(&g_InLock);
        if (Status == STATUS_TIMEOUT)
            InterlockedIncrement(&g_Timeout);
        else if (Status == STATUS_SUCCESS)
            InterlockedIncrement(&g_Success);
        else
            InterlockedIncrement(&g_Unexpected);
        Unlock();
    }
    return 0;
}

static DWORD WINAPI ShortWakeThread(LPVOID Parameter)
{
    ULONG Seed = 12345;

    while (!g_Stop)
    {
        Seed = Seed * 1664525u + 1013904223u;
        if ((Seed >> 24) % 4 == 0)
            pRtlWakeAllConditionVariable(&g_Cv);
        else
            pRtlWakeConditionVariable(&g_Cv);
        InterlockedIncrement(&g_Wakes);
        if ((Seed >> 20) % 8 == 0)
            SwitchToThread();
    }
    return 0;
}

static BOOL WaitForAck(LONG Expected)
{
    DWORD Start = GetTickCount();

    while (g_Ack < Expected)
    {
        if (GetTickCount() - Start > ACK_LIMIT_MS)
            return FALSE;
        SwitchToThread();
    }
    return TRUE;
}

static VOID JoinOrExit(PCSTR Name, ULONG Count, HANDLE *hThreads)
{
    ULONG i;

    /* Closing a handle does not end a thread: if they do not end, the process must go */
    if (WaitForMultipleObjects(Count, hThreads, TRUE, JOIN_LIMIT_MS) != WAIT_OBJECT_0)
    {
        ok(0, "%s: threads did not end in %d ms\n", Name, JOIN_LIMIT_MS);
        ExitProcess(1);
    }
    for (i = 0; i < Count; i++)
        CloseHandle(hThreads[i]);
}

static VOID RunHandOver(PCSTR Name, ULONG Kind, ULONG Threads, BOOL WakeAll)
{
    HANDLE hThreads[MAX_THREADS];
    ULONG i;
    LONG Round;

    g_LockKind = Kind;
    for (i = 0; i < Threads; i++)
    {
        hThreads[i] = CreateThread(NULL, 0, WakeAll ? EpochThread : TokenThread, NULL, 0, NULL);
        if (!hThreads[i])
        {
            ok(0, "%s: CreateThread failed with %lu\n", Name, GetLastError());
            ExitProcess(1);
        }
    }

    for (Round = 1; Round <= ITEMS; Round++)
    {
        LockForChange();
        if (WakeAll)
            g_Epoch++;
        else
            g_Tokens++;
        UnlockForChange();

        if (WakeAll)
            pRtlWakeAllConditionVariable(&g_Cv);
        else
            pRtlWakeConditionVariable(&g_Cv);

        if (!WaitForAck(WakeAll ? Round * (LONG)Threads : Round))
        {
            ok(0, "%s: round %ld was not taken in %d ms, a wake-up was lost\n", Name, Round, ACK_LIMIT_MS);
            ExitProcess(1);
        }
    }

    LockForChange();
    g_Quit = 1;
    UnlockForChange();
    for (i = 0; i < 200; i++)
    {
        pRtlWakeAllConditionVariable(&g_Cv);
        if (WaitForMultipleObjects(Threads, hThreads, TRUE, 100) == WAIT_OBJECT_0)
            break;
    }
    JoinOrExit(Name, Threads, hThreads);

    trace("%s: %ld rounds handed over\n", Name, Round - 1);
}

static VOID RunShortTimeouts(PCSTR Name, ULONG Kind)
{
    HANDLE hThreads[MAX_THREADS];
    ULONG i;

    g_LockKind = Kind;
    for (i = 0; i < 4; i++)
    {
        hThreads[i] = CreateThread(NULL, 0, ShortSleepThread, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
        if (!hThreads[i])
        {
            ok(0, "%s: CreateThread failed with %lu\n", Name, GetLastError());
            ExitProcess(1);
        }
    }
    hThreads[4] = CreateThread(NULL, 0, ShortWakeThread, NULL, 0, NULL);
    if (!hThreads[4])
    {
        ok(0, "%s: CreateThread failed with %lu\n", Name, GetLastError());
        ExitProcess(1);
    }

    Sleep(SHORT_RUN_MS);
    g_Stop = 1;
    for (i = 0; i < 200; i++)
    {
        pRtlWakeAllConditionVariable(&g_Cv);
        if (WaitForMultipleObjects(5, hThreads, TRUE, 100) == WAIT_OBJECT_0)
            break;
    }
    JoinOrExit(Name, 5, hThreads);

    trace("%s: %ld wake calls, %ld sleeps ended by a wake-up, %ld by their timeout\n",
          Name, g_Wakes, g_Success, g_Timeout);
    ok(g_Violations == 0, "%s: %ld times the caller's lock was not held on return from the sleep\n",
       Name, g_Violations);
    ok(g_Unexpected == 0, "%s: %ld sleeps returned a status other than success or timeout\n", Name, g_Unexpected);
    ok(g_Success + g_Timeout > 100, "%s: only %ld sleeps ended\n", Name, g_Success + g_Timeout);
}

/* A wake-up with nobody asleep is not kept for a sleeper that comes later, and a sleep that times out
   gives the lock back to its caller */
static VOID TestNoStockpile(ULONG Kind)
{
    NTSTATUS Status;
    DWORD Start;

    g_LockKind = Kind;
    RtlZeroMemory(&g_Cv, sizeof(g_Cv));
    pRtlWakeConditionVariable(&g_Cv);
    pRtlWakeAllConditionVariable(&g_Cv);

    Lock();
    Start = GetTickCount();
    Status = SleepFor(100);
    ok(Status == STATUS_TIMEOUT, "%s: sleep after wake-ups without sleepers returned 0x%lx\n",
       g_LockNames[Kind], Status);
    ok(GetTickCount() - Start >= 80, "%s: sleep returned after %lu ms\n", g_LockNames[Kind], GetTickCount() - Start);
    if (Kind == LOCK_CS)
    {
        ok(g_Cs.OwningThread == UlongToHandle(GetCurrentThreadId()),
           "%s: the sleep returned without the critical section\n", g_LockNames[Kind]);
    }
    Unlock();

    /* The lock can be taken again and no sleeper is left behind */
    Lock();
    Unlock();
    pRtlWakeAllConditionVariable(&g_Cv);
}

static VOID RunBasics(VOID)
{
    TestNoStockpile(LOCK_SRW_EXCLUSIVE);
    TestNoStockpile(LOCK_SRW_SHARED);
    TestNoStockpile(LOCK_CS);
}

static BOOL ResolveFunctions(PCWSTR DllName)
{
    HMODULE hDll = LoadLibraryW(DllName);

    if (!hDll)
        return FALSE;
    pRtlAcquireSRWLockShared = (FN_SRW)GetProcAddress(hDll, "RtlAcquireSRWLockShared");
    pRtlReleaseSRWLockShared = (FN_SRW)GetProcAddress(hDll, "RtlReleaseSRWLockShared");
    pRtlAcquireSRWLockExclusive = (FN_SRW)GetProcAddress(hDll, "RtlAcquireSRWLockExclusive");
    pRtlReleaseSRWLockExclusive = (FN_SRW)GetProcAddress(hDll, "RtlReleaseSRWLockExclusive");
    pRtlWakeConditionVariable = (FN_WAKE_CV)GetProcAddress(hDll, "RtlWakeConditionVariable");
    pRtlWakeAllConditionVariable = (FN_WAKE_CV)GetProcAddress(hDll, "RtlWakeAllConditionVariable");
    pRtlSleepConditionVariableSRW = (FN_SLEEP_CV_SRW)GetProcAddress(hDll, "RtlSleepConditionVariableSRW");
    pRtlSleepConditionVariableCS = (FN_SLEEP_CV_CS)GetProcAddress(hDll, "RtlSleepConditionVariableCS");
    if (pRtlAcquireSRWLockShared && pRtlReleaseSRWLockShared && pRtlAcquireSRWLockExclusive &&
        pRtlReleaseSRWLockExclusive && pRtlWakeConditionVariable && pRtlWakeAllConditionVariable &&
        pRtlSleepConditionVariableSRW && pRtlSleepConditionVariableCS)
    {
        g_hDll = hDll;
        return TRUE;
    }
    FreeLibrary(hDll);
    return FALSE;
}

static VOID RunChild(PCSTR Case)
{
    RtlInitializeCriticalSection(&g_Cs);

    if (!strcmp(Case, "basics"))
        RunBasics();
    else if (!strcmp(Case, "wakeone1"))
        RunHandOver("wake one, 1 sleeper, SRW exclusive", LOCK_SRW_EXCLUSIVE, 1, FALSE);
    else if (!strcmp(Case, "wakeone3"))
        RunHandOver("wake one, 3 sleepers, SRW exclusive", LOCK_SRW_EXCLUSIVE, 3, FALSE);
    else if (!strcmp(Case, "wakeone3cs"))
        RunHandOver("wake one, 3 sleepers, critical section", LOCK_CS, 3, FALSE);
    else if (!strcmp(Case, "wakeall"))
        RunHandOver("wake all, 3 sleepers, SRW exclusive", LOCK_SRW_EXCLUSIVE, 3, TRUE);
    else if (!strcmp(Case, "wakeallshared"))
        RunHandOver("wake all, 3 sleepers, SRW shared", LOCK_SRW_SHARED, 3, TRUE);
    else if (!strcmp(Case, "wakeallcs"))
        RunHandOver("wake all, 3 sleepers, critical section", LOCK_CS, 3, TRUE);
    else if (!strcmp(Case, "shortsrw"))
        RunShortTimeouts("short timeouts, SRW exclusive", LOCK_SRW_EXCLUSIVE);
    else if (!strcmp(Case, "shortcs"))
        RunShortTimeouts("short timeouts, critical section", LOCK_CS);
}

static VOID RunCase(PCSTR ExePath, PCSTR Case)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CHAR CmdLine[MAX_PATH + 64];

    StringCchPrintfA(CmdLine, _countof(CmdLine), "\"%s\" RtlConditionVariable %s", ExePath, Case);
    if (!CreateProcessA(NULL, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessA failed with %lu\n", GetLastError());
        return;
    }
    if (WaitForSingleObject(pi.hProcess, 180000) != WAIT_OBJECT_0)
    {
        ok(0, "%s case did not finish in 180 s\n", Case);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    winetest_wait_child_process(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

START_TEST(RtlConditionVariable)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    /* ReactOS exports them from ntdll only at NT 6 level; ntdll_vista always does */
    if (!ResolveFunctions(L"ntdll.dll") && !ResolveFunctions(L"ntdll_vista.dll"))
    {
        skip("Condition variable functions not exported\n");
        return;
    }
    if (argc >= 3)
    {
        RunChild(argv[2]);
        FreeLibrary(g_hDll);
        return;
    }

    RunCase(argv[0], "basics");
    RunCase(argv[0], "wakeone1");
    RunCase(argv[0], "wakeone3");
    RunCase(argv[0], "wakeone3cs");
    RunCase(argv[0], "wakeall");
    RunCase(argv[0], "wakeallshared");
    RunCase(argv[0], "wakeallcs");
    RunCase(argv[0], "shortsrw");
    RunCase(argv[0], "shortcs");

    FreeLibrary(g_hDll);
}
