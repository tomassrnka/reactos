/*
 * COPYRIGHT:         See COPYING in the top level directory
 * PROJECT:           ReactOS system libraries
 * PURPOSE:           Rtl user wait functions
 * FILE:              lib/rtl/wait.c
 * PROGRAMERS:
 *                    Alex Ionescu (alex@relsoft.net)
 *                    Eric Kohl
 *                    KJK::Hyperion
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

extern PRTL_START_POOL_THREAD RtlpStartThreadFunc;
extern PRTL_EXIT_POOL_THREAD RtlpExitThreadFunc;

#define WAIT_IDLE         0
#define WAIT_IN_CALLBACK  1
#define WAIT_CANCELLED    2

/* A wait thread services up to MAXIMUM_WAIT_OBJECTS - 1 waits, with one more handle, its control event,
   to be told of new and cancelled waits */
#define WAIT_THREAD_CAPACITY (MAXIMUM_WAIT_OBJECTS - 1)

struct _RTLP_WAIT_THREAD;

typedef struct _RTLP_WAIT
{
    HANDLE Object;
    volatile LONG CallbackState;
    HANDLE CancelEvent;     /* only for a wait of its own on a pool thread */
    LONG DeleteCount;
    HANDLE CompletionEvent;
    ULONG Flags;
    WAITORTIMERCALLBACKFUNC Callback;
    PVOID Context;
    ULONG Milliseconds;
    struct _RTLP_WAIT_THREAD *Owner;  /* the wait thread it is on, under WaitLock; NULL once it is off it */
    LONGLONG Deadline;                /* interrupt time, 100 ns; only the owner touches it */
} RTLP_WAIT, *PRTLP_WAIT;

typedef struct _RTLP_WAIT_THREAD
{
    LIST_ENTRY Link;                  /* WaitThreads, under WaitLock */
    HANDLE Control;                   /* auto-reset: a wait was added or cancelled */
    ULONG Count;                      /* under WaitLock */
    ULONG Next;                       /* where the next round of waiting starts, so that no wait starves */
    PRTLP_WAIT Waits[WAIT_THREAD_CAPACITY];
} RTLP_WAIT_THREAD, *PRTLP_WAIT_THREAD;

#define WAIT_RUN_REARM  0   /* the wait goes on */
#define WAIT_RUN_END    1   /* the wait is over: the caller takes it off its thread, unless cancelled */
#define WAIT_RUN_DONE   2   /* the wait is over, and off its thread and finished already */

static VOID RtlpDetachWaitByPointer(struct _RTLP_WAIT_THREAD *Thread, struct _RTLP_WAIT *Wait);

/* PRIVATE FUNCTIONS *******************************************************/

static inline PLARGE_INTEGER get_nt_timeout( PLARGE_INTEGER pTime, ULONG timeout )
{
    if (timeout == INFINITE) return NULL;
    pTime->QuadPart = (ULONGLONG)timeout * -10000;
    return pTime;
}

/* Whichever of the side that waits and RtlDeregisterWaitEx comes last signals and frees */
static VOID
RtlpFinishWait(PRTLP_WAIT Wait)
{
    HANDLE completion_event;

    if (InterlockedIncrement( &Wait->DeleteCount ) == 2 )
    {
       completion_event = Wait->CompletionEvent;
       if (completion_event) NtSetEvent( completion_event, NULL );

       if (Wait->CancelEvent) NtClose( Wait->CancelEvent );
       RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
    }
}

/* Runs the callback of a wait, on the wait thread Thread (NULL: on a pool thread). A wait that
   cannot fire again, WT_EXECUTEONLYONCE, leaves its wait thread before the callback runs, so that
   a callback that registers the next wait, on the same handle too as base::win::ObjectWatcher does,
   finds the room and does not start another thread */
static ULONG
RtlpRunWaitCallback(PRTLP_WAIT_THREAD Thread, PRTLP_WAIT Wait, BOOLEAN TimerOrWaitFired)
{
    BOOLEAN Failed = FALSE, Detached = FALSE;
    ULONG Flags = Wait->Flags;

    /* Do not start a callback once RtlDeregisterWaitEx has cancelled the wait */
    if (InterlockedCompareExchange( &Wait->CallbackState,
                                    WAIT_IN_CALLBACK,
                                    WAIT_IDLE ) != WAIT_IDLE)
        return WAIT_RUN_END;

    if (Thread && (Flags & WT_EXECUTEONLYONCE))
    {
        /* RtlDeregisterWaitEx finds the callback state, and has nothing to wake: this thread
           finishes the wait when the callback returns */
        RtlpDetachWaitByPointer(Thread, Wait);
        Detached = TRUE;
    }

    /* As on a thread of the pool, an exception in the callback ends only the wait. It must
       not skip the rest: RtlDeregisterWaitEx counts on the callback state and on the
       handshake after it, or it blocks for good and the wait leaks */
    _SEH2_TRY
    {
        Wait->Callback( Wait->Context, TimerOrWaitFired );
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        DPRINT1("Exception 0x%x in the callback of wait %p\n", _SEH2_GetExceptionCode(), Wait);
        Failed = TRUE;
    }
    _SEH2_END;
    InterlockedCompareExchange( &Wait->CallbackState, WAIT_IDLE, WAIT_IN_CALLBACK );

    if (Detached)
    {
        RtlpFinishWait( Wait );
        return WAIT_RUN_DONE;
    }

    return (Failed || (Flags & WT_EXECUTEONLYONCE)) ? WAIT_RUN_END : WAIT_RUN_REARM;
}

/* The wait of a wait that has a pool thread of its own: an I/O or persistent thread, or one that
   transfers the impersonation token, see RtlpStartWait */
static VOID
NTAPI
Wait_thread_proc(LPVOID Arg)
{
    PRTLP_WAIT Wait = (PRTLP_WAIT) Arg;
    NTSTATUS Status;
    BOOLEAN alertable = (Wait->Flags & WT_EXECUTEINIOTHREAD) != 0;
    HANDLE handles[2] = { Wait->CancelEvent, Wait->Object };
    LARGE_INTEGER timeout;

    while (TRUE)
    {
        Status = NtWaitForMultipleObjects( 2,
                                           handles,
                                           WaitAny,
                                           alertable,
                                           get_nt_timeout( &timeout, Wait->Milliseconds ) );

        if (Status == STATUS_WAIT_1 || Status == STATUS_TIMEOUT)
        {
            if (RtlpRunWaitCallback( NULL, Wait, Status == STATUS_TIMEOUT ) != WAIT_RUN_REARM)
                break;
        }
        else if (Status != STATUS_USER_APC)
            break;
    }

    RtlpFinishWait( Wait );
}


/* A wait blocks the thread it runs on until the object is signalled, possibly for good, so it does
   not take a thread of the worker pool: the pool has a fixed number of threads, and once registered
   waits hold all of them, a new wait never starts and every work item of the process starves.
   Windows does not limit the number of registered waits either. Waits run on wait threads of their
   own instead, as on Windows: a wait thread services up to MAXIMUM_WAIT_OBJECTS - 1 waits with one
   NtWaitForMultipleObjects, so that thousands of waits that wait for good (Chrome registers one for
   each child process and each channel it watches, and unregisters few of them) take a few dozen
   threads and not thousands. A new wait goes to a wait thread that has room, or to a new one. A wait
   thread without waits stays a while for the next one, then exits. The callback runs on the wait
   thread, as it did when every wait had a thread of its own: while it runs, the other waits of the
   thread wait.

   Callbacks run inline on the shared wait thread. Windows queues the callback of a wait without
   WT_EXECUTEINWAITTHREAD to a thread of the pool, so that the wait thread never runs user code of
   unknown length. Here a long callback delays every other wait on its thread, and a callback that
   blocks on something that another wait of the same thread would have to signal or notice (a
   callback that waits for a blocking RtlDeregisterWaitEx of a wait that is running its callback
   on this thread, or for work another of these callbacks does) deadlocks. Unregistering another
   wait of the thread, blocking or not, registering a wait and unregistering itself without blocking
   are fine. Dispatching the callbacks to the pool is left to a follow-up. */
static RTL_CRITICAL_SECTION WaitLock;
static volatile LONG WaitLockState;    /* 0: not yet, 1: being initialised, 2: ready */
static LIST_ENTRY WaitThreads;         /* RTLP_WAIT_THREAD, under WaitLock */
#define WAIT_THREAD_IDLE_TIMEOUT (-10 * 1000 * 10000LL) /* 10 seconds */

static NTSTATUS
RtlpEnterWaitLock(VOID)
{
    NTSTATUS Status;
    LONG State;

    while ((State = WaitLockState) != 2)
    {
        if (State == 0 && InterlockedCompareExchange(&WaitLockState, 1, 0) == 0)
        {
            Status = RtlInitializeCriticalSection(&WaitLock);
            if (!NT_SUCCESS(Status))
            {
                InterlockedExchange(&WaitLockState, 0);
                return Status;
            }
            InitializeListHead(&WaitThreads);
            InterlockedExchange(&WaitLockState, 2);
        }
        else
        {
            NtYieldExecution();
        }
    }

    return RtlEnterCriticalSection(&WaitLock);
}

static LONGLONG
RtlpWaitNow(VOID)
{
    LARGE_INTEGER Time;

    do
    {
        Time.HighPart = SharedUserData->InterruptTime.High1Time;
        Time.LowPart = SharedUserData->InterruptTime.LowPart;
    } while (Time.HighPart != SharedUserData->InterruptTime.High2Time);

    return Time.QuadPart;
}

/* Takes the wait off its thread. In WaitLock */
static VOID
RtlpDetachWait(PRTLP_WAIT_THREAD Thread, ULONG Index)
{
    Thread->Waits[Index]->Owner = NULL;
    Thread->Count--;
    Thread->Waits[Index] = Thread->Waits[Thread->Count];
    Thread->Waits[Thread->Count] = NULL;
}

static VOID
RtlpDetachWaitByPointer(PRTLP_WAIT_THREAD Thread, PRTLP_WAIT Wait)
{
    ULONG i;

    RtlEnterCriticalSection(&WaitLock);
    for (i = 0; i < Thread->Count; i++)
    {
        if (Thread->Waits[i] == Wait)
        {
            RtlpDetachWait(Thread, i);
            break;
        }
    }
    RtlLeaveCriticalSection(&WaitLock);
}

/* After a callback: ends the wait if that was its last, else starts its time again. A wait that
   RtlDeregisterWaitEx cancelled stays for the next round, which takes it off */
static VOID
RtlpWaitCallbackDone(PRTLP_WAIT_THREAD Thread, PRTLP_WAIT Wait, ULONG Result)
{
    if (Result == WAIT_RUN_DONE)
        return;

    if (Result == WAIT_RUN_REARM)
    {
        if (Wait->Milliseconds != INFINITE)
            Wait->Deadline = RtlpWaitNow() + (LONGLONG)Wait->Milliseconds * 10000;
    }
    else if (Wait->CallbackState != WAIT_CANCELLED)
    {
        RtlpDetachWaitByPointer(Thread, Wait);
        RtlpFinishWait(Wait);
    }
}

/* NtWaitForMultipleObjects failed: one of the objects is no good, and none says which. Find the waits
   that are not good and end them, without a callback, as a wait of a thread of its own ends. A wait
   whose object is signalled meanwhile runs its callback */
static VOID
RtlpWaitThreadProbe(PRTLP_WAIT_THREAD Thread, PRTLP_WAIT *Snapshot, ULONG Count)
{
    LARGE_INTEGER Zero;
    NTSTATUS Status;
    ULONG i;
    BOOLEAN Found = FALSE;

    Zero.QuadPart = 0;
    for (i = 0; i < Count; i++)
    {
        PRTLP_WAIT Wait = Snapshot[i];

        Status = NtWaitForSingleObject(Wait->Object, FALSE, &Zero);
        if (Status == STATUS_TIMEOUT)
            continue;

        Found = TRUE;
        if (Status == STATUS_WAIT_0 || Status == STATUS_ABANDONED_WAIT_0)
        {
            RtlpWaitCallbackDone(Thread, Wait, RtlpRunWaitCallback(Thread, Wait, FALSE));
        }
        else
        {
            RtlpDetachWaitByPointer(Thread, Wait);
            RtlpFinishWait(Wait);
        }
    }

    if (!Found)
    {
        /* Not an object that is the matter: do not spin */
        LARGE_INTEGER Delay;

        Delay.QuadPart = -100 * 10000LL;
        NtDelayExecution(FALSE, &Delay);
    }
}

static ULONG
NTAPI
Wait_thread_start(PVOID Arg)
{
    PRTLP_WAIT_THREAD Thread = (PRTLP_WAIT_THREAD)Arg;
    HANDLE Handles[MAXIMUM_WAIT_OBJECTS];
    PRTLP_WAIT Snapshot[WAIT_THREAD_CAPACITY];
    PRTLP_WAIT Finished[WAIT_THREAD_CAPACITY];
    LARGE_INTEGER Timeout;
    PLARGE_INTEGER pTimeout;
    LONGLONG Earliest, Now;
    NTSTATUS Status;
    ULONG Count, NumFinished, i, Index;

    for (;;)
    {
        NumFinished = 0;
        Earliest = MAXLONGLONG;

        RtlEnterCriticalSection(&WaitLock);

        /* The waits that RtlDeregisterWaitEx cancelled */
        for (i = 0; i < Thread->Count;)
        {
            if (Thread->Waits[i]->CallbackState == WAIT_CANCELLED)
            {
                Finished[NumFinished++] = Thread->Waits[i];
                RtlpDetachWait(Thread, i);
            }
            else
            {
                i++;
            }
        }

        /* The waits to wait for, starting where the last round stopped */
        Count = Thread->Count;
        if (Thread->Next >= Count)
            Thread->Next = 0;
        for (i = 0; i < Count; i++)
        {
            PRTLP_WAIT Wait = Thread->Waits[(Thread->Next + i) % Count];

            Snapshot[i] = Wait;
            Handles[i + 1] = Wait->Object;
            if (Wait->Milliseconds != INFINITE && Wait->Deadline < Earliest)
                Earliest = Wait->Deadline;
        }

        RtlLeaveCriticalSection(&WaitLock);

        for (i = 0; i < NumFinished; i++)
            RtlpFinishWait(Finished[i]);

        Handles[0] = Thread->Control;
        if (Earliest != MAXLONGLONG)
        {
            Now = RtlpWaitNow();
            if (Earliest <= Now)
            {
                /* Time is up for some waits. Do not leave it to the wait to time out: a wait whose
                   object stays signalled returns at once every round, and the time of the others
                   would never come */
                for (i = 0; i < Count; i++)
                {
                    PRTLP_WAIT Wait = Snapshot[i];

                    if (Wait->Milliseconds == INFINITE || Wait->Deadline > Now)
                        continue;

                    RtlpWaitCallbackDone(Thread, Wait, RtlpRunWaitCallback(Thread, Wait, TRUE));
                }
                continue;
            }
            Timeout.QuadPart = -(Earliest - Now);
            pTimeout = &Timeout;
        }
        else if (Count == 0)
        {
            Timeout.QuadPart = WAIT_THREAD_IDLE_TIMEOUT;
            pTimeout = &Timeout;
        }
        else
        {
            pTimeout = NULL;
        }

        Status = NtWaitForMultipleObjects(Count + 1, Handles, WaitAny, FALSE, pTimeout);

        if (Status == STATUS_WAIT_0 || Status == STATUS_USER_APC || Status == STATUS_ALERTED)
        {
            /* A wait was added or cancelled */
            continue;
        }

        if (Status == STATUS_TIMEOUT)
        {
            if (Count == 0)
            {
                /* Idle for long enough. A wait that came meanwhile keeps the thread */
                RtlEnterCriticalSection(&WaitLock);
                if (Thread->Count == 0)
                {
                    RemoveEntryList(&Thread->Link);
                    RtlLeaveCriticalSection(&WaitLock);
                    NtClose(Thread->Control);
                    RtlFreeHeap(RtlGetProcessHeap(), 0, Thread);
                    RtlpExitThreadFunc(STATUS_SUCCESS);
                    return 0;
                }
                RtlLeaveCriticalSection(&WaitLock);
                continue;
            }

            /* The waits whose time is up, the earliest of them for certain: the clock may not have
               got there yet */
            Now = RtlpWaitNow();
            if (Now < Earliest)
                Now = Earliest;
            for (i = 0; i < Count; i++)
            {
                PRTLP_WAIT Wait = Snapshot[i];

                if (Wait->Milliseconds == INFINITE || Wait->Deadline > Now)
                    continue;

                RtlpWaitCallbackDone(Thread, Wait, RtlpRunWaitCallback(Thread, Wait, TRUE));
            }
            continue;
        }

        /* A wait whose object is signalled (or abandoned) */
        Index = (ULONG)Status & 0x3F;
        if ((Status == STATUS_WAIT_0 + (LONG)Index || Status == STATUS_ABANDONED_WAIT_0 + (LONG)Index) &&
            Index >= 1 && Index <= Count)
        {
            PRTLP_WAIT Wait = Snapshot[Index - 1];

            Thread->Next += Index;
            RtlpWaitCallbackDone(Thread, Wait, RtlpRunWaitCallback(Thread, Wait, FALSE));
            continue;
        }

        /* An object that cannot be waited for */
        RtlpWaitThreadProbe(Thread, Snapshot, Count);
    }
}

/* The flags that need a thread of the pool: an I/O or persistent thread outlives the callback, so
   that I/O it started is not cancelled, and the pool hands the impersonation token over */
#define WAIT_POOL_FLAGS (WT_EXECUTEINIOTHREAD | WT_EXECUTEINPERSISTENTTHREAD | WT_TRANSFER_IMPERSONATION)

static NTSTATUS
RtlpStartWait(PRTLP_WAIT Wait, ULONG Flags)
{
    PRTLP_WAIT_THREAD Thread;
    PLIST_ENTRY Entry;
    HANDLE ThreadHandle;
    NTSTATUS Status;
    ULONG i;

    if (Flags & WAIT_POOL_FLAGS)
    {
        Status = NtCreateEvent(&Wait->CancelEvent, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = RtlQueueWorkItem(Wait_thread_proc, Wait, Flags);
        if (!NT_SUCCESS(Status))
        {
            NtClose(Wait->CancelEvent);
            Wait->CancelEvent = NULL;
        }
        return Status;
    }

    Status = RtlpEnterWaitLock();
    if (!NT_SUCCESS(Status))
        return Status;

    if (Wait->Milliseconds != INFINITE)
        Wait->Deadline = RtlpWaitNow() + (LONGLONG)Wait->Milliseconds * 10000;

    /* A thread with room, and not waiting for this handle already: NtWaitForMultipleObjects does not
       take the same handle twice */
    for (Entry = WaitThreads.Flink; Entry != &WaitThreads; Entry = Entry->Flink)
    {
        Thread = CONTAINING_RECORD(Entry, RTLP_WAIT_THREAD, Link);
        if (Thread->Count >= WAIT_THREAD_CAPACITY)
            continue;
        for (i = 0; i < Thread->Count; i++)
        {
            if (Thread->Waits[i]->Object == Wait->Object)
                break;
        }
        if (i < Thread->Count)
            continue;

        Thread->Waits[Thread->Count++] = Wait;
        Wait->Owner = Thread;
        NtSetEvent(Thread->Control, NULL);
        RtlLeaveCriticalSection(&WaitLock);
        return STATUS_SUCCESS;
    }

    Thread = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Thread));
    if (!Thread)
    {
        RtlLeaveCriticalSection(&WaitLock);
        return STATUS_NO_MEMORY;
    }

    Status = NtCreateEvent(&Thread->Control, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);
    if (NT_SUCCESS(Status))
    {
        Thread->Waits[0] = Wait;
        Thread->Count = 1;
        Wait->Owner = Thread;
        InsertTailList(&WaitThreads, &Thread->Link);

        Status = RtlpStartThreadFunc(Wait_thread_start, Thread, &ThreadHandle);
        if (NT_SUCCESS(Status))
        {
            NtResumeThread(ThreadHandle, NULL);
            NtClose(ThreadHandle);
            RtlLeaveCriticalSection(&WaitLock);
            return STATUS_SUCCESS;
        }

        RemoveEntryList(&Thread->Link);
        Wait->Owner = NULL;
        NtClose(Thread->Control);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Thread);
    RtlLeaveCriticalSection(&WaitLock);
    return Status;
}

/* FUNCTIONS ***************************************************************/


/***********************************************************************
 *              RtlRegisterWait
 *
 * Registers a wait for a handle to become signaled.
 *
 * PARAMS
 *  NewWaitObject [I] Handle to the new wait object. Use RtlDeregisterWait() to free it.
 *  Object   [I] Object to wait to become signaled.
 *  Callback [I] Callback function to execute when the wait times out or the handle is signaled.
 *  Context  [I] Context to pass to the callback function when it is executed.
 *  Milliseconds [I] Number of milliseconds to wait before timing out.
 *  Flags    [I] Flags. See notes.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 *
 * NOTES
 *  Flags can be one or more of the following:
 *|WT_EXECUTEDEFAULT - Executes the work item in a non-I/O worker thread.
 *|WT_EXECUTEINIOTHREAD - Executes the work item in an I/O worker thread.
 *|WT_EXECUTEINPERSISTENTTHREAD - Executes the work item in a thread that is persistent.
 *|WT_EXECUTELONGFUNCTION - Hints that the execution can take a long time.
 *|WT_TRANSFER_IMPERSONATION - Executes the function with the current access token.
 */
NTSTATUS
NTAPI
RtlRegisterWait(PHANDLE NewWaitObject,
                HANDLE Object,
                WAITORTIMERCALLBACKFUNC Callback,
                PVOID Context,
                ULONG Milliseconds,
                ULONG Flags)
{
    PRTLP_WAIT Wait;
    NTSTATUS Status;

    //TRACE( "(%p, %p, %p, %p, %d, 0x%x)\n", NewWaitObject, Object, Callback, Context, Milliseconds, Flags );

    Wait = RtlAllocateHeap( RtlGetProcessHeap(), 0, sizeof(RTLP_WAIT) );
    if (!Wait)
        return STATUS_NO_MEMORY;

    Wait->Object = Object;
    Wait->Callback = Callback;
    Wait->Context = Context;
    Wait->Milliseconds = Milliseconds;
    Wait->Flags = Flags;
    Wait->CallbackState = WAIT_IDLE;
    Wait->DeleteCount = 0;
    Wait->CompletionEvent = NULL;
    Wait->CancelEvent = NULL;
    Wait->Owner = NULL;
    Wait->Deadline = 0;

    Flags = Flags & (WT_EXECUTEINIOTHREAD | WT_EXECUTEINPERSISTENTTHREAD |
                     WT_EXECUTELONGFUNCTION | WT_TRANSFER_IMPERSONATION);

    /* The callback may run, and use the handle, before RtlpStartWait returns */
    *NewWaitObject = Wait;

    Status = RtlpStartWait( Wait, Flags );

    if (Status != STATUS_SUCCESS)
    {
        *NewWaitObject = NULL;
        RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
        return Status;
    }

    return Status;
}

/***********************************************************************
 *              RtlDeregisterWaitEx
 *
 * Cancels a wait operation and frees the resources associated with calling
 * RtlRegisterWait().
 *
 * PARAMS
 *  WaitObject [I] Handle to the wait object to free.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS
NTAPI
RtlDeregisterWaitEx(HANDLE WaitHandle,
                    HANDLE CompletionEvent)
{
    PRTLP_WAIT Wait = (PRTLP_WAIT) WaitHandle;
    NTSTATUS Status = STATUS_SUCCESS;
    HANDLE LocalEvent = NULL;
    BOOLEAN CallbackInProgress;

    //TRACE( "(%p)\n", WaitHandle );

    if (CompletionEvent == INVALID_HANDLE_VALUE)
    {
        Status = NtCreateEvent( &LocalEvent,
                                EVENT_ALL_ACCESS,
                                NULL,
                                NotificationEvent,
                                FALSE );

        if (Status != STATUS_SUCCESS)
            return Status;

        CompletionEvent = LocalEvent;
    }

    CallbackInProgress = (InterlockedExchange( &Wait->CallbackState,
                                               WAIT_CANCELLED ) == WAIT_IN_CALLBACK);
    if (!CallbackInProgress)
    {
        /* No callback can start any more, so the wait is complete now, as on Windows: do not leave the
           event to the wait thread, which may not even have started yet */
        if (LocalEvent)
        {
            NtClose( LocalEvent );
            LocalEvent = NULL;
        }
        else if (CompletionEvent)
        {
            NtSetEvent( CompletionEvent, NULL );
        }
        CompletionEvent = NULL;
    }

    /* Publish the event before the final DeleteCount transition can read it */
    (void)InterlockedExchangePointer( &Wait->CompletionEvent, CompletionEvent );
    if (Wait->CancelEvent)
    {
        NtSetEvent( Wait->CancelEvent, NULL );
    }
    else if (NT_SUCCESS(RtlpEnterWaitLock()))
    {
        /* Wake the wait thread, so that it takes the wait off. Once the wait is off its thread
           it has no owner, and the thread may be gone */
        if (Wait->Owner)
            NtSetEvent( Wait->Owner->Control, NULL );
        RtlLeaveCriticalSection( &WaitLock );
    }

    if (InterlockedIncrement( &Wait->DeleteCount ) == 2 )
    {
        /* The wait is off its thread: no callback can run any more */
        if (CompletionEvent) NtSetEvent( CompletionEvent, NULL );

        if (Wait->CancelEvent) NtClose( Wait->CancelEvent );
        RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
    }
    else if (LocalEvent)
    {
        /* The wait thread sets the event after its last callback returned and the wait is off it */
        NtWaitForSingleObject( LocalEvent, FALSE, NULL );
    }
    else if (CallbackInProgress)
    {
        Status = STATUS_PENDING;
    }

    if (LocalEvent) NtClose( LocalEvent );

    return Status;
}

/***********************************************************************
 *              RtlDeregisterWait
 *
 * Cancels a wait operation and frees the resources associated with calling
 * RtlRegisterWait().
 *
 * PARAMS
 *  WaitObject [I] Handle to the wait object to free.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS
NTAPI
RtlDeregisterWait(HANDLE WaitHandle)
{
    return RtlDeregisterWaitEx(WaitHandle, NULL);
}

/* EOF */
