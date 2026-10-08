/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Tests for RegisterWaitForSingleObject, UnregisterWait and UnregisterWaitEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */
#include "precomp.h"

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

START_TEST(RegisterWait)
{
    TestCompletionEventWithoutCallback();
    TestBlockingUnregister();
    TestNonBlockingUnregister();
    TestHandleBeforeCallback();
}
