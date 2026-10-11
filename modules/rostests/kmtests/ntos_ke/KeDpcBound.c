/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite: a processor busy with DPCs does not starve threads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define HOG_PRIORITY (LOW_REALTIME_PRIORITY)
#define HIGH_HOG_PRIORITY (LOW_REALTIME_PRIORITY + 1)
#define DPC_STALL_US (40 * 1000)    /* longer than two clock ticks */
#define DPC_PERIOD_MS 100
#define TEST_TIME (3 * 1000 * 1000 * 10) /* 3 seconds */
#define MIN_WAKES 5

typedef struct _WORKER
{
    HANDLE Handle;
    PKTHREAD Thread;
    KAFFINITY Affinity;
    KPRIORITY Priority;
    CCHAR IdealProcessor;
    BOOLEAN Sleeps;
    volatile LONG Started;
    volatile LONG Count;
    volatile LONG Wakes;
} WORKER, *PWORKER;

static volatile LONG StopWorkers;
static volatile LONG DpcRuns;
static volatile LONG LongDpcRuns;

static KSTART_ROUTINE WorkerThread;
static
VOID
NTAPI
WorkerThread(
    _In_ PVOID Context)
{
    PWORKER Worker = Context;
    LARGE_INTEGER Timeout;
    ULONG Spin;

    KeSetPriorityThread(KeGetCurrentThread(), Worker->Priority);
    KeSetIdealProcessorThread(KeGetCurrentThread(), Worker->IdealProcessor);
    KeSetSystemAffinityThread(Worker->Affinity);
    InterlockedExchange(&Worker->Started, 1);

    Timeout.QuadPart = -10 * 1000; /* 1 ms */
    while (!StopWorkers)
    {
        InterlockedIncrement(&Worker->Count);
        if (Worker->Sleeps)
        {
            /* Every wake-up readies the thread again */
            for (Spin = 0; Spin < 10000 && !StopWorkers; Spin++)
                InterlockedIncrement(&Worker->Count);
            KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
            InterlockedIncrement(&Worker->Wakes);
        }
    }

    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static KDEFERRED_ROUTINE BusyDpc;
static
VOID
NTAPI
BusyDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    LARGE_INTEGER Start, End;

    /* One DPC run that spans clock ticks, as under a steady interrupt load */
    if (StopWorkers)
        return;
    KeQueryTickCount(&Start);
    KeStallExecutionProcessor(DPC_STALL_US);
    KeQueryTickCount(&End);
    InterlockedIncrement(&DpcRuns);
    if (End.QuadPart - Start.QuadPart >= 2)
        InterlockedIncrement(&LongDpcRuns);
}

static
NTSTATUS
StartWorker(
    _Inout_ PWORKER Worker)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Worker->Handle, SYNCHRONIZE, &ObjectAttributes,
                                  NULL, NULL, WorkerThread, Worker);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ObReferenceObjectByHandle(Worker->Handle, SYNCHRONIZE, *PsThreadType,
                                       KernelMode, (PVOID *)&Worker->Thread, NULL);
    if (!NT_SUCCESS(Status))
    {
        StopWorkers = 1;
        ZwWaitForSingleObject(Worker->Handle, FALSE, NULL);
        ZwClose(Worker->Handle);
        Worker->Handle = NULL;
    }
    return Status;
}

/*
 * Processor A runs a CPU-bound thread at priority 16 and, every 100 ms, a
 * DPC that runs for 40 ms, so the scheduler sees it as busy with DPCs.
 * Processor B runs a CPU-bound thread at priority 17. Thread T (priority
 * 16, may run on A and B, ideal processor A) sleeps 1 ms at a time, so it
 * is made ready again and again. B never runs T ahead of its priority-17
 * thread, and A takes a thread from B's ready list only above priority 16:
 * T runs only if it is given to A, where it shares A with the equal
 * priority hog.
 */
START_TEST(KeDpcBound)
{
    NTSTATUS Status;
    KAFFINITY Active, Mask, A, B;
    KPRIORITY OldPriority;
    ULONG i, Rounds, Number;
    LONG Wakes;
    LARGE_INTEGER Timeout, Due;
    KDPC Dpc;
    KTIMER Timer;
    static WORKER Workers[3];
    PWORKER HogA = &Workers[0], HogB = &Workers[1], T = &Workers[2];

    Active = KeQueryActiveProcessors();
    Mask = Active & (~Active + 1);
    if (skip(((Active & ~Mask) & ((Active & ~Mask) - 1)) != 0, "Needs at least three processors\n"))
        return;

    /* The test thread stays on the lowest processor; A and B are the next two */
    KeSetSystemAffinityThread(Mask);
    OldPriority = KeSetPriorityThread(KeGetCurrentThread(), HIGH_HOG_PRIORITY + 1);
    A = (Active & ~Mask) & (~(Active & ~Mask) + 1);
    B = (Active & ~Mask & ~A) & (~(Active & ~Mask & ~A) + 1);
    BitScanForwardAffinity(&Number, A);

    StopWorkers = 0;
    DpcRuns = 0;
    RtlZeroMemory(Workers, sizeof(Workers));
    HogA->Affinity = A;
    HogA->Priority = HOG_PRIORITY;
    HogA->IdealProcessor = (CCHAR)Number;
    HogB->Affinity = B;
    HogB->Priority = HIGH_HOG_PRIORITY;
    BitScanForwardAffinity(&i, B);
    HogB->IdealProcessor = (CCHAR)i;
    T->Affinity = A | B;
    T->Priority = HOG_PRIORITY;
    T->IdealProcessor = (CCHAR)Number;
    T->Sleeps = TRUE;

    /* The long DPCs on A */
    KeInitializeDpc(&Dpc, BusyDpc, NULL);
    KeSetTargetProcessorDpc(&Dpc, (CCHAR)Number);
    KeInitializeTimerEx(&Timer, NotificationTimer);
    Due.QuadPart = -10 * 1000 * 10; /* 10 ms */
    KeSetTimerEx(&Timer, Due, DPC_PERIOD_MS, &Dpc);

    Timeout.QuadPart = -10 * 1000 * 10; /* 10 ms */
    for (i = 0; i < RTL_NUMBER_OF(Workers); i++)
    {
        Status = StartWorker(&Workers[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            break;
        for (Rounds = 0; !Workers[i].Started && Rounds < 500; Rounds++)
            KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
        ok(Workers[i].Started, "Worker %lu did not start\n", i);
    }

    /* Let the three run against the DPCs, then count T's wake-ups */
    DpcRuns = 0;
    LongDpcRuns = 0;
    Wakes = T->Wakes;
    Due.QuadPart = -TEST_TIME;
    KeDelayExecutionThread(KernelMode, FALSE, &Due);
    Wakes = T->Wakes - Wakes;
    trace("DPC runs %ld (%ld over two ticks), hog A %ld, hog B %ld, T count %ld, T wake-ups %ld\n",
          DpcRuns, LongDpcRuns, HogA->Count, HogB->Count, T->Count, Wakes);
    ok(LongDpcRuns >= 10, "Only %ld of %ld DPC runs spanned two clock ticks\n", LongDpcRuns, DpcRuns);
    ok(HogA->Count > 0 && HogB->Count > 0, "A hog never ran\n");
    ok(Wakes >= MIN_WAKES, "T woke up only %ld times in 3 seconds: starved\n", Wakes);

    /* Stop everything; T may run only now */
    KeCancelTimer(&Timer);
    StopWorkers = 1;
    KeRevertToUserAffinityThread();
    KeFlushQueuedDpcs();
    Timeout.QuadPart = -10 * 1000 * 1000 * 10; /* 10 seconds */
    for (i = 0; i < RTL_NUMBER_OF(Workers); i++)
    {
        if (!Workers[i].Thread)
            continue;
        Status = KeWaitForSingleObject(Workers[i].Thread, Executive, KernelMode, FALSE, &Timeout);
        ok(Status == STATUS_SUCCESS, "Worker %lu did not exit: 0x%lx\n", i, Status);
        /* The worker runs driver code: hang rather than unload under it */
        if (Status != STATUS_SUCCESS)
            KeWaitForSingleObject(Workers[i].Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Workers[i].Thread);
        ZwClose(Workers[i].Handle);
    }

    KeSetPriorityThread(KeGetCurrentThread(), OldPriority);
}
