/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite thread affinity change stress test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define HOPPERS_MAX 8
#define SPINNERS_MAX 4
#define STRESS_TIME (3 * 1000 * 1000 * 10) /* 3 seconds */

typedef struct _WORKER
{
    HANDLE Handle;
    PKTHREAD Thread;
    ULONG Index;
    KEVENT Event;
    KAFFINITY ProcessAffinity;
    NTSTATUS QueryStatus;
    volatile LONG Started;
    volatile LONG Count;
} WORKER, *PWORKER;

static volatile LONG StopWorkers;
static KAFFINITY ActiveSet;
static ULONG CpuCount;
static ULONG HopperCount;
static KAFFINITY SpinnerSet;
static ULONG SpinnerCpus;
static WORKER Hoppers[HOPPERS_MAX];
static WORKER Spinners[SPINNERS_MAX];

static
ULONG
CountProcessors(
    _In_ KAFFINITY Set)
{
    ULONG Count;

    for (Count = 0; Set; Set &= Set - 1)
        Count++;
    return Count;
}

static
KAFFINITY
NthProcessor(
    _In_ KAFFINITY Set,
    _In_ ULONG N)
{
    for (N %= CountProcessors(Set); N; N--)
        Set &= Set - 1;
    return Set & (~Set + 1);
}

/*
 * Hoppers pass a token around a ring. Each one moves to another processor
 * with KeSetSystemAffinityThread before it signals the next one, and goes
 * back to its user affinity now and then. Moving off a processor that has
 * no ready thread puts the idle thread of that processor on standby, while
 * the signals ready threads onto processors that have just become idle.
 */
static KSTART_ROUTINE HopperThread;
static
VOID
NTAPI
HopperThread(
    _In_ PVOID Context)
{
    PWORKER Worker = Context;
    LARGE_INTEGER Timeout;
    ULONG Round = 0;

    Timeout.QuadPart = -10 * 1000; /* 1 ms */
    while (!StopWorkers)
    {
        KeWaitForSingleObject(&Worker->Event, Executive, KernelMode,
                              FALSE, &Timeout);
        KeSetSystemAffinityThread(NthProcessor(ActiveSet,
                                               Worker->Index + Round));
        Round++;
        InterlockedIncrement(&Worker->Count);
        KeSetEvent(&Hoppers[(Worker->Index + 1) % HopperCount].Event,
                   IO_NO_INCREMENT, FALSE);
        if ((Round & 3) == 0)
            KeRevertToUserAffinityThread();
    }

    if (Round & 3)
        KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Spinners run and sleep briefly, while the test thread moves them */
static KSTART_ROUTINE SpinnerThread;
static
VOID
NTAPI
SpinnerThread(
    _In_ PVOID Context)
{
    PWORKER Worker = Context;
    LARGE_INTEGER Timeout;
    PROCESS_BASIC_INFORMATION Info;
    ULONG i;

    /* KeSetAffinityThread accepts only processors of the thread's process */
    Worker->QueryStatus = ZwQueryInformationProcess(ZwCurrentProcess(),
                                                    ProcessBasicInformation,
                                                    &Info, sizeof(Info),
                                                    NULL);
    if (NT_SUCCESS(Worker->QueryStatus))
        Worker->ProcessAffinity = Info.AffinityMask;
    InterlockedExchange(&Worker->Started, 1);

    Timeout.QuadPart = -10 * 1000; /* 1 ms */
    while (!StopWorkers)
    {
        for (i = 0; i < 20000 && !StopWorkers; i++)
            InterlockedIncrement(&Worker->Count);
        KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
BOOLEAN
StartWorker(
    _Inout_ PWORKER Worker,
    _In_ PKSTART_ROUTINE Routine)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    Status = PsCreateSystemThread(&Worker->Handle, SYNCHRONIZE,
                                  &ObjectAttributes, NULL, NULL,
                                  Routine, Worker);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = ObReferenceObjectByHandle(Worker->Handle, SYNCHRONIZE,
                                       *PsThreadType, KernelMode,
                                       (PVOID *)&Worker->Thread, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        /* The thread is running: stop every worker, wait for this one */
        StopWorkers = 1;
        ZwWaitForSingleObject(Worker->Handle, FALSE, NULL);
        ZwClose(Worker->Handle);
        return FALSE;
    }
    return TRUE;
}

static
VOID
StopWorker(
    _Inout_ PWORKER Worker,
    _In_ PCSTR Kind)
{
    NTSTATUS Status;
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10 * 1000 * 1000 * 10; /* 10 seconds */
    Status = KeWaitForSingleObject(Worker->Thread, Executive, KernelMode,
                                   FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "%s %lu did not exit: 0x%lx, count %ld\n",
       Kind, Worker->Index, Status, Worker->Count);
    ok(Worker->Count > 0, "%s %lu never ran\n", Kind, Worker->Index);

    /* The worker runs driver code: hang rather than unload under it */
    if (Status != STATUS_SUCCESS)
    {
        DbgPrint("KeAffinity: %s %lu did not exit, waiting for it\n",
                 Kind, Worker->Index);
        KeWaitForSingleObject(Worker->Thread, Executive, KernelMode,
                              FALSE, NULL);
    }
    ObDereferenceObject(Worker->Thread);
    ZwClose(Worker->Handle);
}

/*
 * Change the affinity of running, ready and standby threads, on themselves
 * and on other threads, while threads are readied onto idle processors.
 * Every thread must keep running and exit at the end.
 */
START_TEST(KeAffinity)
{
    ULONG i, Started, SpinnerCount, Rounds;
    ULONGLONG End;
    LARGE_INTEGER Timeout;

    ActiveSet = KeQueryActiveProcessors();
    if (skip((ActiveSet & (ActiveSet - 1)) != 0,
             "Needs at least two processors\n"))
    {
        return;
    }

    CpuCount = CountProcessors(ActiveSet);
    HopperCount = min(2 * CpuCount, HOPPERS_MAX);
    SpinnerCount = min(CpuCount, SPINNERS_MAX);

    StopWorkers = 0;
    RtlZeroMemory(Hoppers, sizeof(Hoppers));
    RtlZeroMemory(Spinners, sizeof(Spinners));
    for (i = 0; i < HopperCount; i++)
    {
        Hoppers[i].Index = i;
        KeInitializeEvent(&Hoppers[i].Event, SynchronizationEvent, FALSE);
    }

    /* Run with the workers that started */
    for (Started = 0; Started < HopperCount; Started++)
    {
        if (!StartWorker(&Hoppers[Started], HopperThread))
            break;
    }
    for (i = 0; i < SpinnerCount && !StopWorkers; i++)
    {
        Spinners[i].Index = i;
        if (!StartWorker(&Spinners[i], SpinnerThread))
            break;
    }
    SpinnerCount = i;

    /* Spinners move only within their process affinity */
    SpinnerSet = ActiveSet;
    Timeout.QuadPart = -10 * 1000 * 10; /* 10 ms */
    for (i = 0; i < SpinnerCount; i++)
    {
        for (Rounds = 0; !Spinners[i].Started && Rounds < 500; Rounds++)
            KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
        ok(Spinners[i].Started, "Spinner %lu did not start\n", i);
        ok_eq_hex(Spinners[i].QueryStatus, STATUS_SUCCESS);
        SpinnerSet &= Spinners[i].ProcessAffinity;
    }
    SpinnerCpus = CountProcessors(SpinnerSet);
    skip(SpinnerCpus > 1, "Spinners can run on %lu processors only, "
         "KeSetAffinityThread is not tested\n", SpinnerCpus);

    /* Move the spinners between single processors and the whole set */
    Rounds = 0;
    Timeout.QuadPart = -10 * 1000; /* 1 ms */
    End = KeQueryInterruptTime() + STRESS_TIME;
    while (!StopWorkers && KeQueryInterruptTime() < End)
    {
        for (i = 0; i < SpinnerCount && SpinnerCpus > 1; i++)
        {
            KeSetAffinityThread(Spinners[i].Thread,
                                (Rounds & 1) ? SpinnerSet :
                                    NthProcessor(SpinnerSet, i + Rounds));
        }
        KeSetEvent(&Hoppers[Rounds % HopperCount].Event, IO_NO_INCREMENT,
                   FALSE);
        KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
        Rounds++;
    }
    ok(Rounds > 0, "No stress round ran\n");
    trace("%lu processors, %lu hoppers, %lu spinners on %lu processors, "
          "%lu rounds\n", CpuCount, Started, SpinnerCount, SpinnerCpus,
          Rounds);

    StopWorkers = 1;
    for (i = 0; i < Started; i++)
        KeSetEvent(&Hoppers[i].Event, IO_NO_INCREMENT, FALSE);
    for (i = 0; i < Started; i++)
        StopWorker(&Hoppers[i], "Hopper");
    for (i = 0; i < SpinnerCount; i++)
        StopWorker(&Spinners[i], "Spinner");
}
