/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite system work queue test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

//#define NDEBUG
#include <debug.h>

/*
 * Every worker thread of a queue blocks in a work item that waits for an item
 * queued behind them. The queue only drains if the system adds worker threads:
 * the critical queue does that as soon as an item waits with no free worker,
 * and the balance set manager adds one to any queue that made no progress for a
 * second while items were queued. The waiter count exceeds the static worker
 * threads of both queues (8 critical and 12 delayed on ReactOS, fewer on
 * Windows), and stays within the 16 dynamic threads a queue may add.
 */

typedef struct _WQ_TEST_CONTEXT
{
    KEVENT Release;
    KEVENT AllDone;
    LONG Started;
    LONG Done;
    LONG TimedOut;
    LONG Total;
} WQ_TEST_CONTEXT, *PWQ_TEST_CONTEXT;

typedef struct _WQ_TEST_ITEM
{
    WORK_QUEUE_ITEM WorkItem;
    PWQ_TEST_CONTEXT Context;
} WQ_TEST_ITEM, *PWQ_TEST_ITEM;

static
VOID
WqFinishItem(
    _In_ PWQ_TEST_CONTEXT Context)
{
    if (InterlockedIncrement(&Context->Done) == Context->Total)
        KeSetEvent(&Context->AllDone, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
WqWaiterRoutine(
    _In_ PVOID Parameter)
{
    PWQ_TEST_CONTEXT Context = ((PWQ_TEST_ITEM)Parameter)->Context;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    InterlockedIncrement(&Context->Started);

    /* Bounded, so that a broken queue does not keep the workers forever */
    Timeout.QuadPart = -60LL * 10 * 1000 * 1000;
    Status = KeWaitForSingleObject(&Context->Release, Executive, KernelMode, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
        InterlockedIncrement(&Context->TimedOut);

    WqFinishItem(Context);
}

static
VOID
NTAPI
WqReleaserRoutine(
    _In_ PVOID Parameter)
{
    PWQ_TEST_CONTEXT Context = ((PWQ_TEST_ITEM)Parameter)->Context;

    InterlockedIncrement(&Context->Started);
    KeSetEvent(&Context->Release, IO_NO_INCREMENT, FALSE);
    WqFinishItem(Context);
}

static
VOID
TestBlockedQueue(
    _In_ WORK_QUEUE_TYPE QueueType,
    _In_ ULONG Waiters,
    _In_ ULONG MaxSeconds)
{
    PWQ_TEST_CONTEXT Context;
    PWQ_TEST_ITEM Items;
    ULONG i, Total = Waiters + 1;
    ULONGLONG Start, Elapsed;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Context = ExAllocatePoolZero(NonPagedPool, sizeof(*Context), 'tqWK');
    Items = ExAllocatePoolZero(NonPagedPool, Total * sizeof(*Items), 'tqWK');
    if (skip(Context && Items, "Out of memory\n"))
    {
        if (Context) ExFreePoolWithTag(Context, 'tqWK');
        if (Items) ExFreePoolWithTag(Items, 'tqWK');
        return;
    }

    KeInitializeEvent(&Context->Release, NotificationEvent, FALSE);
    KeInitializeEvent(&Context->AllDone, NotificationEvent, FALSE);
    Context->Total = (LONG)Total;

    Start = KeQueryInterruptTime();
    for (i = 0; i < Total; i++)
    {
        Items[i].Context = Context;
        ExInitializeWorkItem(&Items[i].WorkItem,
                             (i < Waiters) ? WqWaiterRoutine : WqReleaserRoutine,
                             &Items[i]);
        ExQueueWorkItem(&Items[i].WorkItem, QueueType);
    }

    Timeout.QuadPart = -(LONGLONG)MaxSeconds * 10 * 1000 * 1000;
    Status = KeWaitForSingleObject(&Context->AllDone, Executive, KernelMode, FALSE, &Timeout);
    Elapsed = (KeQueryInterruptTime() - Start) / (10 * 1000);
    ok(Status == STATUS_SUCCESS,
       "Queue %d: %ld of %lu items done, %ld started, after %I64u ms\n",
       QueueType, Context->Done, Total, Context->Started, Elapsed);
    ok_eq_long(Context->TimedOut, 0L);
    trace("Queue %d: %lu items done after %I64u ms\n", QueueType, Total, Elapsed);

    if (Status != STATUS_SUCCESS)
    {
        /* Let the waiters go; the items stay allocated if they still do not finish */
        KeSetEvent(&Context->Release, IO_NO_INCREMENT, FALSE);
        Timeout.QuadPart = -90LL * 10 * 1000 * 1000;
        Status = KeWaitForSingleObject(&Context->AllDone, Executive, KernelMode, FALSE, &Timeout);
        if (Status != STATUS_SUCCESS)
            return;
    }

    /* The last routine may still be returning from the event it set */
    Timeout.QuadPart = -100LL * 10 * 1000;
    KeDelayExecutionThread(KernelMode, FALSE, &Timeout);

    ExFreePoolWithTag(Items, 'tqWK');
    ExFreePoolWithTag(Context, 'tqWK');
}

START_TEST(ExWorkQueue)
{
    /* Dynamic threads are created on demand: no waiting for the balance set manager's timer */
    TestBlockedQueue(CriticalWorkQueue, 11, 30);

    /* Only the deadlock detection adds threads, one per second per stuck queue */
    TestBlockedQueue(DelayedWorkQueue, 15, 60);
}
