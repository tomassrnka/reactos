/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Tests for the thread injection of QueueUserWorkItem and persistent work items
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */
#include "precomp.h"
#include <tlhelp32.h>

static volatile LONG s_Running;
static volatile LONG s_Started;
static volatile LONG s_Done;
static volatile LONG s_MaxRunning;
static HANDLE s_Gate;
static LONG s_BarrierCount;

static int
CountThreads(void)
{
    THREADENTRY32 Entry;
    HANDLE Snapshot;
    DWORD Pid = GetCurrentProcessId();
    int Count = 0;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return -1;
    Entry.dwSize = sizeof(Entry);
    if (Thread32First(Snapshot, &Entry))
    {
        do
        {
            if (Entry.th32OwnerProcessID == Pid)
                Count++;
            Entry.dwSize = sizeof(Entry);
        } while (Thread32Next(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
    return Count;
}

static void
ResetCounters(void)
{
    s_Running = s_Started = s_Done = s_MaxRunning = 0;
}

static void
EnterItem(void)
{
    LONG Running = InterlockedIncrement(&s_Running), Max;

    InterlockedIncrement(&s_Started);
    while ((Max = s_MaxRunning) < Running &&
           InterlockedCompareExchange(&s_MaxRunning, Running, Max) != Max)
        ;
}

static void
LeaveItem(void)
{
    InterlockedDecrement(&s_Running);
    InterlockedIncrement(&s_Done);
}

/* Blocks until the gate opens */
static DWORD WINAPI
GateItem(PVOID Context)
{
    EnterItem();
    WaitForSingleObject(s_Gate, INFINITE);
    LeaveItem();
    return 0;
}

/* Returns only when all items of the barrier run at the same time */
static DWORD WINAPI
BarrierItem(PVOID Context)
{
    EnterItem();
    if (InterlockedIncrement(&s_BarrierCount) == (LONG)(LONG_PTR)Context)
        SetEvent(s_Gate);
    WaitForSingleObject(s_Gate, INFINITE);
    LeaveItem();
    return 0;
}

static BOOL
WaitDone(LONG Count, DWORD Timeout)
{
    DWORD Start = GetTickCount();

    while (s_Done < Count)
    {
        if (GetTickCount() - Start > Timeout)
            return FALSE;
        Sleep(20);
    }
    return TRUE;
}

/* A flood of blocking items: the pool starts some threads at once, then adds
   them gradually, and keeps adding while the items block */
static void
TestBlockingFlood(ULONG Flags, LONG Processors)
{
    LONG Items, i, StartedEarly, StartedLater;
    int ThreadsBefore, ThreadsEarly;
    DWORD Start, EarlyMs, LaterMs;
    BOOL Ret;

    ResetCounters();
    s_Gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(s_Gate != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!s_Gate)
        return;

    ThreadsBefore = CountThreads();

    /* More items than the pool may start in the time measured, on any guest */
    Items = max(96, 4 * Processors + 32);
    Items = max(Items, ThreadsBefore + Processors + 64);

    Start = GetTickCount();
    for (i = 0; i < Items; i++)
    {
        Ret = QueueUserWorkItem(GateItem, NULL, Flags);
        ok(Ret, "QueueUserWorkItem(0x%lx) %ld failed: %lu\n", Flags, i, GetLastError());
        if (!Ret)
            break;
    }

    Sleep(700);
    StartedEarly = s_Started;
    ThreadsEarly = CountThreads() - ThreadsBefore;
    EarlyMs = GetTickCount() - Start;

    /* Windows 10 runs a thread per processor at once and adds about 6 a second.
       Allow twice that rate over the time that really passed, so a loaded host
       does not matter. Idle workers left by earlier tests (at most ThreadsBefore)
       take items at once. The old pool started every item at once. */
#define FLOOD_GROWTH(Ms) (Processors + 4 + (LONG)((Ms) / 80))
    ok(StartedEarly >= min(Processors, 2), "0x%lx: %ld items started after %lu ms\n", Flags, StartedEarly, EarlyMs);
    ok(StartedEarly <= ThreadsBefore + FLOOD_GROWTH(EarlyMs),
       "0x%lx: %ld of %ld items started after %lu ms (%d threads before), the pool grows too fast\n",
       Flags, StartedEarly, Items, EarlyMs, ThreadsBefore);
    ok(ThreadsEarly <= FLOOD_GROWTH(EarlyMs), "0x%lx: %d threads added after %lu ms\n", Flags, ThreadsEarly, EarlyMs);

    /* The items block: injection goes on, more of them start */
    Sleep(2000);
    while (s_Started <= StartedEarly && GetTickCount() - Start < 8000)
        Sleep(50);
    StartedLater = s_Started;
    LaterMs = GetTickCount() - Start;
    ok(StartedLater > StartedEarly, "0x%lx: no item started between %lu ms and %lu ms (%ld)\n",
       Flags, EarlyMs, LaterMs, StartedLater);
    ok(StartedLater <= ThreadsBefore + FLOOD_GROWTH(LaterMs) && StartedLater < Items,
       "0x%lx: %ld of %ld items started within %lu ms (%d threads before)\n",
       Flags, StartedLater, Items, LaterMs, ThreadsBefore);
#undef FLOOD_GROWTH
    trace("0x%lx: %d threads before, %ld items, started %ld after %lu ms (+%d threads), %ld after %lu ms, %ld processors\n",
          Flags, ThreadsBefore, Items, StartedEarly, EarlyMs, ThreadsEarly, StartedLater, LaterMs, Processors);

    SetEvent(s_Gate);
    ok(WaitDone(i, 30000), "0x%lx: only %ld of %ld items done\n", Flags, s_Done, i);
    Sleep(100);
    CloseHandle(s_Gate);
    s_Gate = NULL;
}

/* Items that wait for each other: all of them must get a thread eventually,
   more than the idle workers there are */
static void
TestBarrier(ULONG Flags)
{
    LONG Items = CountThreads() + 12;
    DWORD Start, Elapsed;
    LONG i;
    BOOL Ret;

    ResetCounters();
    s_BarrierCount = 0;
    s_Gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(s_Gate != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!s_Gate)
        return;

    Start = GetTickCount();
    for (i = 0; i < Items; i++)
    {
        Ret = QueueUserWorkItem(BarrierItem, (PVOID)(LONG_PTR)Items, Flags);
        ok(Ret, "QueueUserWorkItem(0x%lx) %ld failed: %lu\n", Flags, i, GetLastError());
        if (!Ret)
        {
            /* Release whoever waits */
            SetEvent(s_Gate);
            break;
        }
    }

    ok(WaitDone(i, 30000), "0x%lx: barrier of %ld items: only %ld started, %ld done after 30 s\n",
       Flags, i, s_Started, s_Done);
    Elapsed = GetTickCount() - Start;
    ok(s_MaxRunning == i, "0x%lx: max running %ld, expected %ld\n", Flags, s_MaxRunning, i);
    trace("0x%lx: barrier of %ld items done in %lu ms\n", Flags, i, Elapsed);
    Sleep(100);
    CloseHandle(s_Gate);
    s_Gate = NULL;
}

/* Long functions get a thread each, at once */
static void
TestLongFunctions(void)
{
    const LONG Items = 32;
    DWORD Start;
    LONG i;
    BOOL Ret;

    ResetCounters();
    s_Gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(s_Gate != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!s_Gate)
        return;

    for (i = 0; i < Items; i++)
    {
        Ret = QueueUserWorkItem(GateItem, NULL, WT_EXECUTELONGFUNCTION);
        ok(Ret, "QueueUserWorkItem(WT_EXECUTELONGFUNCTION) %ld failed: %lu\n", i, GetLastError());
        if (!Ret)
            break;
    }

    Start = GetTickCount();
    while (s_Started < i && GetTickCount() - Start < 3000)
        Sleep(20);
    ok(s_Started == i, "%ld of %ld long functions started within %lu ms\n", s_Started, i, GetTickCount() - Start);

    SetEvent(s_Gate);
    ok(WaitDone(i, 10000), "only %ld of %ld long functions done\n", s_Done, i);
    Sleep(100);
    CloseHandle(s_Gate);
    s_Gate = NULL;
}

/* Short items: a small pool keeps up, it does not grow */
static DWORD WINAPI
ShortItem(PVOID Context)
{
    EnterItem();
    Sleep(20);
    LeaveItem();
    return 0;
}

static void
TestShortItems(LONG Processors)
{
    const LONG Items = 400;
    LONG i;
    BOOL Ret;

    ResetCounters();
    for (i = 0; i < Items; i++)
    {
        Ret = QueueUserWorkItem(ShortItem, NULL, WT_EXECUTEDEFAULT);
        ok(Ret, "QueueUserWorkItem %ld failed: %lu\n", i, GetLastError());
        if (!Ret)
            break;
        if (i % 40 == 39)
            Sleep(100);
    }
    ok(WaitDone(i, 30000), "only %ld of %ld short items done\n", s_Done, i);
    ok(s_MaxRunning <= Processors + 16, "short items: %ld ran at the same time\n", s_MaxRunning);
    trace("short items: max %ld at the same time\n", s_MaxRunning);
}

/* Persistent items */
static DWORD s_PersistentTid[8];
static DWORD s_PersistentSleepRet;
static volatile LONG s_ApcRan;
static DWORD s_ApcTid;

static VOID CALLBACK
PersistentApc(ULONG_PTR Param)
{
    s_ApcTid = GetCurrentThreadId();
    InterlockedExchange(&s_ApcRan, 1);
}

static DWORD WINAPI
PersistentItem(PVOID Context)
{
    LONG Index = (LONG)(LONG_PTR)Context;

    EnterItem();
    s_PersistentTid[Index] = GetCurrentThreadId();
    if (Index == 0)
        s_PersistentSleepRet = SleepEx(300, TRUE);
    else if (Index == 3)
        QueueUserAPC(PersistentApc, GetCurrentThread(), 0);
    else
        Sleep(50);
    LeaveItem();
    return 0;
}

static VOID CALLBACK
PersistentTimer(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    *(DWORD *)Context = GetCurrentThreadId();
}

static void
TestPersistent(void)
{
    DWORD TimerTid = 0;
    HANDLE Timer;
    LONG i;
    BOOL Ret;

    ResetCounters();
    for (i = 0; i < 3; i++)
    {
        Ret = QueueUserWorkItem(PersistentItem, (PVOID)(LONG_PTR)i, WT_EXECUTEINPERSISTENTTHREAD);
        ok(Ret, "QueueUserWorkItem(WT_EXECUTEINPERSISTENTTHREAD) %ld failed: %lu\n", i, GetLastError());
        if (!Ret)
            return;
    }
    ok(WaitDone(3, 5000), "only %ld of 3 persistent items done\n", s_Done);
    ok(s_MaxRunning == 1, "persistent items: %ld ran at the same time\n", s_MaxRunning);
    ok(s_PersistentTid[0] != 0 && s_PersistentTid[0] == s_PersistentTid[1] && s_PersistentTid[1] == s_PersistentTid[2],
       "persistent items ran on threads %lx %lx %lx\n", s_PersistentTid[0], s_PersistentTid[1], s_PersistentTid[2]);
    /* The next item did not run nested in the alertable wait of the first */
    ok(s_PersistentSleepRet == 0, "SleepEx in a persistent item returned %lu\n", s_PersistentSleepRet);

    /* The thread stays, it runs a later item too, and it waits alertably when idle */
    Sleep(1500);
    s_ApcRan = 0;
    Ret = QueueUserWorkItem(PersistentItem, (PVOID)(LONG_PTR)3, WT_EXECUTEINPERSISTENTTHREAD);
    ok(Ret, "QueueUserWorkItem(WT_EXECUTEINPERSISTENTTHREAD) failed: %lu\n", GetLastError());
    ok(WaitDone(4, 5000), "the later persistent item did not run\n");
    ok(s_PersistentTid[3] == s_PersistentTid[0], "later persistent item ran on thread %lx, not %lx\n",
       s_PersistentTid[3], s_PersistentTid[0]);
    for (i = 0; i < 100 && !s_ApcRan; i++)
        Sleep(20);
    ok(s_ApcRan == 1, "APC queued to the persistent thread did not run\n");
    ok(s_ApcTid == s_PersistentTid[0], "APC ran on thread %lx\n", s_ApcTid);

    /* Timer queue timers with WT_EXECUTEINPERSISTENTTHREAD fire */
    Ret = CreateTimerQueueTimer(&Timer, NULL, PersistentTimer, &TimerTid, 50, 0,
                                WT_EXECUTEINPERSISTENTTHREAD | WT_EXECUTEONLYONCE);
    ok(Ret, "CreateTimerQueueTimer failed: %lu\n", GetLastError());
    if (Ret)
    {
        for (i = 0; i < 100 && !TimerTid; i++)
            Sleep(20);
        ok(TimerTid != 0, "persistent timer callback did not run\n");
        ok(DeleteTimerQueueTimer(NULL, Timer, INVALID_HANDLE_VALUE), "DeleteTimerQueueTimer failed: %lu\n", GetLastError());
    }
}

START_TEST(QueueUserWorkItem)
{
    SYSTEM_INFO Info;
    LONG Processors;

    GetSystemInfo(&Info);
    Processors = Info.dwNumberOfProcessors;

    /* The order matters: the tests that bound the pool run while it is small */
    TestPersistent();
    TestShortItems(Processors);
    TestBlockingFlood(WT_EXECUTEDEFAULT, Processors);
    TestBlockingFlood(WT_EXECUTEINIOTHREAD, Processors);
    TestBarrier(WT_EXECUTEDEFAULT);
    TestBarrier(WT_EXECUTEINIOTHREAD);
    TestLongFunctions();
}
