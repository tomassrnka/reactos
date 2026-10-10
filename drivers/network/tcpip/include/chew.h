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
NTSTATUS ChewInit(PDEVICE_OBJECT DeviceObject);

/**
 * Shutdown CHEW, waits for remaining work items.
 */
VOID ChewShutdown(VOID);

/**
 * Waits until no work item is queued or running.
 */
VOID ChewWaitIdle(VOID);

/**
 * Work kept in the caller's storage, for work that must run even when no
 * work item can be allocated.
 */
typedef struct _CHEW_RESERVED_ITEM
{
    LIST_ENTRY Entry;
    VOID (*Worker)(PVOID WorkerContext);
    PVOID WorkerContext;
} CHEW_RESERVED_ITEM, *PCHEW_RESERVED_ITEM;

/**
 * Queues work in the caller's storage on the work item ChewInit allocated;
 * cannot fail.
 */
VOID ChewQueueReserved(PCHEW_RESERVED_ITEM Item, VOID (*Worker)(PVOID), PVOID WorkerContext);

/**
 * Creates and queues a work item.
 */
BOOLEAN ChewCreate(VOID (*Worker)(PVOID), PVOID WorkerContext);

#endif/*_REACTOS_CHEW_H*/
