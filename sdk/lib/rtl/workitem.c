/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           Work Item implementation
 * FILE:              lib/rtl/workitem.c
 * PROGRAMMER:
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ***************************************************************/

NTSTATUS
NTAPI
RtlpStartThread(IN PTHREAD_START_ROUTINE Function,
                IN PVOID Parameter,
                OUT PHANDLE ThreadHandle)
{
    /* Create a native worker thread -- used for SMSS, CSRSS, etc... */
    return RtlCreateUserThread(NtCurrentProcess(),
                               NULL,
                               TRUE,
                               0,
                               0,
                               0,
                               Function,
                               Parameter,
                               ThreadHandle,
                               NULL);
}

NTSTATUS
NTAPI
RtlpExitThread(IN NTSTATUS ExitStatus)
{
    /* Kill a native worker thread -- used for SMSS, CSRSS, etc... */
    return NtTerminateThread(NtCurrentThread(), ExitStatus);
}

PRTL_START_POOL_THREAD RtlpStartThreadFunc = RtlpStartThread;
PRTL_EXIT_POOL_THREAD RtlpExitThreadFunc = RtlpExitThread;

/*
 * Thread injection, after Windows 10 (measured with QueueUserWorkItem floods on 4 CPUs):
 * - as many workers as there are processors are created as soon as work is queued;
 * - beyond that, a thread is added only while work items wait in the queue and none was
 *   dequeued for about a timer tick, checked every ~160 ms: a pool whose callbacks all
 *   block grows by about 6 threads a second, a pool of short callbacks barely grows;
 * - a WT_EXECUTELONGFUNCTION item that finds no free worker gets a new thread at once;
 * - I/O worker threads were removed in Vista: WT_EXECUTEINIOTHREAD items run in the
 *   same pool (its threads wait alertably, so APCs of I/O they issued are delivered);
 * - WT_EXECUTEINPERSISTENTTHREAD items run one after another on a single thread that
 *   never exits.
 * Blocked callbacks keep the queue from draining, so the injection keeps adding threads
 * up to MAX_WORKERTHREADS: items that wait for other items still get a thread.
 */
#define MAX_WORKERTHREADS           0x100
#define WORKER_INJECT_INTERVAL_MS   160
#define WORKER_STALL_MS             15
#define WORKER_MONITOR_IDLE_CHECKS  16  /* checks without backlog before the monitor exits */

typedef struct _RTLP_WORKITEM
{
    LIST_ENTRY ListEntry;
    WORKERCALLBACKFUNC Function;
    PVOID Context;
    ULONG Flags;
    HANDLE TokenHandle;
} RTLP_WORKITEM, *PRTLP_WORKITEM;

static LONG ThreadPoolInitialized = 0;
static RTL_CRITICAL_SECTION ThreadPoolLock;
static HANDLE ThreadPoolCompletionPort;
static LONG ThreadPoolMinThreads;
static LONG ThreadPoolWorkerThreads;            /* worker threads started and not exited */
static LONG ThreadPoolWorkerThreadsRequests;    /* work items queued or running */
static LONG ThreadPoolWorkerThreadsLongRequests;
static LONG ThreadPoolWorkerThreadsQueued;      /* work items not dequeued yet */
static LONG ThreadPoolWorkerThreadsBusy;        /* workers running a callback */
static volatile ULONG ThreadPoolLastDequeue;    /* interrupt time in ms */
static BOOLEAN ThreadPoolMonitorRunning;

static LIST_ENTRY PersistentThreadQueue;
static HANDLE PersistentThreadEvent;
static BOOLEAN PersistentThreadStarted;

#define IsThreadPoolInitialized() (*((volatile LONG*)&ThreadPoolInitialized) == 1)

static ULONG
RtlpPoolTimeMs(VOID)
{
    ULARGE_INTEGER Time;

    do
    {
        Time.HighPart = SharedUserData->InterruptTime.High1Time;
        Time.LowPart = SharedUserData->InterruptTime.LowPart;
    } while (Time.HighPart != (ULONG)SharedUserData->InterruptTime.High2Time);

    return (ULONG)(Time.QuadPart / 10000);
}

static NTSTATUS
RtlpInitializeThreadPool(VOID)
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InitStatus;

    do
    {
        InitStatus = InterlockedCompareExchange(&ThreadPoolInitialized,
                                                 2,
                                                 0);
        if (InitStatus == 0)
        {
            /* We're the first thread to initialize the thread pool */

            InitializeListHead(&PersistentThreadQueue);
            PersistentThreadEvent = NULL;
            PersistentThreadStarted = FALSE;

            ThreadPoolWorkerThreads = 0;
            ThreadPoolWorkerThreadsRequests = 0;
            ThreadPoolWorkerThreadsLongRequests = 0;
            ThreadPoolWorkerThreadsQueued = 0;
            ThreadPoolWorkerThreadsBusy = 0;
            ThreadPoolMonitorRunning = FALSE;

            ThreadPoolMinThreads = NtCurrentPeb()->NumberOfProcessors;
            if (ThreadPoolMinThreads < 1)
                ThreadPoolMinThreads = 1;
            else if (ThreadPoolMinThreads > MAX_WORKERTHREADS)
                ThreadPoolMinThreads = MAX_WORKERTHREADS;

            /* Initialize the lock */
            Status = RtlInitializeCriticalSection(&ThreadPoolLock);
            if (!NT_SUCCESS(Status))
                goto Finish;

            /* Create the complection port */
            Status = NtCreateIoCompletion(&ThreadPoolCompletionPort,
                                          IO_COMPLETION_ALL_ACCESS,
                                          NULL,
                                          0);
            if (!NT_SUCCESS(Status))
            {
                RtlDeleteCriticalSection(&ThreadPoolLock);
                goto Finish;
            }

Finish:
            /* Initialization done. On failure, let a later call try again. */
            InterlockedExchange(&ThreadPoolInitialized,
                                NT_SUCCESS(Status) ? 1 : 0);
            break;
        }
        else if (InitStatus == 2)
        {
            LARGE_INTEGER Timeout;

            /* Another thread is currently initializing the thread pool!
               Poll after a short period of time to see if the initialization
               was completed */

            Timeout.QuadPart = -10000000LL; /* Wait for a second */
            NtDelayExecution(FALSE,
                             &Timeout);
        }
    } while (InitStatus != 1);

    return Status;
}

static NTSTATUS
RtlpGetImpersonationToken(OUT PHANDLE TokenHandle)
{
    NTSTATUS Status;

    Status = NtOpenThreadToken(NtCurrentThread(),
                               TOKEN_IMPERSONATE,
                               TRUE,
                               TokenHandle);
    if (Status == STATUS_NO_TOKEN || Status == STATUS_CANT_OPEN_ANONYMOUS)
    {
        *TokenHandle = NULL;
        Status = STATUS_SUCCESS;
    }

    return Status;
}

/* Start a pool thread. Do not wait for it to run: the caller holds ThreadPoolLock,
   and the new thread can be held up by the loader lock in its DLL notifications. */
static NTSTATUS
RtlpStartPoolThread(PTHREAD_START_ROUTINE StartRoutine)
{
    NTSTATUS Status;
    HANDLE ThreadHandle;

    Status = RtlpStartThreadFunc(StartRoutine, NULL, &ThreadHandle);
    if (NT_SUCCESS(Status))
    {
        NtResumeThread(ThreadHandle, NULL);
        NtClose(ThreadHandle);
    }

    return Status;
}

static ULONG NTAPI RtlpWorkerThreadProc(IN PVOID Parameter);
static ULONG NTAPI RtlpWorkerMonitorThreadProc(IN PVOID Parameter);

/* Called with ThreadPoolLock held */
static NTSTATUS
RtlpStartWorkerThread(VOID)
{
    NTSTATUS Status;

    if (ThreadPoolWorkerThreads >= MAX_WORKERTHREADS)
        return STATUS_TOO_MANY_THREADS;

    /* Count the thread from now on: it is a free worker even before it runs */
    InterlockedIncrement(&ThreadPoolWorkerThreads);

    Status = RtlpStartPoolThread(RtlpWorkerThreadProc);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to start a worker thread, Status 0x%lx\n", Status);
        InterlockedDecrement(&ThreadPoolWorkerThreads);
    }

    return Status;
}

/* Called with ThreadPoolLock held */
static VOID
RtlpStartWorkerMonitor(VOID)
{
    NTSTATUS Status;

    if (ThreadPoolMonitorRunning)
        return;

    ThreadPoolMonitorRunning = TRUE;
    Status = RtlpStartPoolThread(RtlpWorkerMonitorThreadProc);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to start the worker monitor, Status 0x%lx\n", Status);
        ThreadPoolMonitorRunning = FALSE;
    }
}

/* Run a work item and free it. Returns the flags it was queued with. */
static ULONG
RtlpRunWorkItem(IN PRTLP_WORKITEM QueuedItem)
{
    NTSTATUS Status;
    BOOLEAN Impersonated = FALSE;
    RTLP_WORKITEM WorkItem = *(volatile RTLP_WORKITEM *)QueuedItem;

    RtlFreeHeap(RtlGetProcessHeap(),
                0,
                QueuedItem);

    if (WorkItem.TokenHandle != NULL)
    {
        Status = NtSetInformationThread(NtCurrentThread(),
                                        ThreadImpersonationToken,
                                        &WorkItem.TokenHandle,
                                        sizeof(HANDLE));

        NtClose(WorkItem.TokenHandle);

        if (NT_SUCCESS(Status))
        {
            Impersonated = TRUE;
        }
    }

    _SEH2_TRY
    {
        DPRINT("RtlpRunWorkItem: Function: 0x%p Context: 0x%p ImpersonationToken: 0x%p\n", WorkItem.Function, WorkItem.Context, WorkItem.TokenHandle);

        /* Execute the function */
        WorkItem.Function(WorkItem.Context);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        DPRINT1("Exception 0x%x while executing work item 0x%p\n", _SEH2_GetExceptionCode(), WorkItem.Function);
    }
    _SEH2_END;

    if (Impersonated)
    {
        WorkItem.TokenHandle = NULL;
        Status = NtSetInformationThread(NtCurrentThread(),
                                        ThreadImpersonationToken,
                                        &WorkItem.TokenHandle,
                                        sizeof(HANDLE));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to revert worker thread to self!!! Status: 0x%x\n", Status);
        }
    }

    return WorkItem.Flags;
}

static VOID
NTAPI
RtlpExecuteWorkItem(IN OUT PVOID NormalContext,
                    IN OUT PVOID SystemArgument1,
                    IN OUT PVOID SystemArgument2)
{
    ULONG Flags = RtlpRunWorkItem((PRTLP_WORKITEM)SystemArgument2);

    /* update the requests counter */
    InterlockedDecrement(&ThreadPoolWorkerThreadsRequests);

    if (Flags & WT_EXECUTELONGFUNCTION)
    {
        InterlockedDecrement(&ThreadPoolWorkerThreadsLongRequests);
    }
}

/* Called with ThreadPoolLock held */
static NTSTATUS
RtlpQueueWorkerThread(IN OUT PRTLP_WORKITEM WorkItem)
{
    NTSTATUS Status;

    InterlockedIncrement(&ThreadPoolWorkerThreadsRequests);
    InterlockedIncrement(&ThreadPoolWorkerThreadsQueued);

    if (WorkItem->Flags & WT_EXECUTELONGFUNCTION)
    {
        InterlockedIncrement(&ThreadPoolWorkerThreadsLongRequests);
    }

    /* Queue an IO completion message */
    Status = NtSetIoCompletion(ThreadPoolCompletionPort,
                               RtlpExecuteWorkItem,
                               WorkItem,
                               STATUS_SUCCESS,
                               0);

    if (!NT_SUCCESS(Status))
    {
        InterlockedDecrement(&ThreadPoolWorkerThreadsRequests);
        InterlockedDecrement(&ThreadPoolWorkerThreadsQueued);

        if (WorkItem->Flags & WT_EXECUTELONGFUNCTION)
        {
            InterlockedDecrement(&ThreadPoolWorkerThreadsLongRequests);
        }
    }

    return Status;
}

static ULONG
NTAPI
RtlpPersistentThreadProc(IN PVOID Parameter)
{
    PLIST_ENTRY Entry;

    for (;;)
    {
        RtlEnterCriticalSection(&ThreadPoolLock);

        if (IsListEmpty(&PersistentThreadQueue))
        {
            RtlLeaveCriticalSection(&ThreadPoolLock);

            /* Wait alertably: the APCs of I/O started by the work items are delivered here */
            NtWaitForSingleObject(PersistentThreadEvent, TRUE, NULL);
            continue;
        }

        Entry = RemoveHeadList(&PersistentThreadQueue);
        RtlLeaveCriticalSection(&ThreadPoolLock);

        /* The items run one after another, never nested in each other */
        RtlpRunWorkItem(CONTAINING_RECORD(Entry, RTLP_WORKITEM, ListEntry));
    }

    return 0;
}

/* Called with ThreadPoolLock held */
static NTSTATUS
RtlpQueuePersistentWorkItem(IN OUT PRTLP_WORKITEM WorkItem)
{
    NTSTATUS Status;

    if (!PersistentThreadStarted)
    {
        if (PersistentThreadEvent == NULL)
        {
            Status = NtCreateEvent(&PersistentThreadEvent,
                                   EVENT_ALL_ACCESS,
                                   NULL,
                                   SynchronizationEvent,
                                   FALSE);
            if (!NT_SUCCESS(Status))
            {
                PersistentThreadEvent = NULL;
                return Status;
            }
        }

        Status = RtlpStartPoolThread(RtlpPersistentThreadProc);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to start the persistent thread, Status 0x%lx\n", Status);
            return Status;
        }

        PersistentThreadStarted = TRUE;
    }

    InsertTailList(&PersistentThreadQueue, &WorkItem->ListEntry);
    NtSetEvent(PersistentThreadEvent, NULL);

    return STATUS_SUCCESS;
}

static BOOLEAN
RtlpIsIoPending(IN HANDLE ThreadHandle  OPTIONAL)
{
    NTSTATUS Status;
    ULONG IoPending;
    BOOLEAN CreatedHandle = FALSE;
    BOOLEAN IsIoPending = TRUE;

    if (ThreadHandle == NULL)
    {
        Status = NtDuplicateObject(NtCurrentProcess(),
                                   NtCurrentThread(),
                                   NtCurrentProcess(),
                                   &ThreadHandle,
                                   0,
                                   0,
                                   DUPLICATE_SAME_ACCESS);
        if (!NT_SUCCESS(Status))
        {
            return IsIoPending;
        }

        CreatedHandle = TRUE;
    }

    Status = NtQueryInformationThread(ThreadHandle,
                                      ThreadIsIoPending,
                                      &IoPending,
                                      sizeof(IoPending),
                                      NULL);
    if (NT_SUCCESS(Status) && IoPending == 0)
    {
        IsIoPending = FALSE;
    }

    if (CreatedHandle)
    {
        NtClose(ThreadHandle);
    }

    return IsIoPending;
}

/* Adds a worker while queued work makes no progress; exits once the queue stays empty */
static ULONG
NTAPI
RtlpWorkerMonitorThreadProc(IN PVOID Parameter)
{
    LARGE_INTEGER Interval;
    IO_COMPLETION_BASIC_INFORMATION Info;
    ULONG IdleChecks = 0;
    NTSTATUS Status;

    Interval.QuadPart = -10000LL * WORKER_INJECT_INTERVAL_MS;

    for (;;)
    {
        NtDelayExecution(FALSE, &Interval);

        if (!NT_SUCCESS(RtlEnterCriticalSection(&ThreadPoolLock)))
            continue;

        /* Completions waiting in the port: work items and bound I/O completions */
        Status = NtQueryIoCompletion(ThreadPoolCompletionPort,
                                     IoCompletionBasicInformation,
                                     &Info,
                                     sizeof(Info),
                                     NULL);
        if (!NT_SUCCESS(Status))
            Info.Depth = *((volatile LONG*)&ThreadPoolWorkerThreadsQueued);

        if (Info.Depth > 0)
        {
            IdleChecks = 0;

            /* Every worker is busy and nothing was dequeued for a while: add one.
               (A worker that is not busy is starting, or the port holds it back
               because enough threads run: another thread would not help.) */
            if (ThreadPoolWorkerThreads - *((volatile LONG*)&ThreadPoolWorkerThreadsBusy) <= 0 &&
                (ULONG)(RtlpPoolTimeMs() - ThreadPoolLastDequeue) >= WORKER_STALL_MS)
            {
                RtlpStartWorkerThread();
            }
        }
        else if (++IdleChecks >= WORKER_MONITOR_IDLE_CHECKS)
        {
            ThreadPoolMonitorRunning = FALSE;
            RtlLeaveCriticalSection(&ThreadPoolLock);
            break;
        }

        RtlLeaveCriticalSection(&ThreadPoolLock);
    }

    RtlpExitThreadFunc(STATUS_SUCCESS);
    return 0;
}

static ULONG
NTAPI
RtlpWorkerThreadProc(IN PVOID Parameter)
{
    LARGE_INTEGER Timeout;
    BOOLEAN Terminate;
    FILE_IO_COMPLETION_INFORMATION Completion;
    ULONG Removed;
    ULONG TimeoutCount = 0;
    PKNORMAL_ROUTINE ApcRoutine;
    NTSTATUS Status = STATUS_SUCCESS;

    /* ThreadPoolWorkerThreads was incremented by RtlpStartWorkerThread */

    for (;;)
    {
        Timeout.QuadPart = -50000000LL; /* Wait for 5 seconds by default */

        /* Dequeue a completion message. Wait alertably, as the I/O worker threads
           did: a work item may have started I/O that completes with an APC */
        Removed = 0;
        Status = NtRemoveIoCompletionEx(ThreadPoolCompletionPort,
                                        &Completion,
                                        1,
                                        &Removed,
                                        &Timeout,
                                        TRUE);

        if (Status == STATUS_SUCCESS && Removed == 1)
        {
            TimeoutCount = 0;

            ApcRoutine = (PKNORMAL_ROUTINE)Completion.KeyContext;

            ThreadPoolLastDequeue = RtlpPoolTimeMs();
            if (ApcRoutine == (PKNORMAL_ROUTINE)RtlpExecuteWorkItem)
                InterlockedDecrement(&ThreadPoolWorkerThreadsQueued);

            /* The last free worker takes a callback: have the monitor watch the queue.
               This also covers the completions of files bound to the pool. */
            if (InterlockedIncrement(&ThreadPoolWorkerThreadsBusy) >= *((volatile LONG*)&ThreadPoolWorkerThreads) &&
                !*((volatile BOOLEAN*)&ThreadPoolMonitorRunning) &&
                NT_SUCCESS(RtlEnterCriticalSection(&ThreadPoolLock)))
            {
                RtlpStartWorkerMonitor();
                RtlLeaveCriticalSection(&ThreadPoolLock);
            }

            _SEH2_TRY
            {
                /* Call the APC routine. An I/O completion callback takes the
                   Win32 error code of the I/O as its first argument; work
                   items are queued with STATUS_SUCCESS and ignore it. */
                ApcRoutine((PVOID)(ULONG_PTR)RtlNtStatusToDosErrorNoTeb(Completion.IoStatusBlock.Status),
                           (PVOID)Completion.IoStatusBlock.Information,
                           Completion.ApcContext);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                (void)0;
            }
            _SEH2_END;

            InterlockedDecrement(&ThreadPoolWorkerThreadsBusy);
        }
        else if (Status == STATUS_USER_APC || Status == STATUS_ALERTED ||
                 Status == STATUS_SUCCESS)
        {
            /* An APC ran, wait again */
            continue;
        }
        else
        {
            Terminate = FALSE;

            if (!NT_SUCCESS(RtlEnterCriticalSection(&ThreadPoolLock)))
                continue;

            if (Status == STATUS_TIMEOUT)
            {
                /* Idle for a while: keep the minimum and the threads running long functions.
                   Do not leave while items are queued: their queuer counted this worker as
                   free and may have started neither a thread nor the monitor. */
                if (TimeoutCount++ > 2 &&
                    *((volatile LONG*)&ThreadPoolWorkerThreadsQueued) == 0 &&
                    *((volatile LONG*)&ThreadPoolWorkerThreads) - *((volatile LONG*)&ThreadPoolWorkerThreadsLongRequests) > ThreadPoolMinThreads)
                {
                    Terminate = TRUE;
                }
            }
            else
                Terminate = TRUE;

            if (Terminate)
            {
                /* Prevent termination as long as IO is pending */
                Terminate = !RtlpIsIoPending(NULL);
            }

            /* Leave the pool under the lock that guards the check above */
            if (Terminate)
                InterlockedDecrement(&ThreadPoolWorkerThreads);

            RtlLeaveCriticalSection(&ThreadPoolLock);

            if (Terminate)
            {
                Status = STATUS_SUCCESS;
                break;
            }
        }
    }

    RtlpExitThreadFunc(Status);
    return 0;

}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlQueueWorkItem(IN WORKERCALLBACKFUNC Function,
                 IN PVOID Context  OPTIONAL,
                 IN ULONG Flags)
{
    LONG FreeWorkers;
    NTSTATUS Status;
    PRTLP_WORKITEM WorkItem;

    DPRINT("RtlQueueWorkItem(0x%p, 0x%p, 0x%x)\n", Function, Context, Flags);

    /* Initialize the thread pool if not already initialized */
    if (!IsThreadPoolInitialized())
    {
        Status = RtlpInitializeThreadPool();

        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Allocate a work item */
    WorkItem = RtlAllocateHeap(RtlGetProcessHeap(),
                               0,
                               sizeof(RTLP_WORKITEM));
    if (WorkItem == NULL)
        return STATUS_NO_MEMORY;

    WorkItem->Function = Function;
    WorkItem->Context = Context;
    WorkItem->Flags = Flags;

    if (Flags & WT_TRANSFER_IMPERSONATION)
    {
        Status = RtlpGetImpersonationToken(&WorkItem->TokenHandle);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Failed to get impersonation token! Status: 0x%x\n", Status);
            goto Cleanup;
        }
    }
    else
        WorkItem->TokenHandle = NULL;

    Status = RtlEnterCriticalSection(&ThreadPoolLock);
    if (NT_SUCCESS(Status))
    {
        if (Flags & (WT_EXECUTEINPERSISTENTTHREAD | WT_EXECUTEINPERSISTENTIOTHREAD))
        {
            Status = RtlpQueuePersistentWorkItem(WorkItem);
        }
        else
        {
            /* WT_EXECUTEINIOTHREAD and WT_EXECUTEINUITHREAD items run in the worker
               pool too, as on Vista and later */

            FreeWorkers = ThreadPoolWorkerThreads - ThreadPoolWorkerThreadsBusy - ThreadPoolWorkerThreadsQueued;

            /* Start the minimum at once, and a thread for a long function if no worker is free */
            if (FreeWorkers <= 0 &&
                (ThreadPoolWorkerThreads < ThreadPoolMinThreads || (Flags & WT_EXECUTELONGFUNCTION)))
            {
                if (NT_SUCCESS(RtlpStartWorkerThread()))
                    FreeWorkers++;
            }

            if (ThreadPoolWorkerThreads == 0)
            {
                /* No worker, and none could be started */
                Status = STATUS_NO_MEMORY;
            }
            else
            {
                /* Queue a normal worker thread */
                Status = RtlpQueueWorkerThread(WorkItem);

                /* The item waits for a worker: add threads while the queue makes no progress */
                if (NT_SUCCESS(Status) && FreeWorkers <= 0)
                    RtlpStartWorkerMonitor();
            }
        }

        RtlLeaveCriticalSection(&ThreadPoolLock);
    }

    if (!NT_SUCCESS(Status))
    {
        if (WorkItem->TokenHandle != NULL)
        {
            NtClose(WorkItem->TokenHandle);
        }

Cleanup:
        RtlFreeHeap(RtlGetProcessHeap(),
                    0,
                    WorkItem);
    }

    return Status;
}

/*
 * @unimplemented
 */
NTSTATUS
NTAPI
RtlSetIoCompletionCallback(IN HANDLE FileHandle,
                           IN PIO_APC_ROUTINE Callback,
                           IN ULONG Flags)
{
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_COMPLETION_INFORMATION FileCompletionInfo;
    NTSTATUS Status;

    DPRINT("RtlSetIoCompletionCallback(0x%p, 0x%p, 0x%x)\n", FileHandle, Callback, Flags);

    /* Initialize the thread pool if not already initialized */
    if (!IsThreadPoolInitialized())
    {
        Status = RtlpInitializeThreadPool();
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* The callbacks run in the worker threads, make sure one exists. When all
       workers are busy, the monitor adds more if completions queue up. */
    Status = RtlEnterCriticalSection(&ThreadPoolLock);
    if (!NT_SUCCESS(Status))
        return Status;

    if (ThreadPoolWorkerThreads < ThreadPoolMinThreads)
        RtlpStartWorkerThread();

    if (ThreadPoolWorkerThreads == 0)
        Status = STATUS_NO_MEMORY;

    RtlLeaveCriticalSection(&ThreadPoolLock);

    if (!NT_SUCCESS(Status))
        return Status;

    FileCompletionInfo.Port = ThreadPoolCompletionPort;
    FileCompletionInfo.Key = (PVOID)Callback;

    Status = NtSetInformationFile(FileHandle,
                                  &IoStatusBlock,
                                  &FileCompletionInfo,
                                  sizeof(FileCompletionInfo),
                                  FileCompletionInformation);

    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlSetThreadPoolStartFunc(IN PRTL_START_POOL_THREAD StartPoolThread,
                          IN PRTL_EXIT_POOL_THREAD ExitPoolThread)
{
    RtlpStartThreadFunc = StartPoolThread;
    RtlpExitThreadFunc = ExitPoolThread;
    return STATUS_SUCCESS;
}
