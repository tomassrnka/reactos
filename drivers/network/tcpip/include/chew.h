/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            include/reactos/chew/chew.h
 * PURPOSE:         Common Highlevel Executive Worker
 *
 * PROGRAMMERS:     arty (ayerkes@speakeasy.net)
 */

#ifndef _REACTOS_CHEW_H
#define _REACTOS_CHEW_H

/**
 * Initialize CHEW, given a device object (since IoAllocateWorkItem relies on
 * it).
 */
VOID ChewInit(PDEVICE_OBJECT DeviceObject);

/**
 * Shutdown CHEW, waits for remaining work items.
 */
VOID ChewShutdown(VOID);

/**
 * Creates and queues a work item.
 */
BOOLEAN ChewCreate(VOID (*Worker)(PVOID), PVOID WorkerContext);

/**
 * A queue whose entries are handed to its worker one at a time, in the order
 * they were inserted, by at most one work item at a time.
 */
typedef struct _CHEW_SERIAL_QUEUE
{
    KSPIN_LOCK Lock;
    LIST_ENTRY List;
    PIO_WORKITEM WorkItem;
    VOID (*Worker)(PLIST_ENTRY Entry);
    ULONG Count;
    ULONG Limit;
    BOOLEAN Running;
    BOOLEAN Closed;
    KEVENT Idle;
} CHEW_SERIAL_QUEUE, *PCHEW_SERIAL_QUEUE;

/**
 * Initializes a serial queue that holds at most Limit entries. Fails only
 * when its work item cannot be allocated.
 */
BOOLEAN ChewSerialInit(PCHEW_SERIAL_QUEUE Queue, VOID (*Worker)(PLIST_ENTRY Entry), ULONG Limit);

/**
 * Appends an entry; callable at IRQL <= DISPATCH_LEVEL. Fails when the queue
 * is full or ChewSerialRundown has begun; the caller keeps the entry then.
 */
BOOLEAN ChewSerialInsert(PCHEW_SERIAL_QUEUE Queue, PLIST_ENTRY Entry);

/**
 * Refuses further entries, waits until the worker has handled every queued
 * entry and no longer touches the queue, then frees the work item. Called at
 * PASSIVE_LEVEL; calling it again does nothing.
 */
VOID ChewSerialRundown(PCHEW_SERIAL_QUEUE Queue);

#endif/*_REACTOS_CHEW_H*/
