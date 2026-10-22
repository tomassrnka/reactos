/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for DeleteTimerQueueEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef struct _DELETE_CONTEXT
{
    HANDLE Queue;
    HANDLE CompletionEvent;
    HANDLE ReturnedEvent;
    BOOL Result;
    DWORD Error;
    DWORD EarlyWait;
} DELETE_CONTEXT, *PDELETE_CONTEXT;

static
VOID
CALLBACK
DeleteOwnQueue(
    _In_ PVOID Parameter,
    _In_ BOOLEAN TimerOrWaitFired)
{
    PDELETE_CONTEXT Context = Parameter;

    SetLastError(0xdeadbeef);
    Context->Result = DeleteTimerQueueEx(Context->Queue, Context->CompletionEvent);
    Context->Error = GetLastError();
    /* This callback is still running, so the queue cannot be done yet */
    Context->EarlyWait = WaitForSingleObject(Context->CompletionEvent, 0);
    SetEvent(Context->ReturnedEvent);
}

static
VOID
TestDeleteFromCallback(
    _In_ ULONG Flags)
{
    PDELETE_CONTEXT Context;
    HANDLE Timer;
    DWORD Wait;

    /* Not on the stack: a stuck callback keeps using it */
    Context = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Context));
    if (!Context)
    {
        skip("Out of memory\n");
        return;
    }
    Context->CompletionEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Context->ReturnedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Context->Queue = CreateTimerQueue();
    ok(Context->Queue != NULL, "CreateTimerQueue failed with %lu\n", GetLastError());
    if (!Context->Queue || !Context->CompletionEvent || !Context->ReturnedEvent)
    {
        skip("Setup failed\n");
        return;
    }

    if (!CreateTimerQueueTimer(&Timer, Context->Queue, DeleteOwnQueue, Context, 10, 0, Flags))
    {
        skip("CreateTimerQueueTimer failed with %lu\n", GetLastError());
        return;
    }

    /* DeleteTimerQueueEx must return without waiting for the callback */
    Wait = WaitForSingleObject(Context->ReturnedEvent, 10000);
    ok(Wait == WAIT_OBJECT_0, "Flags 0x%lx: DeleteTimerQueueEx did not return (%lu)\n", Flags, Wait);
    if (Wait != WAIT_OBJECT_0)
        return;
    ok(Context->Result || Context->Error == ERROR_IO_PENDING,
       "Flags 0x%lx: DeleteTimerQueueEx returned %d, error %lu\n",
       Flags, Context->Result, Context->Error);

    ok(Context->EarlyWait == WAIT_TIMEOUT,
       "Flags 0x%lx: completion event set before the callback returned (%lu)\n",
       Flags, Context->EarlyWait);

    /* The event is set once the callback has returned */
    Wait = WaitForSingleObject(Context->CompletionEvent, 10000);
    ok(Wait == WAIT_OBJECT_0, "Flags 0x%lx: completion event not set (%lu)\n", Flags, Wait);

    CloseHandle(Context->CompletionEvent);
    CloseHandle(Context->ReturnedEvent);
    HeapFree(GetProcessHeap(), 0, Context);
}

START_TEST(DeleteTimerQueueEx)
{
    TestDeleteFromCallback(WT_EXECUTEINTIMERTHREAD);
    TestDeleteFromCallback(WT_EXECUTEDEFAULT);
}
