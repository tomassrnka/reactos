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
 * wait for the lock. If they spin with interrupts disabled the system hangs.
 *
 * A hang must not take the whole kmtest run with it, so the test is built
 * around a watchdog:
 *
 *  - Processor 0 runs the lock holder, on a thread of its own.
 *  - Processors 1 .. N-2 run one worker each.
 *  - The test thread itself stays on processor N-1, which nothing else uses,
 *    and watches the others. Processor N-1 takes the IPIs, so it stays alive
 *    when the rest of the system is wedged. This is why three processors are
 *    the minimum.
 *  - KeIpiGenericCall is a rendezvous of every processor, the watchdog's
 *    included: against a broken kernel it would wedge the whole system and no
 *    code could run to notice. So the holder first checks, with the lock held
 *    and nothing sent yet, that every worker keeps taking interrupts while it
 *    waits for the lock (its PRCB interrupt count advances). A worker that
 *    spins with interrupts disabled would not take the IPI: that is reported
 *    as the failure, and the holder releases the lock and ends without
 *    sending anything, so the system recovers. The IPIs are only sent after
 *    that check passed.
 *  - The watchdog does not use a timer or a dispatcher wait with a timeout.
 *    Timers expire in a DPC, and a DPC cannot run on a processor stuck at
 *    DISPATCH_LEVEL (the holder, waiting for the IPI). It polls with
 *    KeStallExecutionProcessor and counts the time itself.
 *  - When the holder or a worker makes no progress for about WATCHDOG_MS, the
 *    test reports a failure and then bugchecks with KMT_IPI_DEADLOCK_BUGCHECK.
 *    A wedged system cannot return to the test runner anyway, and a bugcheck
 *    gives a CI run a verdict (and a dump) in place of a silent hang until the
 *    outer timeout.
 */

#define ENTRY_COUNT 64
#define MAX_WORKERS 31
#define RUN_SECONDS 3
#define KEIPI_TAG 'iIeK'

#define WATCHDOG_MS 60000
#define THREAD_EXIT_WAIT_MS 5000

/* Interrupt check: hold the lock, wait for the workers to block on it, then see them take interrupts */
#define PROBE_ROUNDS 5
#define PROBE_SETTLE_MS 25
#define PROBE_MEASURE_MS 50

/* "KEIP". Parameters: holder mode, phase, (workers << 16) | started, holder iterations */
#define KMT_IPI_DEADLOCK_BUGCHECK 0x4B454950

#define PHASE_STARTUP 1
#define PHASE_HOLDER 2
#define PHASE_STOP 3

typedef struct _IPI_ENTRY
{
    LIST_ENTRY ListEntry;
    SINGLE_LIST_ENTRY SingleEntry;
} IPI_ENTRY, *PIPI_ENTRY;

typedef struct _IPI_WORKER
{
    HANDLE Handle;
    ULONG Processor;
    ULONG Iterations;
    ULONG Lost;
    ULONG InterruptsOff;
    ULONG Starved;
    PKPRCB Prcb;
    volatile LONG Exited;
} IPI_WORKER, *PIPI_WORKER;

typedef enum _HOLDER_MODE
{
    HolderIpiGenericCall,
    HolderPoolFree,
} HOLDER_MODE;

static KSPIN_LOCK Lock;
static LIST_ENTRY ListHead;
static SINGLE_LIST_ENTRY SingleHead;
static ULONG Counter;
static volatile LONG Stop;
static volatile LONG Started;
static IPI_ENTRY Entries[ENTRY_COUNT];
static IPI_WORKER Workers[MAX_WORKERS];
static volatile LONG IpiCount;

static volatile LONG HolderExited;
static volatile ULONG HolderIterations;
static volatile LONG HolderAllocFailed;
static volatile LONG ProbeFailed;
static ULONG ProbeWorkers;

/* Milliseconds left for the watchdog */
static ULONG WatchdogLeft;

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
    Worker->Prcb = KeGetCurrentPrcb();
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
    InterlockedExchange(&Worker->Exited, 1);
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

static
ULONG
ReadInterruptCount(
    _In_ PKPRCB Prcb)
{
    return *(volatile ULONG*)&Prcb->InterruptCount;
}

/*
 * With the lock held the workers pile up on it. If they wait with interrupts
 * enabled their processors keep counting interrupts (the clock at least). One
 * that counts none cannot take an IPI either. Returns FALSE then.
 */
static
BOOLEAN
HolderProbe(VOID)
{
    ULONG Round, i, Ms;
    ULONG Before[MAX_WORKERS];
    BOOLEAN Ok = TRUE;
    KIRQL OldIrql;

    for (Round = 0; Round < PROBE_ROUNDS; Round++)
    {
        KeAcquireSpinLock(&Lock, &OldIrql);

        for (Ms = 0; Ms < PROBE_SETTLE_MS; Ms++)
            KeStallExecutionProcessor(1000);

        for (i = 0; i < ProbeWorkers; i++)
            Before[i] = ReadInterruptCount(Workers[i].Prcb);

        for (Ms = 0; Ms < PROBE_MEASURE_MS; Ms++)
            KeStallExecutionProcessor(1000);

        for (i = 0; i < ProbeWorkers; i++)
        {
            if (ReadInterruptCount(Workers[i].Prcb) == Before[i])
            {
                Workers[i].Starved++;
                Ok = FALSE;
            }
        }

        KeReleaseSpinLock(&Lock, OldIrql);

        if (!Ok)
            break;

        /* Let the workers run for a moment */
        KeStallExecutionProcessor(2000);
    }

    return Ok;
}

static
VOID
NTAPI
HolderThread(
    _In_ PVOID Context)
{
    HOLDER_MODE Mode = (HOLDER_MODE)(ULONG_PTR)Context;
    ULONGLONG Start, Now, LastReport;
    KIRQL OldIrql;
    PVOID Block;

    KeSetSystemAffinityThread(AFFINITY_MASK(0));

    if (!HolderProbe())
    {
        /* Sending an IPI now would hang the system */
        InterlockedExchange(&ProbeFailed, 1);
        KeRevertToUserAffinityThread();
        InterlockedExchange(&HolderExited, 1);
        PsTerminateSystemThread(STATUS_SUCCESS);
    }

    Start = LastReport = KeQueryInterruptTime();
    do
    {
        Block = NULL;
        if (Mode == HolderPoolFree)
        {
            Block = ExAllocatePoolWithTag(NonPagedPool, 64, KEIPI_TAG);
            if (!Block)
            {
                InterlockedExchange(&HolderAllocFailed, 1);
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
        HolderIterations++;

        /* Say we are alive: a hang leaves the debug output quiet */
        Now = KeQueryInterruptTime();
        if (Now - LastReport >= 10ULL * 1000 * 1000)
        {
            DbgPrint("ExInterlockedIpi: holder: %lu iterations\n", HolderIterations);
            LastReport = Now;
        }
    } while (Now - Start < RUN_SECONDS * 10ULL * 1000 * 1000);

    KeRevertToUserAffinityThread();
    InterlockedExchange(&HolderExited, 1);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*
 * Fail, then bugcheck. Nothing here can return to the test runner: the
 * processors that hold the system up are the ones that are stuck.
 */
static
VOID
Deadlock(
    _In_ HOLDER_MODE Mode,
    _In_ ULONG Phase,
    _In_ ULONG WorkerCount,
    _In_ PCSTR Name)
{
    ULONG StartedNow = (ULONG)Started;
    ULONG Iterations = HolderIterations;

    ok(FALSE, "ExInterlocked/IPI deadlock: %s: no progress in phase %lu, %lu of %lu workers started, %lu holder iterations\n",
       Name, Phase, StartedNow, WorkerCount, Iterations);

    /* The test log never reaches the runner, so say it on the debug port too */
    DbgPrint("ExInterlockedIpi: ExInterlocked/IPI deadlock: %s: no progress in phase %lu, %lu of %lu workers started, %lu holder iterations; bugchecking\n",
             Name, Phase, StartedNow, WorkerCount, Iterations);

    KeBugCheckEx(KMT_IPI_DEADLOCK_BUGCHECK,
                 Mode,
                 Phase,
                 (WorkerCount << 16) | (StartedNow & 0xFFFF),
                 Iterations);
}

/* Wait for *Flag == Target within what is left of the watchdog's time */
static
BOOLEAN
WaitFlag(
    _In_ volatile LONG *Flag,
    _In_ LONG Target)
{
    while (*Flag != Target)
    {
        if (WatchdogLeft == 0)
            return FALSE;
        KeStallExecutionProcessor(1000);
        WatchdogLeft--;
    }
    return TRUE;
}

/* The thread has set its exit flag, so it is on its way out */
static
VOID
WaitThreadExit(
    _In_ HANDLE Handle,
    _In_ HOLDER_MODE Mode,
    _In_ ULONG Phase,
    _In_ ULONG WorkerCount,
    _In_ PCSTR Name)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Timeout.QuadPart = -10LL * 1000 * THREAD_EXIT_WAIT_MS;
    Status = ZwWaitForSingleObject(Handle, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
        Deadlock(Mode, Phase, WorkerCount, Name);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

/* Stop the first WorkerCount workers, wait for them and close their handles */
static
VOID
StopWorkers(
    _In_ HOLDER_MODE Mode,
    _In_ ULONG WorkerCount,
    _In_ PCSTR Name)
{
    ULONG i;

    InterlockedExchange(&Stop, 1);
    for (i = 0; i < WorkerCount; i++)
    {
        if (!WaitFlag(&Workers[i].Exited, 1))
            Deadlock(Mode, PHASE_STOP, WorkerCount, Name);
        WaitThreadExit(Workers[i].Handle, Mode, PHASE_STOP, WorkerCount, Name);
        ZwClose(Workers[i].Handle);
        Workers[i].Handle = NULL;
    }
}

/*
 * The pool free only exercises an IPI when the tag is in SpecialPoolTags (or
 * PoolTag selects it). Rather than parse the registry the way MM does,
 * allocate with the tag and see whether the block came from special pool:
 * data at the very end of a page (overruns) or the very start (underruns),
 * which no ordinary pool block does, with an inaccessible guard page next to it.
 */
static
BOOLEAN
IsSpecialPoolActive(VOID)
{
    PVOID Block;
    ULONG_PTR Start;
    ULONG_PTR End;
    BOOLEAN Special;

    Block = ExAllocatePoolWithTag(NonPagedPool, 64, KEIPI_TAG);
    if (!Block)
        return FALSE;

    Start = (ULONG_PTR)Block;
    End = Start + 64;
    if ((Start & (PAGE_SIZE - 1)) == 0)
        Special = !MmIsAddressValid((PVOID)(Start - 1));
    else if ((End & (PAGE_SIZE - 1)) == 0)
        Special = !MmIsAddressValid((PVOID)End);
    else
        Special = FALSE;

    ExFreePoolWithTag(Block, KEIPI_TAG);
    return Special;
}

static
VOID
TestHolder(
    _In_ HOLDER_MODE Mode,
    _In_ PCSTR Name)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    ULONG Processors = KeNumberProcessors;
    ULONG WorkerCount, i, Count, Iterations, Total = 0;
    HANDLE HolderHandle;
    PLIST_ENTRY Entry;
    PSINGLE_LIST_ENTRY Single;

    /* Processor 0 holds, the last one watches, one worker on each in between */
    WorkerCount = min(Processors - 2, MAX_WORKERS);

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
    HolderExited = 0;
    HolderIterations = 0;
    HolderAllocFailed = 0;
    ProbeFailed = 0;
    ProbeWorkers = WorkerCount;
    WatchdogLeft = WATCHDOG_MS;
    RtlZeroMemory(Workers, sizeof(Workers));

    KeSetSystemAffinityThread(AFFINITY_MASK(Processors - 1));

    for (i = 0; i < WorkerCount; i++)
    {
        Workers[i].Processor = i + 1;
        InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = PsCreateSystemThread(&Workers[i].Handle, SYNCHRONIZE, &ObjectAttributes,
                                      NULL, NULL, WorkerThread, &Workers[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
        {
            /* The workers that did start must not outlive the test */
            StopWorkers(Mode, i, Name);
            goto Done;
        }
    }

    if (!WaitFlag(&Started, (LONG)WorkerCount))
        Deadlock(Mode, PHASE_STARTUP, WorkerCount, Name);

    DbgPrint("ExInterlockedIpi: %s: %lu workers\n", Name, WorkerCount);

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&HolderHandle, SYNCHRONIZE, &ObjectAttributes,
                                  NULL, NULL, HolderThread, (PVOID)(ULONG_PTR)Mode);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        StopWorkers(Mode, WorkerCount, Name);
        goto Done;
    }

    /* This is where a hang shows */
    if (!WaitFlag(&HolderExited, 1))
        Deadlock(Mode, PHASE_HOLDER, WorkerCount, Name);
    WaitThreadExit(HolderHandle, Mode, PHASE_HOLDER, WorkerCount, Name);
    ZwClose(HolderHandle);

    StopWorkers(Mode, WorkerCount, Name);

    Iterations = HolderIterations;
    ok(!HolderAllocFailed, "%s: allocation failed\n", Name);

    for (i = 0; i < WorkerCount; i++)
    {
        ok(Workers[i].Starved == 0, "ExInterlocked/IPI deadlock: %s: worker %lu on processor %lu took no interrupt while it waited for a lock held at DISPATCH_LEVEL, so it would not take an IPI\n",
           Name, i, Workers[i].Processor);
        ok(Workers[i].Iterations != 0, "%s: worker %lu made no progress\n", Name, i);
        ok_eq_ulong(Workers[i].Lost, 0UL);
        ok_eq_ulong(Workers[i].InterruptsOff, 0UL);
        Total += Workers[i].Iterations;
    }

    /* Nothing got lost or corrupted on the way */
    ok_eq_ulong(Counter, Total);
    for (Count = 0, Entry = ListHead.Flink; Entry != &ListHead && Count <= ENTRY_COUNT; Entry = Entry->Flink)
        Count++;
    ok_eq_ulong(Count, (ULONG)ENTRY_COUNT);
    for (Count = 0, Single = SingleHead.Next; Single && Count <= ENTRY_COUNT; Single = Single->Next)
        Count++;
    ok_eq_ulong(Count, (ULONG)ENTRY_COUNT);
    ok(Iterations != 0 || ProbeFailed, "%s: no holder iterations\n", Name);
    if (Mode == HolderIpiGenericCall)
        ok_eq_long(IpiCount, (LONG)(Iterations * Processors));

    trace("%s: %lu holder iterations, %lu worker iterations on %lu workers\n",
          Name, Iterations, Total, WorkerCount);
    DbgPrint("ExInterlockedIpi: %s: done, %lu holder iterations, %lu worker iterations\n",
             Name, Iterations, Total);

Done:
    KeRevertToUserAffinityThread();
}

START_TEST(ExInterlockedIpi)
{
    if (skip(KeNumberProcessors > 2, "Needs three processors: one for the lock holder, one for a worker and one to watch them\n"))
        return;

    /* Frees flush the TLB on every processor when the tag is in SpecialPoolTags */
    if (!skip(IsSpecialPoolActive(), "Pool free mode needs the tag 'KeIi' in SpecialPoolTags (or PoolTag), it would not send an IPI\n"))
        TestHolder(HolderPoolFree, "pool free");
    TestHolder(HolderIpiGenericCall, "KeIpiGenericCall");
}
