/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Stress test for SRW locks: mutual exclusion, and the lifetime of the wait blocks that
 *              live on the stacks of the waiting threads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/* A thread that waits for an SRW lock queues a wait block on its own stack. When the lock is its own it
   returns, and its stack is used for other calls at once. The thread that released the lock for it must
   not touch the wait block after that. Threads that wait for the lock in a tight loop meet the unlucky
   timing within a second or two: a write into a returned thread's stack, or the next wait block of that
   thread being marked as granted, shows up as damage to the pattern each thread keeps in the stack area
   right below its caller, as a failed mutual exclusion check, or as an exception inside the lock code. */

#define CANARY_SIZE         512
#define CANARY_BYTE         0xA4
#define RUN_MS              2500
#define CV_ITEMS            1500
#define CV_QUEUE            8
#define CV_WAIT_MS          50

typedef VOID (NTAPI *FN_SRW)(PRTL_SRWLOCK);
typedef BOOLEAN (NTAPI *FN_TRY_SRW)(PRTL_SRWLOCK);
typedef VOID (NTAPI *FN_WAKE_CV)(PRTL_CONDITION_VARIABLE);
typedef NTSTATUS (NTAPI *FN_SLEEP_CV_SRW)(PRTL_CONDITION_VARIABLE, PRTL_SRWLOCK, PLARGE_INTEGER, ULONG);

static FN_SRW pRtlAcquireSRWLockShared, pRtlReleaseSRWLockShared;
static FN_SRW pRtlAcquireSRWLockExclusive, pRtlReleaseSRWLockExclusive;
static FN_TRY_SRW pRtlTryAcquireSRWLockShared, pRtlTryAcquireSRWLockExclusive;
static FN_WAKE_CV pRtlWakeConditionVariable, pRtlWakeAllConditionVariable;
static FN_SLEEP_CV_SRW pRtlSleepConditionVariableSRW;

static RTL_SRWLOCK g_Lock;
static volatile LONG g_Stop;
static volatile LONG g_Writers, g_Readers;
static volatile LONG g_Data, g_Updates;
static volatile LONG g_Violations, g_Corrupt, g_Faults;
static volatile LONG g_TryOk, g_TryFail;
static LARGE_INTEGER g_Freq;
static HANDLE g_Workers[8];

enum { MODE_EXCLUSIVE, MODE_SHARED, MODE_MIXED };

static VOID SpinMicros(ULONG Micros)
{
    LARGE_INTEGER Start, Now;

    QueryPerformanceCounter(&Start);
    do
    {
        YieldProcessor();
        QueryPerformanceCounter(&Now);
    } while ((Now.QuadPart - Start.QuadPart) * 1000000 / g_Freq.QuadPart < Micros);
}

static ULONG NextRandom(PULONG State)
{
    ULONG x = *State;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *State = x;
}

/* Must be called from the function that called the acquire function, so that the pattern covers the stack
   area that the acquire function used */
static DECLSPEC_NOINLINE VOID HoldWithCanary(ULONG HoldMicros)
{
    volatile UCHAR Canary[CANARY_SIZE];
    ULONG i;

    for (i = 0; i < CANARY_SIZE; i++)
        Canary[i] = CANARY_BYTE;

    if (HoldMicros != 0)
        SpinMicros(HoldMicros);

    for (i = 0; i < CANARY_SIZE; i++)
    {
        if (Canary[i] != CANARY_BYTE)
        {
            InterlockedIncrement(&g_Corrupt);
            break;
        }
    }
}

static VOID EnterExclusive(VOID)
{
    if (InterlockedIncrement(&g_Writers) != 1 || g_Readers != 0)
        InterlockedIncrement(&g_Violations);
}

static VOID LeaveExclusive(VOID)
{
    LONG Value;

    Value = g_Data;
    YieldProcessor();
    g_Data = Value + 1;
    InterlockedIncrement(&g_Updates);
    InterlockedDecrement(&g_Writers);
}

static VOID EnterShared(VOID)
{
    InterlockedIncrement(&g_Readers);
    if (g_Writers != 0)
        InterlockedIncrement(&g_Violations);
}

static VOID LeaveShared(VOID)
{
    LONG Before, After;

    Before = g_Data;
    YieldProcessor();
    After = g_Data;
    if (Before != After || g_Writers != 0)
        InterlockedIncrement(&g_Violations);
    InterlockedDecrement(&g_Readers);
}

static VOID LockedRelease(FN_SRW Release)
{
    _SEH2_TRY
    {
        Release(&g_Lock);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement(&g_Faults);
    }
    _SEH2_END;
}

static VOID OpExclusive(ULONG HoldMicros)
{
    volatile BOOL Locked = FALSE;

    _SEH2_TRY
    {
        pRtlAcquireSRWLockExclusive(&g_Lock);
        Locked = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement(&g_Faults);
    }
    _SEH2_END;
    if (!Locked)
        return;
    EnterExclusive();
    HoldWithCanary(HoldMicros);
    LeaveExclusive();
    LockedRelease(pRtlReleaseSRWLockExclusive);
}

static VOID OpShared(ULONG HoldMicros)
{
    volatile BOOL Locked = FALSE;

    _SEH2_TRY
    {
        pRtlAcquireSRWLockShared(&g_Lock);
        Locked = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedIncrement(&g_Faults);
    }
    _SEH2_END;
    if (!Locked)
        return;
    EnterShared();
    HoldWithCanary(HoldMicros);
    LeaveShared();
    LockedRelease(pRtlReleaseSRWLockShared);
}

static DWORD WINAPI WorkerThread(LPVOID Parameter)
{
    ULONG Mode = (ULONG)(ULONG_PTR)Parameter & 0xFF;
    ULONG Seed = ((ULONG)(ULONG_PTR)Parameter >> 8) * 2654435761u + 12345;
    ULONG Roll;

    while (!g_Stop)
    {
        Roll = NextRandom(&Seed) % 100;
        if (Mode == MODE_EXCLUSIVE)
            OpExclusive(0);
        else if (Mode == MODE_SHARED)
            OpShared(0);
        else if (Roll < 30)
            OpExclusive(Roll % 5);
        else if (Roll < 65)
            OpShared(Roll % 7);
        else if (Roll < 80 && pRtlTryAcquireSRWLockShared)
        {
            if (pRtlTryAcquireSRWLockShared(&g_Lock))
            {
                InterlockedIncrement(&g_TryOk);
                EnterShared();
                HoldWithCanary(1);
                LeaveShared();
                LockedRelease(pRtlReleaseSRWLockShared);
            }
            else
                InterlockedIncrement(&g_TryFail);
        }
        else if (Roll < 95 && pRtlTryAcquireSRWLockExclusive)
        {
            if (pRtlTryAcquireSRWLockExclusive(&g_Lock))
            {
                InterlockedIncrement(&g_TryOk);
                EnterExclusive();
                HoldWithCanary(1);
                LeaveExclusive();
                LockedRelease(pRtlReleaseSRWLockExclusive);
            }
            else
                InterlockedIncrement(&g_TryFail);
        }
        else
            SwitchToThread();
    }
    return 0;
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
    pRtlTryAcquireSRWLockShared = (FN_TRY_SRW)GetProcAddress(hDll, "RtlTryAcquireSRWLockShared");
    pRtlTryAcquireSRWLockExclusive = (FN_TRY_SRW)GetProcAddress(hDll, "RtlTryAcquireSRWLockExclusive");
    pRtlWakeConditionVariable = (FN_WAKE_CV)GetProcAddress(hDll, "RtlWakeConditionVariable");
    pRtlWakeAllConditionVariable = (FN_WAKE_CV)GetProcAddress(hDll, "RtlWakeAllConditionVariable");
    pRtlSleepConditionVariableSRW = (FN_SLEEP_CV_SRW)GetProcAddress(hDll, "RtlSleepConditionVariableSRW");
    /* The try functions are optional: ntdll_vista.dll, which ReactOS uses below the NT 6.1 level, lacks one */
    return pRtlAcquireSRWLockShared && pRtlReleaseSRWLockShared && pRtlAcquireSRWLockExclusive &&
           pRtlReleaseSRWLockExclusive && pRtlWakeConditionVariable && pRtlWakeAllConditionVariable &&
           pRtlSleepConditionVariableSRW;
}

/* One lock, Count workers; the run ends at the first failure, since later results only repeat it */
static VOID RunLockCase(PCSTR Name, ULONG Count, const ULONG *Modes)
{
    ULONG i, Waited = 0;
    BOOL Hung;

    RtlZeroMemory(&g_Lock, sizeof(g_Lock));
    g_Stop = g_Writers = g_Readers = g_Data = g_Updates = 0;
    g_Violations = g_Corrupt = g_Faults = g_TryOk = g_TryFail = 0;

    for (i = 0; i < Count; i++)
    {
        g_Workers[i] = CreateThread(NULL, 0, WorkerThread, (LPVOID)(ULONG_PTR)(Modes[i] | ((i + 1) << 8)), 0, NULL);
        ok(g_Workers[i] != NULL, "%s: CreateThread failed with %lu\n", Name, GetLastError());
        if (!g_Workers[i])
            ExitProcess(1);
    }

    while (Waited < RUN_MS && g_Violations + g_Corrupt + g_Faults == 0)
    {
        Sleep(20);
        Waited += 20;
    }
    g_Stop = 1;

    Hung = WaitForMultipleObjects(Count, g_Workers, TRUE, 30000) != WAIT_OBJECT_0;
    ok(!Hung, "%s: workers still running 30 s after the stop, the lock is stuck\n", Name);

    trace("%s: %ld updates, %ld try ok, %ld try failed\n", Name, g_Updates, g_TryOk, g_TryFail);
    ok(g_Faults == 0, "%s: %ld exceptions inside the lock functions\n", Name, g_Faults);
    ok(g_Corrupt == 0, "%s: %ld times a thread's stack was written to after its acquire had returned\n",
       Name, g_Corrupt);
    ok(g_Violations == 0, "%s: %ld mutual exclusion violations\n", Name, g_Violations);
    if (Hung)
        ExitProcess(1);
    ok(g_Data == g_Updates, "%s: protected counter is %ld, expected %ld\n", Name, g_Data, g_Updates);
}

/* Condition variables, with the SRW lock in both modes. A wait ends at its timeout at the latest, so the
   test does not depend on every wake-up arriving */
static RTL_SRWLOCK g_QueueLock;
static RTL_CONDITION_VARIABLE g_NotEmpty, g_NotFull, g_Epoch;
static LONG g_Queue[CV_QUEUE];
static LONG g_QHead, g_QTail, g_QCount, g_Produced, g_Done, g_EpochValue;
static volatile LONG g_Sum, g_Taken, g_Timeouts, g_EpochSeen;

static VOID SleepOn(PRTL_CONDITION_VARIABLE Cv, ULONG Flags)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10000LL * CV_WAIT_MS;
    if (pRtlSleepConditionVariableSRW(Cv, &g_QueueLock, &Timeout, Flags) == STATUS_TIMEOUT)
        InterlockedIncrement(&g_Timeouts);
}

static DWORD WINAPI ProducerThread(LPVOID Parameter)
{
    LONG Item;

    for (;;)
    {
        pRtlAcquireSRWLockExclusive(&g_QueueLock);
        while (g_QCount == CV_QUEUE && g_Produced < CV_ITEMS)
            SleepOn(&g_NotFull, 0);
        if (g_Produced == CV_ITEMS)
        {
            pRtlReleaseSRWLockExclusive(&g_QueueLock);
            break;
        }
        Item = ++g_Produced;
        g_Queue[g_QTail] = Item;
        g_QTail = (g_QTail + 1) % CV_QUEUE;
        g_QCount++;
        g_EpochValue++;
        pRtlReleaseSRWLockExclusive(&g_QueueLock);
        pRtlWakeConditionVariable(&g_NotEmpty);
        if (Item % 16 == 0)
            pRtlWakeAllConditionVariable(&g_Epoch);
    }
    return 0;
}

static DWORD WINAPI ConsumerThread(LPVOID Parameter)
{
    LONG Item;

    for (;;)
    {
        pRtlAcquireSRWLockExclusive(&g_QueueLock);
        while (g_QCount == 0 && !g_Done)
            SleepOn(&g_NotEmpty, 0);
        if (g_QCount == 0)
        {
            pRtlReleaseSRWLockExclusive(&g_QueueLock);
            break;
        }
        Item = g_Queue[g_QHead];
        g_QHead = (g_QHead + 1) % CV_QUEUE;
        g_QCount--;
        pRtlReleaseSRWLockExclusive(&g_QueueLock);
        pRtlWakeConditionVariable(&g_NotFull);
        InterlockedExchangeAdd(&g_Sum, Item);
        InterlockedIncrement(&g_Taken);
    }
    return 0;
}

/* Waits in shared mode for the epoch to change, until the production is over */
static DWORD WINAPI EpochThread(LPVOID Parameter)
{
    LONG Seen = 0;

    for (;;)
    {
        pRtlAcquireSRWLockShared(&g_QueueLock);
        while (g_EpochValue == Seen && !g_Done)
            SleepOn(&g_Epoch, RTL_CONDITION_VARIABLE_LOCKMODE_SHARED);
        if (g_EpochValue == Seen)
        {
            pRtlReleaseSRWLockShared(&g_QueueLock);
            break;
        }
        Seen = g_EpochValue;
        pRtlReleaseSRWLockShared(&g_QueueLock);
        InterlockedIncrement(&g_EpochSeen);
    }
    return 0;
}

static VOID RunCondVarCase(VOID)
{
    HANDLE hThreads[6];
    ULONG i;
    DWORD Wait;
    LONG Expected = CV_ITEMS * (CV_ITEMS + 1) / 2;

    hThreads[0] = CreateThread(NULL, 0, ConsumerThread, NULL, 0, NULL);
    hThreads[1] = CreateThread(NULL, 0, ConsumerThread, NULL, 0, NULL);
    hThreads[2] = CreateThread(NULL, 0, EpochThread, NULL, 0, NULL);
    hThreads[3] = CreateThread(NULL, 0, EpochThread, NULL, 0, NULL);
    hThreads[4] = CreateThread(NULL, 0, ProducerThread, NULL, 0, NULL);
    hThreads[5] = CreateThread(NULL, 0, ProducerThread, NULL, 0, NULL);
    for (i = 0; i < _countof(hThreads); i++)
    {
        ok(hThreads[i] != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (!hThreads[i])
            ExitProcess(1);
    }

    /* The producers end first; then the consumers and the epoch waiters are told that production is over */
    Wait = WaitForMultipleObjects(2, &hThreads[4], TRUE, 120000);
    ok(Wait == WAIT_OBJECT_0, "Producers did not finish: %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        ExitProcess(1);
    pRtlAcquireSRWLockExclusive(&g_QueueLock);
    g_Done = 1;
    pRtlReleaseSRWLockExclusive(&g_QueueLock);
    pRtlWakeAllConditionVariable(&g_NotEmpty);
    pRtlWakeAllConditionVariable(&g_Epoch);
    Wait = WaitForMultipleObjects(4, hThreads, TRUE, 120000);
    ok(Wait == WAIT_OBJECT_0, "Consumers or epoch waiters did not finish: %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        ExitProcess(1);

    trace("condvar: %ld waits timed out\n", g_Timeouts);
    ok(g_Produced == CV_ITEMS, "Produced %ld items, expected %d\n", g_Produced, CV_ITEMS);
    ok(g_Taken == CV_ITEMS, "Consumed %ld items, expected %d\n", g_Taken, CV_ITEMS);
    ok(g_Sum == Expected, "Consumed sum is %ld, expected %ld\n", g_Sum, Expected);
    ok(g_QCount == 0, "%ld items left in the queue\n", g_QCount);
    ok(g_EpochSeen > 0, "The shared-mode waiters never saw an epoch change\n");
    pRtlAcquireSRWLockExclusive(&g_QueueLock);
    pRtlReleaseSRWLockExclusive(&g_QueueLock);
    if (pRtlTryAcquireSRWLockExclusive)
    {
        ok(pRtlTryAcquireSRWLockExclusive(&g_QueueLock), "Lock is still owned after all threads ended\n");
        pRtlReleaseSRWLockExclusive(&g_QueueLock);
    }
}

static VOID RunChild(PCSTR Case)
{
    static const ULONG Writers[] = { MODE_EXCLUSIVE, MODE_EXCLUSIVE };
    static const ULONG WriterReader[] = { MODE_EXCLUSIVE, MODE_SHARED };
    static const ULONG WriterReaders[] = { MODE_EXCLUSIVE, MODE_SHARED, MODE_SHARED };
    static const ULONG WriterManyReaders[] = { MODE_EXCLUSIVE, MODE_SHARED, MODE_SHARED, MODE_SHARED, MODE_SHARED, MODE_SHARED };
    static const ULONG Mixed[] = { MODE_MIXED, MODE_MIXED, MODE_MIXED, MODE_MIXED };

    QueryPerformanceFrequency(&g_Freq);

    if (!strcmp(Case, "writers"))
        RunLockCase(Case, _countof(Writers), Writers);
    else if (!strcmp(Case, "writerreader"))
        RunLockCase(Case, _countof(WriterReader), WriterReader);
    else if (!strcmp(Case, "writerreaders"))
        RunLockCase(Case, _countof(WriterReaders), WriterReaders);
    else if (!strcmp(Case, "manyreaders"))
        RunLockCase(Case, _countof(WriterManyReaders), WriterManyReaders);
    else if (!strcmp(Case, "mixed"))
        RunLockCase(Case, _countof(Mixed), Mixed);
    else if (!strcmp(Case, "condvar"))
        RunCondVarCase();
}

static VOID RunCase(PCSTR ExePath, PCSTR Case)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CHAR CmdLine[MAX_PATH + 64];

    StringCchPrintfA(CmdLine, _countof(CmdLine), "\"%s\" RtlSRWLockStress %s", ExePath, Case);
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

START_TEST(RtlSRWLockStress)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    /* ReactOS exports them from ntdll only at NT 6 level; ntdll_vista always does */
    if (!ResolveFunctions(L"ntdll.dll") && !ResolveFunctions(L"ntdll_vista.dll"))
    {
        skip("SRW lock and condition variable functions not exported\n");
        return;
    }
    if (argc >= 3)
    {
        RunChild(argv[2]);
        return;
    }
    RunCase(argv[0], "writers");
    RunCase(argv[0], "writerreader");
    RunCase(argv[0], "writerreaders");
    RunCase(argv[0], "manyreaders");
    RunCase(argv[0], "mixed");
    RunCase(argv[0], "condvar");
}
