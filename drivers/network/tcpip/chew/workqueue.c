/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            lib/drivers/chew/workqueue.c
 * PURPOSE:         Common Highlevel Executive Worker
 *
 * PROGRAMMERS:     arty (ayerkes@speakeasy.net)
 */

#include <wdm.h>

#include <chew.h>

#define FOURCC(w,x,y,z) (((w) << 24) | ((x) << 16) | ((y) << 8) | (z))
#define CHEW_TAG FOURCC('C','H','E','W')

PDEVICE_OBJECT WorkQueueDevice;
LIST_ENTRY     WorkQueue;
KSPIN_LOCK     WorkQueueLock;
KEVENT         WorkQueueClear;
PIO_WORKITEM   ReserveWorkItem;
LIST_ENTRY     ReserveQueue;
BOOLEAN        ReserveRunning;

typedef struct _WORK_ITEM
{
    LIST_ENTRY Entry;
    PIO_WORKITEM WorkItem;
    VOID (*Worker)(PVOID WorkerContext);
    PVOID WorkerContext;
} WORK_ITEM, *PWORK_ITEM;

NTSTATUS ChewInit(PDEVICE_OBJECT DeviceObject)
{
    WorkQueueDevice = DeviceObject;
    InitializeListHead(&WorkQueue);
    InitializeListHead(&ReserveQueue);
    KeInitializeSpinLock(&WorkQueueLock);
    KeInitializeEvent(&WorkQueueClear, NotificationEvent, TRUE);

    ReserveWorkItem = IoAllocateWorkItem(DeviceObject);
    return ReserveWorkItem ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

VOID ChewWaitIdle(VOID)
{
    KeWaitForSingleObject(&WorkQueueClear, Executive, KernelMode, FALSE, NULL);
}

VOID ChewShutdown(VOID)
{
    ChewWaitIdle();

    if (ReserveWorkItem)
    {
        IoFreeWorkItem(ReserveWorkItem);
        ReserveWorkItem = NULL;
    }
}

VOID NTAPI ChewWorkItem(PDEVICE_OBJECT DeviceObject, PVOID ChewItem)
{
    PWORK_ITEM WorkItem = ChewItem;
    KIRQL OldIrql;

    WorkItem->Worker(WorkItem->WorkerContext);

    IoFreeWorkItem(WorkItem->WorkItem);

    KeAcquireSpinLock(&WorkQueueLock, &OldIrql);
    RemoveEntryList(&WorkItem->Entry);

    if (IsListEmpty(&WorkQueue) && !ReserveRunning)
        KeSetEvent(&WorkQueueClear, 0, FALSE);

    KeReleaseSpinLock(&WorkQueueLock, OldIrql);

    ExFreePoolWithTag(WorkItem, CHEW_TAG);
}

static VOID NTAPI ChewReservedWorkItem(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    PCHEW_RESERVED_ITEM Item;
    KIRQL OldIrql;

    for (;;)
    {
        KeAcquireSpinLock(&WorkQueueLock, &OldIrql);

        if (IsListEmpty(&ReserveQueue))
        {
            ReserveRunning = FALSE;
            if (IsListEmpty(&WorkQueue))
                KeSetEvent(&WorkQueueClear, 0, FALSE);
            KeReleaseSpinLock(&WorkQueueLock, OldIrql);
            return;
        }

        Item = CONTAINING_RECORD(RemoveHeadList(&ReserveQueue), CHEW_RESERVED_ITEM, Entry);
        KeReleaseSpinLock(&WorkQueueLock, OldIrql);

        /* The worker may free the item's storage */
        Item->Worker(Item->WorkerContext);
    }
}

VOID ChewQueueReserved(PCHEW_RESERVED_ITEM Item, VOID (*Worker)(PVOID), PVOID WorkerContext)
{
    KIRQL OldIrql;
    BOOLEAN Start;

    Item->Worker = Worker;
    Item->WorkerContext = WorkerContext;

    KeAcquireSpinLock(&WorkQueueLock, &OldIrql);
    InsertTailList(&ReserveQueue, &Item->Entry);
    Start = !ReserveRunning;
    ReserveRunning = TRUE;
    KeClearEvent(&WorkQueueClear);
    KeReleaseSpinLock(&WorkQueueLock, OldIrql);

    /* One run drains the queue; the work item is queued again only after that run ended */
    if (Start)
        IoQueueWorkItem(ReserveWorkItem, ChewReservedWorkItem, DelayedWorkQueue, NULL);
}

BOOLEAN ChewCreate(VOID (*Worker)(PVOID), PVOID WorkerContext)
{
    PWORK_ITEM Item;
    Item = ExAllocatePoolWithTag(NonPagedPool,
                                 sizeof(WORK_ITEM),
                                 CHEW_TAG);

    if (Item)
    {
        Item->WorkItem = IoAllocateWorkItem(WorkQueueDevice);
        if (!Item->WorkItem)
        {
            ExFreePool(Item);
            return FALSE;
        }

        Item->Worker = Worker;
        Item->WorkerContext = WorkerContext;
        ExInterlockedInsertTailList(&WorkQueue, &Item->Entry, &WorkQueueLock);
        KeClearEvent(&WorkQueueClear);
        IoQueueWorkItem(Item->WorkItem, ChewWorkItem, DelayedWorkQueue, Item);

        return TRUE;
    }
    else
    {
        return FALSE;
    }
}
