/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Tests for RegisterWaitForSingleObject, UnregisterWait and UnregisterWaitEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */
#include "precomp.h"
#include <tlhelp32.h>

static volatile LONG s_Started;
static volatile LONG s_Done;
static HANDLE volatile s_WaitHandle;
static HANDLE s_SeenHandle;

static VOID CALLBACK
CountCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    InterlockedIncrement((PLONG)Context);
}

static VOID CALLBACK
SlowCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    InterlockedExchange((PLONG)&s_Started, 1);
    Sleep(200);
    InterlockedExchange((PLONG)&s_Done, 1);
}

static VOID CALLBACK
HandleCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    s_SeenHandle = s_WaitHandle;
    SetEvent((HANDLE)Context);
}

static void
TestCompletionEventWithoutCallback(void)
{
    HANDLE Object, Completion, Wait;
    LONG Count = 0;
    DWORD Result;
    BOOL Ret;

    Object = CreateEventW(NULL, TRUE, FALSE, NULL);
    Completion = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Object != NULL && Completion != NULL, "CreateEventW failed: %lu\n", GetLastError());

    Ret = RegisterWaitForSingleObject(&Wait, Object, CountCallback, &Count,
                                      INFINITE, WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    if (!Ret)
        goto Cleanup;

    /* Let the wait thread block on the object */
    Sleep(50);

    Ret = UnregisterWaitEx(Wait, Completion);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());

    Result = WaitForSingleObject(Completion, 5000);
    ok(Result == WAIT_OBJECT_0, "Completion event not signalled: %lu\n", Result);
    ok(Count == 0, "Callback ran %ld times\n", Count);

Cleanup:
    CloseHandle(Completion);
    CloseHandle(Object);
}

static void
TestBlockingUnregister(void)
{
    HANDLE Object, Wait;
    LONG StartedAtReturn, DoneAtReturn;
    int OldPriority, i;
    BOOL Ret;

    Object = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(Object != NULL, "CreateEventW failed: %lu\n", GetLastError());

    /* Keep the woken wait thread from preempting us on one processor */
    OldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    for (i = 0; i < 3; i++)
    {
        s_Started = 0;
        s_Done = 0;

        Ret = RegisterWaitForSingleObject(&Wait, Object, SlowCallback, NULL,
                                          INFINITE, WT_EXECUTEONLYONCE);
        ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
        if (!Ret)
            break;

        /* Let the wait thread block on the object */
        Sleep(50);

        SetEvent(Object);
        Ret = UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
        StartedAtReturn = s_Started;
        DoneAtReturn = s_Done;
        ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());

        Sleep(400);
        ok(!StartedAtReturn || DoneAtReturn,
           "Iteration %d: callback still running after UnregisterWaitEx returned\n", i);
        ok(s_Started == StartedAtReturn && s_Done == DoneAtReturn,
           "Iteration %d: callback ran after UnregisterWaitEx returned (%ld/%ld -> %ld/%ld)\n",
           i, StartedAtReturn, DoneAtReturn, s_Started, s_Done);
        ResetEvent(Object);
    }

    SetThreadPriority(GetCurrentThread(), OldPriority);
    CloseHandle(Object);
}

static void
TestNonBlockingUnregister(void)
{
    HANDLE Object, Wait;
    LONG StartedAtReturn;
    int OldPriority, i;
    DWORD Error;
    BOOL Ret;

    Object = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(Object != NULL, "CreateEventW failed: %lu\n", GetLastError());

    /* Keep the woken wait thread from preempting us on one processor */
    OldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    for (i = 0; i < 3; i++)
    {
        s_Started = 0;
        s_Done = 0;

        Ret = RegisterWaitForSingleObject(&Wait, Object, SlowCallback, NULL,
                                          INFINITE, WT_EXECUTEONLYONCE);
        ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
        if (!Ret)
            break;

        /* Let the wait thread block on the object */
        Sleep(50);

        SetEvent(Object);
        SetLastError(0xdeadbeef);
        Ret = UnregisterWait(Wait);
        Error = GetLastError();
        StartedAtReturn = s_Started;

        /* Let a callback that did start finish */
        Sleep(400);
        if (Ret)
        {
            ok(s_Started == StartedAtReturn,
               "Iteration %d: callback started after UnregisterWait returned TRUE\n", i);
        }
        else
        {
            ok(Error == ERROR_IO_PENDING, "Iteration %d: UnregisterWait failed: %lu\n", i, Error);
        }
        ResetEvent(Object);
    }

    SetThreadPriority(GetCurrentThread(), OldPriority);
    CloseHandle(Object);
}

static void
TestHandleBeforeCallback(void)
{
    HANDLE Object, Done;
    int OldPriority, i;
    DWORD Result;
    BOOL Ret;

    Object = CreateEventW(NULL, TRUE, TRUE, NULL);
    Done = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(Object != NULL && Done != NULL, "CreateEventW failed: %lu\n", GetLastError());

    /* Let the wait thread run before RegisterWaitForSingleObject returns */
    OldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    for (i = 0; i < 10; i++)
    {
        s_WaitHandle = INVALID_HANDLE_VALUE;
        s_SeenHandle = NULL;

        Ret = RegisterWaitForSingleObject((PHANDLE)&s_WaitHandle, Object, HandleCallback, Done,
                                          INFINITE, WT_EXECUTEONLYONCE);
        ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
        if (!Ret)
            break;

        Result = WaitForSingleObject(Done, 5000);
        ok(Result == WAIT_OBJECT_0, "Callback did not run: %lu\n", Result);
        ok(s_SeenHandle == s_WaitHandle,
           "Iteration %d: callback saw handle %p, expected %p\n", i, s_SeenHandle, s_WaitHandle);

        Ret = UnregisterWaitEx(s_WaitHandle, INVALID_HANDLE_VALUE);
        ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    }

    SetThreadPriority(GetCurrentThread(), OldPriority);
    CloseHandle(Done);
    CloseHandle(Object);
}

static VOID CALLBACK
SetEventCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    SetEvent((HANDLE)Context);
}

/* More registered waits than the worker pool has threads, as the Chrome sandbox broker has (a wait per
   IPC channel of every child): each wait still fires, and unregistering one with an event completes */
#define MANY_WAITS 300

static void
TestManyWaits(void)
{
    HANDLE *Objects, *Waits, Object, Fired, Completion, Wait;
    LONG Count = 0;
    int i, Registered = 0;
    DWORD Result;
    BOOL Ret;

    Objects = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MANY_WAITS * sizeof(HANDLE));
    Waits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MANY_WAITS * sizeof(HANDLE));
    Object = CreateEventW(NULL, FALSE, FALSE, NULL);
    Fired = CreateEventW(NULL, TRUE, FALSE, NULL);
    Completion = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Objects && Waits && Object && Fired && Completion, "Allocation failed: %lu\n", GetLastError());
    if (!Objects || !Waits || !Object || !Fired || !Completion)
        goto Cleanup;

    for (i = 0; i < MANY_WAITS; i++)
    {
        Objects[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!Objects[i])
            break;
        if (!RegisterWaitForSingleObject(&Waits[i], Objects[i], CountCallback, &Count,
                                         INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE))
        {
            CloseHandle(Objects[i]);
            Objects[i] = NULL;
            break;
        }
        Registered++;
    }
    ok(Registered == MANY_WAITS, "Registered %d of %d waits: %lu\n", Registered, MANY_WAITS, GetLastError());

    /* A wait registered after all the others fires */
    Ret = RegisterWaitForSingleObject(&Wait, Object, SetEventCallback, Fired,
                                      INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    if (Ret)
    {
        SetEvent(Object);
        Result = WaitForSingleObject(Fired, 5000);
        ok(Result == WAIT_OBJECT_0, "Wait registered after %d others did not fire: %lu\n", Registered, Result);
        Ret = UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
        ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    }

    /* Unregistering a wait with no callback running completes at once, whether or not its thread ran */
    ResetEvent(Object);
    Ret = RegisterWaitForSingleObject(&Wait, Object, SetEventCallback, Fired,
                                      INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    if (Ret)
    {
        Ret = UnregisterWaitEx(Wait, Completion);
        ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
        Result = WaitForSingleObject(Completion, 5000);
        ok(Result == WAIT_OBJECT_0, "Completion event not signalled: %lu\n", Result);
    }

    /* The first and the last of the many waits fire */
    if (Registered == MANY_WAITS)
    {
        SetEvent(Objects[0]);
        SetEvent(Objects[MANY_WAITS - 1]);
        for (i = 0; i < 50 && Count < 2; i++)
            Sleep(100);
        ok(Count == 2, "%ld of 2 signalled waits fired\n", Count);
    }

    for (i = 0; i < Registered; i++)
    {
        Ret = UnregisterWaitEx(Waits[i], INVALID_HANDLE_VALUE);
        ok(Ret, "UnregisterWaitEx(%d) failed: %lu\n", i, GetLastError());
        CloseHandle(Objects[i]);
    }

Cleanup:
    if (Completion) CloseHandle(Completion);
    if (Fired) CloseHandle(Fired);
    if (Object) CloseHandle(Object);
    if (Waits) HeapFree(GetProcessHeap(), 0, Waits);
    if (Objects) HeapFree(GetProcessHeap(), 0, Objects);
}


/* A wait thread whose wait is gone stays idle for 10 seconds, then exits; a wait registered while it
   exits must still run and complete. Register and unregister across that boundary */
static void
TestIdleLingerBoundary(void)
{
    static const DWORD Pauses[] = { 9700, 9900, 10000, 10100, 10400 };
    HANDLE Object, Fired, Wait;
    DWORD Result;
    BOOL Ret;
    int Round, i;

    Object = CreateEventW(NULL, FALSE, FALSE, NULL);
    Fired = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(Object && Fired, "CreateEventW failed: %lu\n", GetLastError());
    if (!Object || !Fired)
        goto Cleanup;

    for (Round = 0; Round < (int)(sizeof(Pauses) / sizeof(Pauses[0])); Round++)
    {
        /* A burst: the waits reuse idle wait threads and start new ones */
        for (i = 0; i < 20; i++)
        {
            Ret = RegisterWaitForSingleObject(&Wait, Object, SetEventCallback, Fired,
                                              INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
            ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
            if (!Ret)
                continue;
            SetEvent(Object);
            Result = WaitForSingleObject(Fired, 5000);
            ok(Result == WAIT_OBJECT_0, "Round %d, wait %d did not fire: %lu\n", Round, i, Result);
            Ret = UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
            ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
        }

        /* Everything is idle now. Let the threads reach the end of their 10 seconds */
        Sleep(Pauses[Round]);

        Ret = RegisterWaitForSingleObject(&Wait, Object, SetEventCallback, Fired,
                                          INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
        ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
        if (!Ret)
            continue;
        SetEvent(Object);
        Result = WaitForSingleObject(Fired, 5000);
        ok(Result == WAIT_OBJECT_0, "Round %d: wait after a pause of %lu ms did not fire: %lu\n",
           Round, Pauses[Round], Result);
        Ret = UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
        ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    }

Cleanup:
    if (Fired) CloseHandle(Fired);
    if (Object) CloseHandle(Object);
}

static volatile LONG s_Raised;

static VOID CALLBACK
RaiseCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    InterlockedIncrement(&s_Raised);
    /* Nothing catches this in the test: the wait must contain it, as a pool thread contains the
       exception of a work item */
    RaiseException(0xE0000001, 0, 0, NULL);
}

static DWORD WINAPI
UnregisterThread(PVOID Context)
{
    return UnregisterWaitEx((HANDLE)Context, INVALID_HANDLE_VALUE);
}

static void
TestCallbackException(ULONG Flags)
{
    HANDLE Object, Completion, Wait, Thread;
    DWORD Result;
    BOOL Ret;
    int i;

    /* Windows ends the process on an exception in a wait callback. ReactOS contains it, and ends
       only the wait */
    if (!strcmp(winetest_platform, "windows"))
    {
        skip("Flags %lx: an exception in a wait callback ends the process on Windows\n", Flags);
        return;
    }

    Object = CreateEventW(NULL, TRUE, FALSE, NULL);
    Completion = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Object && Completion, "CreateEventW failed: %lu\n", GetLastError());
    if (!Object || !Completion)
        goto Cleanup;

    /* Blocking unregister of a wait whose callback raised: it must return */
    s_Raised = 0;
    Ret = RegisterWaitForSingleObject(&Wait, Object, RaiseCallback, NULL, INFINITE, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    if (Ret)
    {
        SetEvent(Object);
        for (i = 0; i < 50 && !s_Raised; i++)
            Sleep(100);
        ok(s_Raised >= 1, "Callback did not run\n");
        Sleep(100);

        /* On a thread of its own: a hang here ends in the timeout, not in a stuck test */
        Thread = CreateThread(NULL, 0, UnregisterThread, Wait, 0, NULL);
        ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (Thread)
        {
            Result = WaitForSingleObject(Thread, 10000);
            ok(Result == WAIT_OBJECT_0, "UnregisterWaitEx(INVALID_HANDLE_VALUE) did not return: %lu\n", Result);
            if (Result == WAIT_OBJECT_0)
            {
                GetExitCodeThread(Thread, &Result);
                ok(Result == TRUE, "UnregisterWaitEx failed\n");
            }
            CloseHandle(Thread);
        }
    }

    /* Unregister with an event of a wait whose callback raised: the event is signalled */
    ResetEvent(Object);
    s_Raised = 0;
    Ret = RegisterWaitForSingleObject(&Wait, Object, RaiseCallback, NULL, INFINITE, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    if (Ret)
    {
        SetEvent(Object);
        for (i = 0; i < 50 && !s_Raised; i++)
            Sleep(100);
        ok(s_Raised >= 1, "Callback did not run\n");
        Sleep(100);
        Ret = UnregisterWaitEx(Wait, Completion);
        ok(Ret || GetLastError() == ERROR_IO_PENDING, "UnregisterWaitEx failed: %lu\n", GetLastError());
        Result = WaitForSingleObject(Completion, 5000);
        ok(Result == WAIT_OBJECT_0, "Completion event not signalled: %lu\n", Result);
    }

Cleanup:
    if (Completion) CloseHandle(Completion);
    if (Object) CloseHandle(Object);
}

static DWORD
CountProcessThreads(void)
{
    HANDLE Snapshot;
    THREADENTRY32 Entry;
    DWORD Pid = GetCurrentProcessId(), Count = 0;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;
    Entry.dwSize = sizeof(Entry);
    if (Thread32First(Snapshot, &Entry))
    {
        do
        {
            if (Entry.th32OwnerProcessID == Pid)
                Count++;
        } while (Thread32Next(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
    return Count;
}

/* Waits that wait for good, a thousand of them at a time and in cycles, as Chrome's do: they share
   the wait threads, up to 63 to a thread, and do not take a thread each */
#define GROWTH_WAITS 1000
#define GROWTH_BATCH 100
#define GROWTH_SLACK 40   /* 1000 waits need 16 threads; room for the threads of the other tests */

static void
TestThreadGrowth(void)
{
    HANDLE *Objects, *Waits;
    DWORD Base, Now, Peak;
    LONG Count = 0;
    int Cycle, i, Registered;
    BOOL Ret;

    Objects = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, GROWTH_WAITS * sizeof(HANDLE));
    Waits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, GROWTH_WAITS * sizeof(HANDLE));
    ok(Objects && Waits, "Allocation failed\n");
    if (!Objects || !Waits)
        goto Cleanup;
    for (i = 0; i < GROWTH_WAITS; i++)
    {
        Objects[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!Objects[i])
            break;
    }
    ok(i == GROWTH_WAITS, "Created %d of %d events\n", i, GROWTH_WAITS);
    if (i != GROWTH_WAITS)
        goto Cleanup;

    Base = CountProcessThreads();
    ok(Base != 0, "Could not count the threads\n");
    Peak = Base;

    /* Cycles of a hundred waits, registered and unregistered, ten times over: a thousand in all.
       The thread of a wait that goes stays for the next one, so the count does not grow with the cycles */
    for (Cycle = 0; Cycle < GROWTH_WAITS / GROWTH_BATCH; Cycle++)
    {
        Registered = 0;
        for (i = 0; i < GROWTH_BATCH; i++)
        {
            Ret = RegisterWaitForSingleObject(&Waits[i], Objects[Cycle * GROWTH_BATCH + i], CountCallback,
                                              &Count, INFINITE,
                                              (i & 1) ? (WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE) : WT_EXECUTEDEFAULT);
            if (Ret)
                Registered++;
        }
        ok(Registered == GROWTH_BATCH, "Cycle %d: registered %d of %d\n", Cycle, Registered, GROWTH_BATCH);
        Now = CountProcessThreads();
        if (Now > Peak)
            Peak = Now;
        for (i = 0; i < Registered; i++)
        {
            Ret = UnregisterWaitEx(Waits[i], INVALID_HANDLE_VALUE);
            ok(Ret, "Cycle %d: UnregisterWaitEx(%d) failed: %lu\n", Cycle, i, GetLastError());
        }
    }
    ok(Peak <= Base + GROWTH_SLACK, "Threads grew from %lu to %lu over the cycles\n", Base, Peak);
    ok(Count == 0, "A callback ran %ld times\n", Count);

    /* All thousand at the same time */
    Registered = 0;
    for (i = 0; i < GROWTH_WAITS; i++)
    {
        Ret = RegisterWaitForSingleObject(&Waits[i], Objects[i], CountCallback, &Count, INFINITE,
                                          WT_EXECUTEONLYONCE);
        if (Ret)
            Registered++;
    }
    ok(Registered == GROWTH_WAITS, "Registered %d of %d at once\n", Registered, GROWTH_WAITS);
    Now = CountProcessThreads();
    ok(Now <= Base + GROWTH_SLACK, "%d waits at once: threads grew from %lu to %lu\n", Registered, Base, Now);
    trace("threads: %lu before, %lu for %d waits at once\n", Base, Now, Registered);

    /* The first, a middle one and the last one fire, and only those */
    SetEvent(Objects[0]);
    SetEvent(Objects[GROWTH_WAITS / 2]);
    SetEvent(Objects[GROWTH_WAITS - 1]);
    for (i = 0; i < 50 && Count < 3; i++)
        Sleep(100);
    ok(Count == 3, "%ld of 3 signalled waits fired\n", Count);
    Sleep(200);
    ok(Count == 3, "%ld callbacks ran, 3 expected\n", Count);

    for (i = 0; i < Registered; i++)
    {
        Ret = UnregisterWaitEx(Waits[i], INVALID_HANDLE_VALUE);
        ok(Ret, "UnregisterWaitEx(%d) failed: %lu\n", i, GetLastError());
    }

    /* Once the waits are gone the threads leave after their 10 seconds */
    Sleep(11500);
    Now = CountProcessThreads();
    ok(Now <= Base + 4, "%lu threads long after the waits, %lu before\n", Now, Base);

Cleanup:
    if (Objects)
    {
        for (i = 0; i < GROWTH_WAITS; i++)
            if (Objects[i]) CloseHandle(Objects[i]);
        HeapFree(GetProcessHeap(), 0, Objects);
    }
    if (Waits) HeapFree(GetProcessHeap(), 0, Waits);
}

static volatile LONG s_Timeouts;
static volatile LONG s_Signals;

static VOID CALLBACK
FiredCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    InterlockedIncrement(TimerOrWaitFired ? &s_Timeouts : &s_Signals);
    if (Context)
        SetEvent((HANDLE)Context);
}

/* Waits that share a thread keep their own timeouts, fire for their own objects, and do not hurt
   each other: a handle that went away, the same handle twice */
static void
TestSharedThread(void)
{
    HANDLE Object, Object2, Bad, Fast, Slow, Never, Twice1, Twice2, Fired1, Fired2, Wait;
    LONG TimeoutsAtStop;
    DWORD Result;
    BOOL Ret;

    Object = CreateEventW(NULL, FALSE, FALSE, NULL);
    Object2 = CreateEventW(NULL, TRUE, FALSE, NULL);
    Bad = CreateEventW(NULL, FALSE, FALSE, NULL);
    Fired1 = CreateEventW(NULL, FALSE, FALSE, NULL);
    Fired2 = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(Object && Object2 && Bad && Fired1 && Fired2, "CreateEventW failed: %lu\n", GetLastError());

    s_Timeouts = 0;
    s_Signals = 0;

    /* A repeating wait with a timeout: fires with TimerOrWaitFired set, again and again */
    Ret = RegisterWaitForSingleObject(&Fast, Object, FiredCallback, NULL, 50, WT_EXECUTEINWAITTHREAD);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    /* A wait that never fires */
    Ret = RegisterWaitForSingleObject(&Never, Object2, FiredCallback, NULL, INFINITE, WT_EXECUTEINWAITTHREAD);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    /* A wait with a longer timeout, once */
    Ret = RegisterWaitForSingleObject(&Slow, Object2, FiredCallback, Fired2, 400,
                                      WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    /* A wait on a handle that is closed while the wait is there */
    Ret = RegisterWaitForSingleObject(&Wait, Bad, FiredCallback, NULL, INFINITE, WT_EXECUTEINWAITTHREAD);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    CloseHandle(Bad);

    Result = WaitForSingleObject(Fired2, 5000);
    ok(Result == WAIT_OBJECT_0, "The wait with a timeout of 400 ms did not fire: %lu\n", Result);
    ok(s_Timeouts >= 2, "The wait with a timeout of 50 ms fired %ld times by then\n", s_Timeouts);
    Sleep(300);
    TimeoutsAtStop = s_Timeouts;
    ok(TimeoutsAtStop >= 6, "%ld timeouts in all, the wait with 50 ms keeps firing\n", TimeoutsAtStop);

    /* A signal gets to the right wait, with TimerOrWaitFired clear, among the others and the dead one */
    s_Signals = 0;
    SetEvent(Object);
    Sleep(200);
    ok(s_Signals == 1, "%ld signals, 1 expected\n", s_Signals);

    /* The same handle in two waits: both fire */
    Twice1 = NULL;
    Twice2 = NULL;
    ResetEvent(Object2);
    Ret = RegisterWaitForSingleObject(&Twice1, Object2, FiredCallback, Fired1, INFINITE,
                                      WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Ret = RegisterWaitForSingleObject(&Twice2, Object2, FiredCallback, Fired2, INFINITE,
                                      WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    SetEvent(Object2);
    Result = WaitForSingleObject(Fired1, 5000);
    ok(Result == WAIT_OBJECT_0, "The first of two waits on one handle did not fire: %lu\n", Result);
    Result = WaitForSingleObject(Fired2, 5000);
    ok(Result == WAIT_OBJECT_0, "The second of two waits on one handle did not fire: %lu\n", Result);

    Ret = UnregisterWaitEx(Twice1, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(Twice2, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(Fast, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(Never, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(Slow, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());

    /* Gone is gone: nothing fires any more */
    TimeoutsAtStop = s_Timeouts;
    Sleep(300);
    ok(s_Timeouts == TimeoutsAtStop, "A wait fired after it was unregistered\n");

    CloseHandle(Fired2);
    CloseHandle(Fired1);
    CloseHandle(Object2);
    CloseHandle(Object);
}

/* --- Callbacks that touch other waits of their thread. Registered one after the other, the waits
   share a wait thread. WT_EXECUTEINWAITTHREAD callbacks run on it, WT_EXECUTEDEFAULT ones in the
   pool --- */

typedef struct _CB_CTX
{
    HANDLE Other;       /* the wait to unregister */
    HANDLE Done;
    BOOL Blocking;
    BOOL Ret;
    DWORD Error;
} CB_CTX;

static VOID CALLBACK
UnregisterOtherCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    CB_CTX *Ctx = Context;

    SetLastError(0);
    if (Ctx->Blocking)
        Ctx->Ret = UnregisterWaitEx(Ctx->Other, INVALID_HANDLE_VALUE);
    else
        Ctx->Ret = UnregisterWait(Ctx->Other);
    Ctx->Error = GetLastError();
    SetEvent(Ctx->Done);
}

static void
TestCallbackUnregistersOther(ULONG Flags, BOOL Blocking)
{
    HANDLE ObjA, ObjB, Done, WaitA, WaitB;
    CB_CTX Ctx;
    LONG Count = 0;
    DWORD Result;
    BOOL Ret;

    ObjA = CreateEventW(NULL, TRUE, FALSE, NULL);
    ObjB = CreateEventW(NULL, TRUE, FALSE, NULL);
    Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(ObjA && ObjB && Done, "CreateEventW failed: %lu\n", GetLastError());

    Ctx.Done = Done;
    Ctx.Blocking = Blocking;
    Ctx.Ret = FALSE;
    Ctx.Error = 0;
    Ret = RegisterWaitForSingleObject(&WaitB, ObjB, CountCallback, &Count, INFINITE, Flags | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Ctx.Other = WaitB;
    Ret = RegisterWaitForSingleObject(&WaitA, ObjA, UnregisterOtherCallback, &Ctx, INFINITE, Flags | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());

    SetEvent(ObjA);
    Result = WaitForSingleObject(Done, 5000);
    ok(Result == WAIT_OBJECT_0, "Flags %lx, blocking %d: the callback did not return\n", Flags, Blocking);
    ok(Ctx.Ret || (!Blocking && Ctx.Error == ERROR_IO_PENDING),
       "Flags %lx, blocking %d: unregister from the callback failed: %lu\n", Flags, Blocking, Ctx.Error);

    /* The other wait is gone: its object does not fire it */
    SetEvent(ObjB);
    Sleep(200);
    ok(Count == 0, "Flags %lx: the unregistered wait fired\n", Flags);

    Ret = UnregisterWaitEx(WaitA, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    CloseHandle(Done);
    CloseHandle(ObjB);
    CloseHandle(ObjA);
}

/* A callback that registers the next wait, on its own handle and on another one, as
   base::win::ObjectWatcher does: a chain of waits that does not start a thread for each */
#define REARM_COUNT 1000

typedef struct _REARM_CTX
{
    HANDLE Object;
    HANDLE Other[REARM_COUNT / 10 + 1];  /* a handle of its own for each wait: one handle once per thread */
    HANDLE Done;
    ULONG Flags;
    volatile LONG Index;
    DWORD PeakThreads;
    HANDLE Waits[REARM_COUNT + 1];
    HANDLE OtherWaits[REARM_COUNT / 10 + 1];
    volatile LONG OtherFired;
} REARM_CTX;

static VOID CALLBACK
OtherFiredCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    InterlockedIncrement(&((REARM_CTX *)Context)->OtherFired);
}

static VOID CALLBACK
RearmCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    REARM_CTX *Ctx = Context;
    LONG i = InterlockedIncrement(&Ctx->Index);
    HANDLE Dummy;

    if (i % 100 == 0)
    {
        DWORD n = CountProcessThreads();
        if (n > Ctx->PeakThreads)
            Ctx->PeakThreads = n;
    }
    if (i >= REARM_COUNT)
    {
        SetEvent(Ctx->Done);
        return;
    }
    /* Same handle, from the callback of the wait that waited for it */
    if (!RegisterWaitForSingleObject(&Ctx->Waits[i], Ctx->Object, RearmCallback, Ctx, INFINITE,
                                     Ctx->Flags | WT_EXECUTEONLYONCE))
        SetEvent(Ctx->Done);
    /* Another handle */
    if (i % 10 == 0 && RegisterWaitForSingleObject(&Dummy, Ctx->Other[i / 10], OtherFiredCallback, Ctx, INFINITE,
                                                   Ctx->Flags | WT_EXECUTEONLYONCE))
        Ctx->OtherWaits[i / 10] = Dummy;
}

static void
TestRearmFromCallback(ULONG Flags)
{
    static REARM_CTX Ctx;
    DWORD Base, Result;
    HANDLE Wait;
    int i;
    BOOL Ret;

    memset(&Ctx, 0, sizeof(Ctx));
    Ctx.Object = CreateEventW(NULL, TRUE, TRUE, NULL);  /* signalled: every wait fires at once */
    for (i = 0; i < REARM_COUNT / 10 + 1; i++)
        Ctx.Other[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ctx.Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ctx.Flags = Flags;
    ok(Ctx.Object && Ctx.Other[0] && Ctx.Done, "CreateEventW failed: %lu\n", GetLastError());

    Base = CountProcessThreads();
    Ctx.PeakThreads = Base;
    Ret = RegisterWaitForSingleObject(&Wait, Ctx.Object, RearmCallback, &Ctx, INFINITE, Flags | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Result = WaitForSingleObject(Ctx.Done, 30000);
    ok(Result == WAIT_OBJECT_0, "Flags %lx: the chain of waits stopped at %ld of %d\n", Flags, Ctx.Index, REARM_COUNT);
    trace("flags %lx: %d re-arms from the callback: threads %lu before, peak %lu\n", Flags, REARM_COUNT, Base, Ctx.PeakThreads);
    ok(Ctx.PeakThreads <= Base + 6, "Flags %lx: threads grew from %lu to %lu over the chain\n", Flags, Base, Ctx.PeakThreads);

    /* Every wait of the chain is unregistered at last, as its owner does */
    UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
    for (i = 1; i < REARM_COUNT; i++)
        if (Ctx.Waits[i])
            UnregisterWaitEx(Ctx.Waits[i], INVALID_HANDLE_VALUE);
    for (i = 0; i < REARM_COUNT / 10 + 1; i++)
        if (Ctx.OtherWaits[i])
            UnregisterWaitEx(Ctx.OtherWaits[i], INVALID_HANDLE_VALUE);
    /* The waits for the other handles: unregistered one by one, and the last of them may be any of
       the REARM_COUNT / 10 */
    Sleep(100);
    for (i = 0; i < REARM_COUNT / 10 + 1; i++)
        CloseHandle(Ctx.Other[i]);
    CloseHandle(Ctx.Done);
    CloseHandle(Ctx.Object);
}

/* A callback that unregisters its own wait without blocking, a wait that repeats */
static volatile LONG s_SelfFired;
static HANDLE volatile s_SelfWait;
static BOOL s_SelfRet;
static DWORD s_SelfError;

static VOID CALLBACK
SelfUnregisterCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    if (InterlockedIncrement(&s_SelfFired) == 1)
    {
        SetLastError(0);
        s_SelfRet = UnregisterWait(s_SelfWait);
        s_SelfError = GetLastError();
        SetEvent((HANDLE)Context);
    }
}

static void
TestSelfUnregister(ULONG Flags)
{
    HANDLE Object, Done;
    LONG Fired;
    DWORD Result;
    BOOL Ret;

    Object = CreateEventW(NULL, TRUE, TRUE, NULL);  /* signalled for good: it fires again if it is still there */
    Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_SelfFired = 0;
    s_SelfWait = NULL;
    Ret = RegisterWaitForSingleObject((PHANDLE)&s_SelfWait, Object, SelfUnregisterCallback, Done, INFINITE, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Result = WaitForSingleObject(Done, 5000);
    ok(Result == WAIT_OBJECT_0, "Flags %lx: the callback did not run\n", Flags);
    ok(s_SelfRet || s_SelfError == ERROR_IO_PENDING, "Flags %lx: UnregisterWait of itself failed: %lu\n", Flags, s_SelfError);
    Sleep(200);
    Fired = s_SelfFired;
    Sleep(200);
    /* The wait thread runs the callback of WT_EXECUTEINWAITTHREAD and waits for nothing meanwhile. In
       the pool the wait goes on while the callback runs, and fires again for the object that stays
       signalled, until the callback unregistered it (Windows 10: twice) */
    if (Flags & WT_EXECUTEINWAITTHREAD)
        ok(Fired == 1, "Flags %lx: the wait fired %ld times, 1 expected\n", Flags, Fired);
    else
        ok(Fired >= 1, "Flags %lx: the wait fired %ld times\n", Flags, Fired);
    ok(s_SelfFired == Fired, "Flags %lx: the wait fired after it was unregistered (%ld, then %ld)\n", Flags, Fired, s_SelfFired);
    /* The wait is gone and freed: nothing to unregister again */
    CloseHandle(Done);
    CloseHandle(Object);
}

/* A fire and an unregister at the same moment, many times: when UnregisterWaitEx returns the
   callback is not running and does not run again */
static volatile LONG s_RaceInCallback;
static volatile LONG s_RaceFired;

static VOID CALLBACK
RaceCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    int i;

    InterlockedExchange(&s_RaceInCallback, 1);
    InterlockedIncrement(&s_RaceFired);
    for (i = 0; i < 200; i++)
        YieldProcessor();
    InterlockedExchange(&s_RaceInCallback, 0);
}

static void
TestUnregisterRace(ULONG Flags)
{
    HANDLE Object, Wait;
    LONG FiredAtReturn;
    int i, Bad = 0, Failed = 0, Ran = 0;
    BOOL Ret;

    Object = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < 1500; i++)
    {
        s_RaceFired = 0;
        if (i % 2 == 0)
            SetEvent(Object);   /* the object is signalled before the wait: it fires as soon as the thread looks */
        Ret = RegisterWaitForSingleObject(&Wait, Object, RaceCallback, NULL, INFINITE, Flags);
        if (!Ret)
        {
            Failed++;
            continue;
        }
        if (i % 2 != 0)
            SetEvent(Object);
        if (i % 3 == 1)
            YieldProcessor();
        else if (i % 3 == 2)
            Sleep(0);
        Ret = UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
        FiredAtReturn = s_RaceFired;
        if (!Ret)
            Failed++;
        if (s_RaceInCallback)
            Bad++;
        Sleep(0);
        if (s_RaceFired != FiredAtReturn)
            Bad++;
        Ran += FiredAtReturn;
        ResetEvent(Object);
    }
    ok(Failed == 0, "Flags %lx: %d registers or unregisters failed\n", Flags, Failed);
    ok(Bad == 0, "Flags %lx: %d times a callback ran after UnregisterWaitEx returned\n", Flags, Bad);
    trace("flags %lx: the callback won the race %d times in 1500\n", Flags, Ran);
    CloseHandle(Object);
}

/* A wait whose object stays signalled returns at once, every round. The timed wait of its thread
   still fires on time */
static volatile LONG s_Persistent;
static volatile LONG s_Timed;

static VOID CALLBACK
PersistentCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    InterlockedIncrement(&s_Persistent);
    Sleep(1);
}

static VOID CALLBACK
TimedCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    if (TimerOrWaitFired)
        InterlockedIncrement(&s_Timed);
}

static void
TestPersistentSignalAndTimeout(ULONG Flags)
{
    HANDLE Signalled, Quiet, WaitP, WaitT;
    BOOL Ret;

    Signalled = CreateEventW(NULL, TRUE, TRUE, NULL);
    Quiet = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_Persistent = 0;
    s_Timed = 0;

    /* The persistent one first: it is the first handle of the thread */
    Ret = RegisterWaitForSingleObject(&WaitP, Signalled, PersistentCallback, NULL, INFINITE, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Ret = RegisterWaitForSingleObject(&WaitT, Quiet, TimedCallback, NULL, 100, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());

    Sleep(1050);
    ok(s_Timed >= 8 && s_Timed <= 11, "Flags %lx: the wait with 100 ms fired %ld times in a second, about 10 expected\n", Flags, s_Timed);
    ok(s_Persistent > 20, "Flags %lx: the signalled wait fired %ld times\n", Flags, s_Persistent);

    Ret = UnregisterWaitEx(WaitP, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(WaitT, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    CloseHandle(Quiet);
    CloseHandle(Signalled);
}

static void
TestSharedThreadCallbacks(void)
{
    ULONG Flags[2] = { WT_EXECUTEDEFAULT, WT_EXECUTEINWAITTHREAD };
    int i;

    for (i = 0; i < 2; i++)
    {
        TestCallbackUnregistersOther(Flags[i], TRUE);
        TestCallbackUnregistersOther(Flags[i], FALSE);
        TestRearmFromCallback(Flags[i]);
        TestSelfUnregister(Flags[i]);
        TestUnregisterRace(Flags[i]);
        TestPersistentSignalAndTimeout(Flags[i]);
    }
}

/* --- Pool dispatch: Windows queues the callback of a wait without WT_EXECUTEINWAITTHREAD to the pool,
   and the wait thread goes on waiting --- */

typedef struct _BLOCK_CTX
{
    HANDLE ObjB;        /* the object of the other wait */
    HANDLE Notice;      /* the other wait sets it */
    HANDLE Done;
    DWORD Result;
} BLOCK_CTX;

static VOID CALLBACK
BlockOnOtherCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    BLOCK_CTX *Ctx = Context;

    SetEvent(Ctx->ObjB);
    Ctx->Result = WaitForSingleObject(Ctx->Notice, 3000);
    SetEvent(Ctx->Done);
}

/* A callback that waits for something another wait of its thread must notice: the wait thread must
   not be the thread that runs the callback */
static void
TestCallbackBlocksOnOtherWait(ULONG FlagsB)
{
    HANDLE ObjA, WaitA, WaitB;
    BLOCK_CTX Ctx;
    DWORD Result;
    BOOL Ret;

    ObjA = CreateEventW(NULL, FALSE, FALSE, NULL);
    Ctx.ObjB = CreateEventW(NULL, FALSE, FALSE, NULL);
    Ctx.Notice = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ctx.Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ctx.Result = 0xdeadbeef;
    ok(ObjA && Ctx.ObjB && Ctx.Notice && Ctx.Done, "CreateEventW failed: %lu\n", GetLastError());

    Ret = RegisterWaitForSingleObject(&WaitA, ObjA, BlockOnOtherCallback, &Ctx, INFINITE, WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Ret = RegisterWaitForSingleObject(&WaitB, Ctx.ObjB, SetEventCallback, Ctx.Notice, INFINITE,
                                      FlagsB | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());

    SetEvent(ObjA);
    Result = WaitForSingleObject(Ctx.Done, 10000);
    ok(Result == WAIT_OBJECT_0, "Flags %lx: the callback did not return: %lu\n", FlagsB, Result);
    ok(Ctx.Result == WAIT_OBJECT_0, "Flags %lx: the other wait did not fire while the callback waited for it: %lu\n",
       FlagsB, Ctx.Result);

    Ret = UnregisterWaitEx(WaitA, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(WaitB, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    CloseHandle(Ctx.Done);
    CloseHandle(Ctx.Notice);
    CloseHandle(Ctx.ObjB);
    CloseHandle(ObjA);
}

static VOID CALLBACK
LongCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    Sleep(1000);
    SetEvent((HANDLE)Context);
}

/* A long callback does not hold up the timeouts of the other waits of its thread */
static void
TestLongCallbackTimeouts(ULONG FlagsT)
{
    HANDLE Signalled, Quiet, LongDone, WaitL, WaitT;
    LONG Timed;
    BOOL Ret;

    Signalled = CreateEventW(NULL, TRUE, TRUE, NULL);
    Quiet = CreateEventW(NULL, TRUE, FALSE, NULL);
    LongDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_Timed = 0;

    Ret = RegisterWaitForSingleObject(&WaitL, Signalled, LongCallback, LongDone, INFINITE, WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Ret = RegisterWaitForSingleObject(&WaitT, Quiet, TimedCallback, NULL, 100, FlagsT);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());

    Sleep(1050);
    Timed = s_Timed;
    ok(Timed >= 8 && Timed <= 11, "Flags %lx: the wait with 100 ms fired %ld times while a callback took 1 s, about 10 expected\n",
       FlagsT, Timed);

    ok(WaitForSingleObject(LongDone, 5000) == WAIT_OBJECT_0, "Flags %lx: the long callback did not return\n", FlagsT);
    Ret = UnregisterWaitEx(WaitL, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    Ret = UnregisterWaitEx(WaitT, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    CloseHandle(LongDone);
    CloseHandle(Quiet);
    CloseHandle(Signalled);
}

typedef struct _TID_CTX
{
    HANDLE Done;
    DWORD Tid;
} TID_CTX;

static VOID CALLBACK
TidCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    TID_CTX *Ctx = Context;

    Ctx->Tid = GetCurrentThreadId();
    SetEvent(Ctx->Done);
}

/* WT_EXECUTEINWAITTHREAD callbacks of waits of one thread run on that thread, and so do those of
   WT_EXECUTEINPERSISTENTTHREAD (Windows 10); any other callback runs on another thread */
static void
TestCallbackThreads(void)
{
    static const ULONG Flags[] = { WT_EXECUTEINWAITTHREAD, WT_EXECUTEDEFAULT, WT_EXECUTEINWAITTHREAD,
                                   WT_EXECUTEINIOTHREAD, WT_EXECUTELONGFUNCTION, WT_EXECUTEINPERSISTENTTHREAD };
    HANDLE Objects[6], Waits[6], Done[6];
    TID_CTX Ctx[6];
    DWORD Result;
    BOOL Ret;
    int i;

    for (i = 0; i < 6; i++)
    {
        Objects[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        Done[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        Ctx[i].Done = Done[i];
        Ctx[i].Tid = 0;
        Ret = RegisterWaitForSingleObject(&Waits[i], Objects[i], TidCallback, &Ctx[i], INFINITE,
                                          Flags[i] | WT_EXECUTEONLYONCE);
        ok(Ret, "RegisterWaitForSingleObject(%lx) failed: %lu\n", Flags[i], GetLastError());
    }
    for (i = 0; i < 6; i++)
        SetEvent(Objects[i]);
    Result = WaitForMultipleObjects(6, Done, TRUE, 5000);
    ok(Result == WAIT_OBJECT_0, "Not every callback ran: %lu\n", Result);

    ok(Ctx[0].Tid != 0 && Ctx[0].Tid == Ctx[2].Tid,
       "WT_EXECUTEINWAITTHREAD callbacks of one wait thread ran on threads %lu and %lu\n", Ctx[0].Tid, Ctx[2].Tid);
    ok(Ctx[0].Tid != GetCurrentThreadId(), "A WT_EXECUTEINWAITTHREAD callback ran on the thread that registered it\n");
    ok(Ctx[5].Tid == Ctx[0].Tid, "The WT_EXECUTEINPERSISTENTTHREAD callback ran on thread %lu, not on the wait thread %lu\n",
       Ctx[5].Tid, Ctx[0].Tid);
    for (i = 1; i < 5; i++)
    {
        if (i == 2)
            continue;
        ok(Ctx[i].Tid != 0 && Ctx[i].Tid != Ctx[0].Tid,
           "The callback with flags %lx ran on the wait thread %lu\n", Flags[i], Ctx[i].Tid);
        ok(Ctx[i].Tid != GetCurrentThreadId(), "The callback with flags %lx ran on the thread that registered it\n", Flags[i]);
    }

    for (i = 0; i < 6; i++)
    {
        Ret = UnregisterWaitEx(Waits[i], INVALID_HANDLE_VALUE);
        ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
        CloseHandle(Done[i]);
        CloseHandle(Objects[i]);
    }
}

static HANDLE s_Gate;
static HANDLE s_GateStarted;
static volatile LONG s_GateRan;

static VOID CALLBACK
GateCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    SetEvent(s_GateStarted);
    WaitForSingleObject(s_Gate, 10000);
    InterlockedIncrement(&s_GateRan);
}

static DWORD WINAPI
UnregisterBlockingThread(PVOID Context)
{
    return UnregisterWaitEx((HANDLE)Context, INVALID_HANDLE_VALUE);
}

/* Unregister while the callback runs: blocking waits for it, otherwise ERROR_IO_PENDING and the event
   is set once it returned; the wait does not fire again */
static void
TestUnregisterRunning(ULONG Flags)
{
    HANDLE Object, Completion, Wait, Thread;
    DWORD Result, Error, Code;
    BOOL Ret;

    Object = CreateEventW(NULL, FALSE, FALSE, NULL);
    Completion = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_Gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_GateStarted = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* With an event */
    s_GateRan = 0;
    Ret = RegisterWaitForSingleObject(&Wait, Object, GateCallback, NULL, INFINITE, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    SetEvent(Object);
    ok(WaitForSingleObject(s_GateStarted, 5000) == WAIT_OBJECT_0, "Flags %lx: the callback did not start\n", Flags);
    SetLastError(0xdeadbeef);
    Ret = UnregisterWaitEx(Wait, Completion);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_IO_PENDING, "Flags %lx: UnregisterWaitEx(event) returned %d, error %lu\n", Flags, Ret, Error);
    Result = WaitForSingleObject(Completion, 300);
    ok(Result == WAIT_TIMEOUT, "Flags %lx: the event was set while the callback ran: %lu\n", Flags, Result);
    SetEvent(Object);   /* would fire the wait again if it were still there */
    SetEvent(s_Gate);
    Result = WaitForSingleObject(Completion, 5000);
    ok(Result == WAIT_OBJECT_0, "Flags %lx: the event was not set after the callback: %lu\n", Flags, Result);
    Sleep(200);
    ok(s_GateRan == 1, "Flags %lx: %ld callbacks ran, 1 expected\n", Flags, s_GateRan);

    /* Without an event */
    ResetEvent(Object);
    ResetEvent(s_Gate);
    ResetEvent(s_GateStarted);
    s_GateRan = 0;
    Ret = RegisterWaitForSingleObject(&Wait, Object, GateCallback, NULL, INFINITE, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    SetEvent(Object);
    ok(WaitForSingleObject(s_GateStarted, 5000) == WAIT_OBJECT_0, "Flags %lx: the callback did not start\n", Flags);
    SetLastError(0xdeadbeef);
    Ret = UnregisterWait(Wait);
    Error = GetLastError();
    ok(!Ret && Error == ERROR_IO_PENDING, "Flags %lx: UnregisterWait returned %d, error %lu\n", Flags, Ret, Error);
    SetEvent(Object);
    SetEvent(s_Gate);
    Sleep(300);
    ok(s_GateRan == 1, "Flags %lx: %ld callbacks ran, 1 expected\n", Flags, s_GateRan);

    /* Blocking */
    ResetEvent(Object);
    ResetEvent(s_Gate);
    ResetEvent(s_GateStarted);
    s_GateRan = 0;
    Ret = RegisterWaitForSingleObject(&Wait, Object, GateCallback, NULL, INFINITE, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    SetEvent(Object);
    ok(WaitForSingleObject(s_GateStarted, 5000) == WAIT_OBJECT_0, "Flags %lx: the callback did not start\n", Flags);
    Thread = CreateThread(NULL, 0, UnregisterBlockingThread, Wait, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (Thread)
    {
        Result = WaitForSingleObject(Thread, 300);
        ok(Result == WAIT_TIMEOUT, "Flags %lx: UnregisterWaitEx(INVALID_HANDLE_VALUE) returned while the callback ran: %lu\n",
           Flags, Result);
        SetEvent(s_Gate);
        Result = WaitForSingleObject(Thread, 5000);
        ok(Result == WAIT_OBJECT_0, "Flags %lx: UnregisterWaitEx(INVALID_HANDLE_VALUE) did not return: %lu\n", Flags, Result);
        Code = 0;
        GetExitCodeThread(Thread, &Code);
        ok(Code == TRUE, "Flags %lx: UnregisterWaitEx(INVALID_HANDLE_VALUE) failed\n", Flags);
        ok(s_GateRan == 1, "Flags %lx: %ld callbacks ran, 1 expected\n", Flags, s_GateRan);
        CloseHandle(Thread);
    }
    else
    {
        SetEvent(s_Gate);
    }

    CloseHandle(s_GateStarted);
    CloseHandle(s_Gate);
    CloseHandle(Completion);
    CloseHandle(Object);
}

static volatile LONG s_QueuedRan;

static VOID CALLBACK
QueuedCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    InterlockedIncrement(&s_QueuedRan);
}

/* The wait thread runs a long callback, and another wait of the thread fires meanwhile: unregistering
   that one without blocking completes at once, and its callback does not run. Unregistering it with
   blocking may wait for the wait thread (Windows 10 does), and nothing runs after it returned */
static void
TestUnregisterWhileThreadBusy(ULONG FlagsA, BOOL Blocking)
{
    HANDLE ObjA, ObjB, Completion, WaitA, WaitB, Thread = NULL;
    LONG RanAtReturn = 0;
    DWORD Result, Error, Code;
    BOOL Ret;

    ObjA = CreateEventW(NULL, FALSE, FALSE, NULL);
    ObjB = CreateEventW(NULL, FALSE, FALSE, NULL);
    Completion = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_Gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_GateStarted = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_GateRan = 0;
    s_QueuedRan = 0;

    Ret = RegisterWaitForSingleObject(&WaitA, ObjA, GateCallback, NULL, INFINITE, FlagsA | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Ret = RegisterWaitForSingleObject(&WaitB, ObjB, QueuedCallback, NULL, INFINITE, FlagsA | WT_EXECUTEONLYONCE);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());

    SetEvent(ObjA);
    ok(WaitForSingleObject(s_GateStarted, 5000) == WAIT_OBJECT_0, "Flags %lx: the callback did not start\n", FlagsA);
    SetEvent(ObjB);
    Sleep(200);
    ok(s_QueuedRan == 0, "Flags %lx: the other wait fired while the wait thread ran a callback\n", FlagsA);

    if (Blocking)
    {
        Thread = CreateThread(NULL, 0, UnregisterBlockingThread, WaitB, 0, NULL);
        ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (Thread && WaitForSingleObject(Thread, 300) == WAIT_OBJECT_0)
            RanAtReturn = s_QueuedRan;
        else
            RanAtReturn = -1;   /* still blocked: let the wait thread go */
    }
    else
    {
        SetLastError(0xdeadbeef);
        Ret = UnregisterWaitEx(WaitB, Completion);
        Error = GetLastError();
        ok(Ret, "Flags %lx: UnregisterWaitEx(event) of a wait that did not fire yet returned %d, error %lu\n", FlagsA, Ret, Error);
        Result = WaitForSingleObject(Completion, 0);
        ok(Result == WAIT_OBJECT_0, "Flags %lx: the event was not set at once: %lu\n", FlagsA, Result);
    }

    SetEvent(s_Gate);
    if (Thread)
    {
        Result = WaitForSingleObject(Thread, 5000);
        ok(Result == WAIT_OBJECT_0, "Flags %lx: UnregisterWaitEx(INVALID_HANDLE_VALUE) did not return: %lu\n", FlagsA, Result);
        Code = 0;
        GetExitCodeThread(Thread, &Code);
        ok(Code == TRUE, "Flags %lx: UnregisterWaitEx(INVALID_HANDLE_VALUE) failed\n", FlagsA);
        if (RanAtReturn < 0)
            RanAtReturn = s_QueuedRan;
        CloseHandle(Thread);
    }
    Sleep(300);
    ok(s_GateRan == 1, "Flags %lx: the first callback ran %ld times\n", FlagsA, s_GateRan);
    if (Blocking)
        ok(s_QueuedRan == RanAtReturn, "Flags %lx: the callback ran after UnregisterWaitEx returned\n", FlagsA);
    else
        ok(s_QueuedRan == 0, "Flags %lx: the callback of the unregistered wait ran %ld times\n", FlagsA, s_QueuedRan);

    Ret = UnregisterWaitEx(WaitA, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    CloseHandle(s_GateStarted);
    CloseHandle(s_Gate);
    CloseHandle(Completion);
    CloseHandle(ObjB);
    CloseHandle(ObjA);
}

static volatile LONG s_InCallback;
static volatile LONG s_MaxInCallback;
static volatile LONG s_Calls;
static DWORD s_CallbackSleep;

static VOID CALLBACK
OverlapCallback(PVOID Context, BOOLEAN TimerOrWaitFired)
{
    LONG n = InterlockedIncrement(&s_InCallback), Max;

    while ((Max = s_MaxInCallback) < n && InterlockedCompareExchange(&s_MaxInCallback, n, Max) != Max)
        ;
    InterlockedIncrement(&s_Calls);
    Sleep(s_CallbackSleep);
    InterlockedDecrement(&s_InCallback);
}

/* A wait goes on while its callback runs in the pool: an object that stays signalled, or a timeout
   that comes again, fires it again, and the callbacks overlap (Windows 10). A WT_EXECUTEINWAITTHREAD
   callback is alone. Once unregistered, nothing runs any more */
static void
TestOverlap(ULONG Flags, DWORD Milliseconds)
{
    HANDLE Object, Wait;
    LONG Calls;
    BOOL Ret;

    Object = CreateEventW(NULL, TRUE, Milliseconds == INFINITE, NULL);
    s_InCallback = 0;
    s_MaxInCallback = 0;
    s_Calls = 0;
    s_CallbackSleep = (Milliseconds == INFINITE) ? 100 : 200;

    Ret = RegisterWaitForSingleObject(&Wait, Object, OverlapCallback, NULL, Milliseconds, Flags);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Sleep(1000);
    Ret = UnregisterWaitEx(Wait, INVALID_HANDLE_VALUE);
    ok(Ret, "UnregisterWaitEx failed: %lu\n", GetLastError());
    ok(s_InCallback == 0, "Flags %lx, %lu ms: a callback still ran after UnregisterWaitEx returned\n", Flags, Milliseconds);
    Calls = s_Calls;
    trace("flags %lx, %lu ms: %ld callbacks in 1 s, %ld at most at the same time\n", Flags, Milliseconds, Calls, s_MaxInCallback);

    if (Flags & WT_EXECUTEINWAITTHREAD)
    {
        ok(s_MaxInCallback == 1, "Flags %lx, %lu ms: %ld callbacks ran at the same time\n", Flags, Milliseconds, s_MaxInCallback);
        ok(Calls >= 4 && Calls <= 11, "Flags %lx, %lu ms: %ld callbacks\n", Flags, Milliseconds, Calls);
    }
    else
    {
        ok(s_MaxInCallback >= 2, "Flags %lx, %lu ms: the callbacks did not overlap\n", Flags, Milliseconds);
        ok(Calls >= 8, "Flags %lx, %lu ms: %ld callbacks\n", Flags, Milliseconds, Calls);
    }

    Sleep(300);
    ok(s_Calls == Calls, "Flags %lx, %lu ms: a callback ran after UnregisterWaitEx returned\n", Flags, Milliseconds);
    CloseHandle(Object);
}

/* A wait whose object stays signalled, with callbacks that take a while, fires faster than the pool
   starts them: callbacks wait in the queue. Unregistering with an event is pending while callbacks
   run, the event is set once they returned, and no queued callback starts after UnregisterWaitEx
   returned (Windows 10 drops them) */
static void
TestUnregisterQueued(void)
{
    HANDLE Object, Completion, Wait;
    LONG CallsAtReturn;
    DWORD Result, Error;
    BOOL Ret;

    Object = CreateEventW(NULL, TRUE, TRUE, NULL);
    Completion = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_InCallback = 0;
    s_MaxInCallback = 0;
    s_Calls = 0;
    s_CallbackSleep = 500;

    Ret = RegisterWaitForSingleObject(&Wait, Object, OverlapCallback, NULL, INFINITE, WT_EXECUTEDEFAULT);
    ok(Ret, "RegisterWaitForSingleObject failed: %lu\n", GetLastError());
    Sleep(150);
    SetLastError(0xdeadbeef);
    Ret = UnregisterWaitEx(Wait, Completion);
    Error = GetLastError();
    CallsAtReturn = s_Calls;
    ok(!Ret && Error == ERROR_IO_PENDING, "UnregisterWaitEx(event) returned %d, error %lu\n", Ret, Error);
    Result = WaitForSingleObject(Completion, 5000);
    ok(Result == WAIT_OBJECT_0, "The event was not set: %lu\n", Result);
    ok(s_InCallback == 0, "%ld callbacks still ran when the event was set\n", s_InCallback);
    Sleep(700);
    ok(s_Calls == CallsAtReturn, "%ld callbacks started after UnregisterWaitEx returned\n", s_Calls - CallsAtReturn);
    trace("%ld callbacks started, %ld at most at the same time\n", CallsAtReturn, s_MaxInCallback);
    CloseHandle(Completion);
    CloseHandle(Object);
}

/* Unregister with an event at the moment the wait fires, many times: the event is set, and once it is,
   no callback runs or starts */
static void
TestUnregisterEventRace(ULONG Flags)
{
    HANDLE Object, Completion, Wait;
    LONG FiredAtEvent;
    int i, Bad = 0, Failed = 0, Pending = 0;
    DWORD Result;
    BOOL Ret;

    Object = CreateEventW(NULL, FALSE, FALSE, NULL);
    Completion = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < 500; i++)
    {
        s_RaceFired = 0;
        ResetEvent(Completion);
        Ret = RegisterWaitForSingleObject(&Wait, Object, RaceCallback, NULL, INFINITE, Flags);
        if (!Ret)
        {
            Failed++;
            continue;
        }
        SetEvent(Object);
        if (i % 3 == 1)
            YieldProcessor();
        else if (i % 3 == 2)
            Sleep(0);
        SetLastError(0xdeadbeef);
        Ret = UnregisterWaitEx(Wait, Completion);
        if (!Ret)
        {
            if (GetLastError() == ERROR_IO_PENDING)
                Pending++;
            else
                Failed++;
        }
        Result = WaitForSingleObject(Completion, 5000);
        if (Result != WAIT_OBJECT_0)
            Failed++;
        FiredAtEvent = s_RaceFired;
        if (s_RaceInCallback)
            Bad++;
        Sleep(0);
        if (s_RaceFired != FiredAtEvent)
            Bad++;
        ResetEvent(Object);
    }
    ok(Failed == 0, "Flags %lx: %d registers, unregisters or events failed\n", Flags, Failed);
    ok(Bad == 0, "Flags %lx: %d times a callback ran after the event was set\n", Flags, Bad);
    trace("flags %lx: the callback ran when the wait was unregistered %d times in 500\n", Flags, Pending);
    CloseHandle(Completion);
    CloseHandle(Object);
}

static void
TestPoolDispatch(void)
{
    TestCallbackBlocksOnOtherWait(WT_EXECUTEINWAITTHREAD);
    TestCallbackBlocksOnOtherWait(WT_EXECUTEDEFAULT);
    TestLongCallbackTimeouts(WT_EXECUTEINWAITTHREAD);
    TestLongCallbackTimeouts(WT_EXECUTEDEFAULT);
    TestCallbackThreads();
    TestUnregisterRunning(WT_EXECUTEDEFAULT);
    TestUnregisterRunning(WT_EXECUTELONGFUNCTION);
    TestUnregisterWhileThreadBusy(WT_EXECUTEINWAITTHREAD, FALSE);
    TestUnregisterWhileThreadBusy(WT_EXECUTEINWAITTHREAD, TRUE);
    TestUnregisterWhileThreadBusy(WT_EXECUTEINPERSISTENTTHREAD, FALSE);
    TestUnregisterQueued();
    TestOverlap(WT_EXECUTEDEFAULT, INFINITE);
    TestOverlap(WT_EXECUTEDEFAULT, 50);
    TestOverlap(WT_EXECUTEINWAITTHREAD, INFINITE);
    TestOverlap(WT_EXECUTEINWAITTHREAD, 50);
    TestUnregisterEventRace(WT_EXECUTEDEFAULT);
    TestUnregisterEventRace(WT_EXECUTEINWAITTHREAD);
}

START_TEST(RegisterWait)
{
    TestCompletionEventWithoutCallback();
    TestBlockingUnregister();
    TestNonBlockingUnregister();
    TestHandleBeforeCallback();
    TestManyWaits();
    TestCallbackException(WT_EXECUTEINWAITTHREAD);
    TestCallbackException(WT_EXECUTEDEFAULT);
    TestCallbackException(WT_EXECUTEINIOTHREAD);
    TestIdleLingerBoundary();
    TestSharedThread();
    TestSharedThreadCallbacks();
    TestPoolDispatch();
    TestThreadGrowth();
}
