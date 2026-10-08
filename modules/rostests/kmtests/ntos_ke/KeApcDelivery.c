/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite kernel APC interrupt delivery test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/* A missed APC means that an APC interrupt was never ended on that
   processor. On the x86 APIC HALs a thread switch between enabling
   interrupts and the EOI did that; the window is narrow, so the test
   hits it only by chance. */

#define THREADS_PER_PROCESSOR 3
#define TEST_SECONDS 5
#define TEST_PRIORITY (LOW_REALTIME_PRIORITY + 2)

typedef struct _APC_THREAD
{
    HANDLE Handle;
    KAPC Apc;
    volatile LONG Ran;
    ULONG Queued;
    ULONG Missed;
    ULONG MissProcessor;
    BOOLEAN InsertFailed;
} APC_THREAD, *PAPC_THREAD;

static KEVENT StartEvent;
static ULONGLONG StopTime;

static
VOID
NTAPI
ApcKernelRoutine(
    _In_ PKAPC Apc,
    _Inout_ PKNORMAL_ROUTINE *NormalRoutine,
    _Inout_ PVOID *NormalContext,
    _Inout_ PVOID *SystemArgument1,
    _Inout_ PVOID *SystemArgument2)
{
    PAPC_THREAD Context = CONTAINING_RECORD(Apc, APC_THREAD, Apc);

    InterlockedExchange(&Context->Ran, 1);
}

static
VOID
NTAPI
ApcThread(
    _In_ PVOID Parameter)
{
    PAPC_THREAD Context = Parameter;
    LARGE_INTEGER Interval;
    ULONG Processor, Spin;

    KeSetPriorityThread(KeGetCurrentThread(), TEST_PRIORITY);
    KeWaitForSingleObject(&StartEvent, Executive, KernelMode, FALSE, NULL);

    /* The shortest relative wait; it ends at the next clock tick */
    Interval.QuadPart = -1;

    while ((ULONGLONG)KeQueryInterruptTime() < StopTime)
    {
        Context->Ran = 0;
        KeInitializeApc(&Context->Apc, KeGetCurrentThread(), OriginalApcEnvironment,
                        ApcKernelRoutine, NULL, NULL, KernelMode, NULL);
        if (!KeInsertQueueApc(&Context->Apc, NULL, NULL, IO_NO_INCREMENT))
        {
            Context->InsertFailed = TRUE;
            break;
        }
        Processor = KeGetCurrentProcessorNumber();
        Context->Queued++;

        /* At PASSIVE_LEVEL the APC interrupt runs a special kernel APC as
           soon as KeInsertQueueApc lowers the IRQL. The short spin only
           allows for the interrupt delivery latency of real hardware. */
        for (Spin = 0; !Context->Ran && Spin < 1000; Spin++)
            YieldProcessor();

        if (!Context->Ran)
        {
            if (Context->Missed++ == 0)
                Context->MissProcessor = Processor;

            /* Leaving a critical region delivers pending kernel APCs, and
               so does a wait, which switches the thread out and back in */
            KeEnterCriticalRegion();
            KeLeaveCriticalRegion();
            while (!Context->Ran)
                KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        }

        /* Wait now and then, so that threads are also switched in from a wait */
        if ((Context->Queued % 64) == 0)
            KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(KeApcDelivery)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PAPC_THREAD Threads;
    ULONG ThreadCount, Created, i;
    ULONG Queued = 0, Missed = 0, MissProcessor = 0;
    BOOLEAN InsertFailed = FALSE;

    ThreadCount = KeNumberProcessors * THREADS_PER_PROCESSOR;
    Threads = ExAllocatePoolWithTag(NonPagedPool, ThreadCount * sizeof(*Threads), 'cpAK');
    if (skip(Threads != NULL, "Out of memory\n"))
        return;
    RtlZeroMemory(Threads, ThreadCount * sizeof(*Threads));

    KeInitializeEvent(&StartEvent, NotificationEvent, FALSE);
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    for (Created = 0; Created < ThreadCount; Created++)
    {
        Status = PsCreateSystemThread(&Threads[Created].Handle, SYNCHRONIZE, &ObjectAttributes,
                                      NULL, NULL, ApcThread, &Threads[Created]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            break;
    }

    StopTime = KeQueryInterruptTime() + TEST_SECONDS * 10000000ULL;
    KeSetEvent(&StartEvent, IO_NO_INCREMENT, FALSE);

    for (i = 0; i < Created; i++)
    {
        /* The threads use this driver's code and data: wait for all of them */
        Status = ZwWaitForSingleObject(Threads[i].Handle, FALSE, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Threads[i].Handle);

        Queued += Threads[i].Queued;
        if (Threads[i].Missed && !Missed)
            MissProcessor = Threads[i].MissProcessor;
        Missed += Threads[i].Missed;
        InsertFailed |= Threads[i].InsertFailed;
    }

    trace("%lu threads, %lu APCs queued, %lu not delivered at once\n", Created, Queued, Missed);
    ok(Queued != 0, "No APC was queued\n");
    ok(!InsertFailed, "KeInsertQueueApc failed\n");
    ok(Missed == 0, "%lu of %lu APCs to the current thread had not run right after KeInsertQueueApc returned (first on processor %lu)\n",
       Missed, Queued, MissProcessor);

    ExFreePoolWithTag(Threads, 'cpAK');
}
