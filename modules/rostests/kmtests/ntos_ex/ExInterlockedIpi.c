/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite ExInterlocked list operations against
 *              a holder of the same lock that waits for an IPI
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/*
 * Drivers may take the lock of ExInterlocked operations with
 * KeAcquireSpinLock too. A holder may then do something that waits for every
 * processor to take an IPI: KeIpiGenericCall, or a pool free that flushes the
 * TLB everywhere (special pool). The other processors run ExInterlocked
 * operations on the same lock meanwhile, so they must take the IPI while they
 * wait for the lock. If they spin with interrupts disabled the system hangs,
 * so a failure of this test is a hang, not a failed check.
 */

#define ENTRY_COUNT 64
#define MAX_WORKERS 31
#define RUN_SECONDS 3
#define KEIPI_TAG 'iIeK'

typedef struct _IPI_ENTRY
{
    LIST_ENTRY ListEntry;
    SINGLE_LIST_ENTRY SingleEntry;
} IPI_ENTRY, *PIPI_ENTRY;

typedef struct _IPI_WORKER
{
    HANDLE Handle;
    PKTHREAD Thread;
    ULONG Processor;
    ULONG Iterations;
    ULONG Lost;
    ULONG InterruptsOff;
} IPI_WORKER, *PIPI_WORKER;

static KSPIN_LOCK Lock;
static LIST_ENTRY ListHead;
static SINGLE_LIST_ENTRY SingleHead;
static ULONG Counter;
static volatile LONG Stop;
static volatile LONG Started;
static IPI_ENTRY Entries[ENTRY_COUNT];
static IPI_WORKER Workers[MAX_WORKERS];
static volatile LONG IpiCount;

static
BOOLEAN
InterruptsEnabled(VOID)
{
#if defined(_M_IX86) || defined(_M_AMD64)
    return (__readeflags() & 0x200) != 0;
#else
    return TRUE;
#endif
}

static
VOID
NTAPI
WorkerThread(
    _In_ PVOID Context)
{
    PIPI_WORKER Worker = Context;
    PLIST_ENTRY Entry;
    PSINGLE_LIST_ENTRY Single;
    KIRQL OldIrql = PASSIVE_LEVEL;
    BOOLEAN Raised;

    KeSetSystemAffinityThread(AFFINITY_MASK(Worker->Processor));
    InterlockedIncrement(&Started);

    while (!Stop)
    {
        /* Drivers use these at DISPATCH_LEVEL as often as below it */
        Raised = (Worker->Iterations & 1) != 0;
        if (Raised)
            KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

        /* Rotate an entry through the list and the stack */
        Entry = ExInterlockedRemoveHeadList(&ListHead, &Lock);
        if (Entry)
            ExInterlockedInsertTailList(&ListHead, Entry, &Lock);
        else
            Worker->Lost++;

        Single = ExInterlockedPopEntryList(&SingleHead, &Lock);
        if (Single)
            ExInterlockedPushEntryList(&SingleHead, Single, &Lock);
        else
            Worker->Lost++;

        ExInterlockedAddUlong(&Counter, 1, &Lock);

        /* Every operation returns with the caller's interrupt state */
        if (!InterruptsEnabled())
            Worker->InterruptsOff++;

        if (Raised)
            KeLowerIrql(OldIrql);

        Worker->Iterations++;
    }

    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
ULONG_PTR
NTAPI
IpiWorker(
    _In_ ULONG_PTR Argument)
{
    UNREFERENCED_PARAMETER(Argument);
    InterlockedIncrement(&IpiCount);
    return 0;
}

typedef enum _HOLDER_MODE
{
    HolderIpiGenericCall,
    HolderPoolFree,
} HOLDER_MODE;

static
VOID
TestHolder(
    _In_ HOLDER_MODE Mode,
    _In_ PCSTR Name)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    ULONG Processors = KeNumberProcessors;
    ULONG WorkerCount, i, Count, Iterations = 0, Total = 0;
    ULONGLONG Start, Now, LastReport;
    LARGE_INTEGER Delay;
    KIRQL OldIrql;
    PVOID Block;
    PLIST_ENTRY Entry;
    PSINGLE_LIST_ENTRY Single;

    WorkerCount = min(Processors - 1, MAX_WORKERS);

    KeInitializeSpinLock(&Lock);
    InitializeListHead(&ListHead);
    SingleHead.Next = NULL;
    for (i = 0; i < ENTRY_COUNT; i++)
    {
        InsertTailList(&ListHead, &Entries[i].ListEntry);
        PushEntryList(&SingleHead, &Entries[i].SingleEntry);
    }
    Counter = 0;
    Stop = 0;
    Started = 0;
    IpiCount = 0;
    RtlZeroMemory(Workers, sizeof(Workers));

    /* The holder stays on processor 0, one worker on each other processor */
    KeSetSystemAffinityThread(AFFINITY_MASK(0));

    for (i = 0; i < WorkerCount; i++)
    {
        Workers[i].Processor = i + 1;
        InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = PsCreateSystemThread(&Workers[i].Handle, SYNCHRONIZE, &ObjectAttributes,
                                      NULL, NULL, WorkerThread, &Workers[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
        {
            WorkerCount = i;
            break;
        }
        Status = ObReferenceObjectByHandle(Workers[i].Handle, SYNCHRONIZE, *PsThreadType,
                                           KernelMode, (PVOID*)&Workers[i].Thread, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    Delay.QuadPart = -10 * 1000;
    while (Started != (LONG)WorkerCount)
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    DbgPrint("ExInterlockedIpi: %s: %lu workers\n", Name, WorkerCount);

    Start = LastReport = KeQueryInterruptTime();
    do
    {
        Block = NULL;
        if (Mode == HolderPoolFree)
        {
            Block = ExAllocatePoolWithTag(NonPagedPool, 64, KEIPI_TAG);
            if (!Block)
            {
                ok(FALSE, "%s: allocation failed\n", Name);
                break;
            }
        }

        KeAcquireSpinLock(&Lock, &OldIrql);

        /* Hold the lock long enough that the workers wait for it */
        KeStallExecutionProcessor(5);

        if (Mode == HolderIpiGenericCall)
            KeIpiGenericCall(IpiWorker, 0);
        else
            ExFreePoolWithTag(Block, KEIPI_TAG);

        KeReleaseSpinLock(&Lock, OldIrql);
        Iterations++;

        /* Say we are alive: a hang leaves the debug output quiet */
        Now = KeQueryInterruptTime();
        if (Now - LastReport >= 10ULL * 1000 * 1000)
        {
            DbgPrint("ExInterlockedIpi: %s: %lu holder iterations\n", Name, Iterations);
            LastReport = Now;
        }
    } while (Now - Start < RUN_SECONDS * 10ULL * 1000 * 1000);

    InterlockedExchange(&Stop, 1);
    for (i = 0; i < WorkerCount; i++)
    {
        if (Workers[i].Thread)
        {
            Status = KeWaitForSingleObject(Workers[i].Thread, Executive, KernelMode, FALSE, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ObDereferenceObject(Workers[i].Thread);
        }
        ZwClose(Workers[i].Handle);

        ok(Workers[i].Iterations != 0, "%s: worker %lu made no progress\n", Name, i);
        ok_eq_ulong(Workers[i].Lost, 0UL);
        ok_eq_ulong(Workers[i].InterruptsOff, 0UL);
        Total += Workers[i].Iterations;
    }

    KeRevertToUserAffinityThread();

    /* Nothing got lost or corrupted on the way */
    ok_eq_ulong(Counter, Total);
    for (Count = 0, Entry = ListHead.Flink; Entry != &ListHead && Count <= ENTRY_COUNT; Entry = Entry->Flink)
        Count++;
    ok_eq_ulong(Count, (ULONG)ENTRY_COUNT);
    for (Count = 0, Single = SingleHead.Next; Single && Count <= ENTRY_COUNT; Single = Single->Next)
        Count++;
    ok_eq_ulong(Count, (ULONG)ENTRY_COUNT);
    ok(Iterations != 0, "%s: no holder iterations\n", Name);
    if (Mode == HolderIpiGenericCall)
        ok_eq_long(IpiCount, (LONG)(Iterations * (WorkerCount + 1)));

    trace("%s: %lu holder iterations, %lu worker iterations on %lu workers\n",
          Name, Iterations, Total, WorkerCount);
    DbgPrint("ExInterlockedIpi: %s: done, %lu holder iterations, %lu worker iterations\n",
             Name, Iterations, Total);
}

START_TEST(ExInterlockedIpi)
{
    if (skip(KeNumberProcessors > 1, "Needs more than one processor\n"))
        return;

    /* Frees flush the TLB on every processor when the tag is in SpecialPoolTags */
    TestHolder(HolderPoolFree, "pool free");
    TestHolder(HolderIpiGenericCall, "KeIpiGenericCall");
}
