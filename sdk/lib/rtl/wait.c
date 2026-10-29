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

/* A wait thread services up to MAXIMUM_WAIT_OBJECTS - 1 waits, with one more handle, its control event,
   to be told of new and cancelled waits */
#define WAIT_THREAD_CAPACITY (MAXIMUM_WAIT_OBJECTS - 1)

struct _RTLP_WAIT_THREAD;

typedef struct _RTLP_WAIT
{
    HANDLE Object;
    WAITORTIMERCALLBACKFUNC Callback;
    PVOID Context;
    ULONG Milliseconds;
    ULONG Flags;
    HANDLE Token;                     /* WT_TRANSFER_IMPERSONATION: the token of the thread that registered */
    /* Under WaitLock */
    LONG Refs;                        /* the registration, the wait thread it is on, its queued callback,
                                         each callback that runs */
    LONG Running;                     /* callbacks that run */
    BOOLEAN Queued;                   /* a callback is queued to the pool and has not started */
    BOOLEAN QueuedFired;              /* TimerOrWaitFired of that callback */
    BOOLEAN Cancelled;                /* RtlDeregisterWaitEx was called */
    BOOLEAN Stopped;                  /* fires no more: cancelled, or a callback raised */
    HANDLE CompletionEvent;           /* set once the wait is cancelled and no callback runs */
    struct _RTLP_WAIT_THREAD *Owner;  /* the wait thread it is on; NULL while it is off any */
    LONGLONG Deadline;                /* interrupt time, 100 ns, at which it times out */
    LONGLONG FireTime;                /* when it fired for the queued callback */
} RTLP_WAIT, *PRTLP_WAIT;

typedef struct _RTLP_WAIT_THREAD
{
    LIST_ENTRY Link;                  /* WaitThreads, under WaitLock */
    HANDLE Control;                   /* auto-reset: a wait was added or cancelled */
    ULONG Count;                      /* under WaitLock */
    ULONG Next;                       /* where the next round of waiting starts, so that no wait starves */
    PRTLP_WAIT Waits[WAIT_THREAD_CAPACITY];
} RTLP_WAIT_THREAD, *PRTLP_WAIT_THREAD;

/* The flags of RtlQueueWorkItem that apply to the callback of a wait */
#define WAIT_QUEUE_FLAGS (WT_EXECUTEINIOTHREAD | WT_EXECUTELONGFUNCTION)

/* The callbacks that the wait thread runs itself. Windows 10 runs those of WT_EXECUTEINPERSISTENTTHREAD
   on the wait thread too: it is a thread that does not exit while it has waits */
#define WAIT_INLINE_FLAGS (WT_EXECUTEINWAITTHREAD | WT_EXECUTEINPERSISTENTTHREAD)

/* A wait blocks the thread it runs on until the object is signalled, possibly for good, so it does
   not take a thread of the worker pool: the pool has a fixed number of threads, and once registered
   waits hold all of them, a new wait never starts and every work item of the process starves.
   Windows does not limit the number of registered waits either. Waits run on wait threads of their
   own instead, as on Windows: a wait thread services up to MAXIMUM_WAIT_OBJECTS - 1 waits with one
   NtWaitForMultipleObjects, so that thousands of waits that wait for good (Chrome registers one for
   each child process and each channel it watches, and unregisters few of them) take a few dozen
   threads and not thousands. A new wait goes to a wait thread that has room, or to a new one. A wait
   thread without waits stays a while for the next one, then exits.

   The wait thread runs WT_EXECUTEINWAITTHREAD and WT_EXECUTEINPERSISTENTTHREAD callbacks itself;
   while one runs, the other waits of the thread wait. The callback of any other wait goes to the pool,
   with the flags of the wait (I/O thread, long function, impersonation), so that a long callback does not delay
   the other waits of its thread, and a callback may block on something that another wait of the same
   thread signals or notices. As on Windows (10 22H2, measured), such a wait goes on while its
   callback runs: an object that is signalled again, or stays signalled, or a timeout that comes again,
   fires it again, and its callbacks may run at the same time. Windows re-arms the wait as soon as it
   queued the callback, so that callbacks pile up in the queue while the pool is busy; here a wait
   whose callback is queued is off its thread until the callback starts, so that a wait has one
   callback in the queue at most, and goes back on a wait thread then, with its time counted from when
   it fired. A callback that has not started when the wait is cancelled does not start, as on Windows,
   and RtlDeregisterWaitEx does not wait for it: it waits for, or reports pending, only the callbacks
   that run. */
static RTL_CRITICAL_SECTION WaitLock;
static volatile LONG WaitLockState;    /* 0: not yet, 1: being initialised, 2: ready */
static LIST_ENTRY WaitThreads;         /* RTLP_WAIT_THREAD, under WaitLock */
#define WAIT_THREAD_IDLE_TIMEOUT (-10 * 1000 * 10000LL) /* 10 seconds */

/* PRIVATE FUNCTIONS *******************************************************/

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

/* Drops a reference; the last one frees the wait. In WaitLock */
static VOID
RtlpReleaseWait(PRTLP_WAIT Wait)
{
    if (--Wait->Refs == 0)
    {
        if (Wait->Token) NtClose(Wait->Token);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Wait);
    }
}

/* A callback returned. The last callback of a cancelled wait completes RtlDeregisterWaitEx. In WaitLock */
static VOID
RtlpCallbackFinished(PRTLP_WAIT Wait)
{
    Wait->Running--;
    if (Wait->Cancelled && Wait->Running == 0 && Wait->CompletionEvent)
    {
        NtSetEvent(Wait->CompletionEvent, NULL);
        Wait->CompletionEvent = NULL;
    }
    RtlpReleaseWait(Wait);
}

/* Calls the callback, with the token of the thread that registered the wait for
   WT_TRANSFER_IMPERSONATION. An exception in the callback ends the wait, and only the wait: it must
   not skip the rest, which RtlDeregisterWaitEx counts on, or that blocks for good and the wait leaks
   (Windows ends the process). FALSE if the callback raised */
static BOOLEAN
RtlpCallWaitCallback(PRTLP_WAIT Wait, BOOLEAN TimerOrWaitFired)
{
    BOOLEAN Failed = FALSE, Impersonated = FALSE;
    HANDLE Token;

    if (Wait->Token &&
        NT_SUCCESS(NtSetInformationThread(NtCurrentThread(), ThreadImpersonationToken,
                                          &Wait->Token, sizeof(HANDLE))))
    {
        Impersonated = TRUE;
    }

    _SEH2_TRY
    {
        Wait->Callback(Wait->Context, TimerOrWaitFired);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        DPRINT1("Exception 0x%x in the callback of wait %p\n", _SEH2_GetExceptionCode(), Wait);
        Failed = TRUE;
    }
    _SEH2_END;

    if (Impersonated)
    {
        Token = NULL;
        NtSetInformationThread(NtCurrentThread(), ThreadImpersonationToken, &Token, sizeof(HANDLE));
    }

    return !Failed;
}

/* Takes the wait off its thread; the reference of the thread goes to the caller. In WaitLock */
static VOID
RtlpDetachWait(PRTLP_WAIT_THREAD Thread, ULONG Index)
{
    Thread->Waits[Index]->Owner = NULL;
    Thread->Count--;
    Thread->Waits[Index] = Thread->Waits[Thread->Count];
    Thread->Waits[Thread->Count] = NULL;
}

/* In WaitLock */
static VOID
RtlpDetachWaitByPointer(PRTLP_WAIT_THREAD Thread, PRTLP_WAIT Wait)
{
    ULONG i;

    for (i = 0; i < Thread->Count; i++)
    {
        if (Thread->Waits[i] == Wait)
        {
            RtlpDetachWait(Thread, i);
            break;
        }
    }
}

static ULONG NTAPI Wait_thread_start(PVOID Arg);

/* Puts the wait on a wait thread that has room, or on a new one, which takes a reference. Only the
   wait thread a wait is on takes it off. In WaitLock */
static NTSTATUS
RtlpAttachWait(PRTLP_WAIT Wait)
{
    PRTLP_WAIT_THREAD Thread;
    PLIST_ENTRY Entry;
    HANDLE ThreadHandle;
    NTSTATUS Status;
    ULONG i;

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
        Wait->Refs++;
        NtSetEvent(Thread->Control, NULL);
        return STATUS_SUCCESS;
    }

    Thread = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Thread));
    if (!Thread)
        return STATUS_NO_MEMORY;

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
            Wait->Refs++;
            return STATUS_SUCCESS;
        }

        RemoveEntryList(&Thread->Link);
        Wait->Owner = NULL;
        NtClose(Thread->Control);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Thread);
    return Status;
}

/* The queued callback of a wait, on a thread of the pool. The wait is off its wait thread until the
   callback starts; then a wait that goes on goes back on a wait thread, while the callback runs */
static VOID
NTAPI
RtlpWaitWorkItem(PVOID Arg)
{
    PRTLP_WAIT Wait = (PRTLP_WAIT)Arg;
    BOOLEAN Fired, Succeeded;

    RtlEnterCriticalSection(&WaitLock);
    Wait->Queued = FALSE;
    if (Wait->Stopped)
    {
        /* Cancelled before it started: it does not start */
        RtlpReleaseWait(Wait);
        RtlLeaveCriticalSection(&WaitLock);
        return;
    }

    /* The reference of the queue goes to the callback */
    Fired = Wait->QueuedFired;
    Wait->Running++;
    if (!(Wait->Flags & WT_EXECUTEONLYONCE))
    {
        if (Wait->Milliseconds != INFINITE)
            Wait->Deadline = Wait->FireTime + (LONGLONG)Wait->Milliseconds * 10000;
        if (!NT_SUCCESS(RtlpAttachWait(Wait)))
        {
            DPRINT1("Wait %p cannot wait again, it ends\n", Wait);
            Wait->Stopped = TRUE;
        }
    }
    RtlLeaveCriticalSection(&WaitLock);

    Succeeded = RtlpCallWaitCallback(Wait, Fired);

    RtlEnterCriticalSection(&WaitLock);
    if (!Succeeded)
    {
        /* The thread it is on takes it off */
        Wait->Stopped = TRUE;
        if (Wait->Owner)
            NtSetEvent(Wait->Owner->Control, NULL);
    }
    RtlpCallbackFinished(Wait);
    RtlLeaveCriticalSection(&WaitLock);
}

/* The object of a wait of the wait thread Thread is signalled, or its time is up. A callback for
   WT_EXECUTEINWAITTHREAD or WT_EXECUTEINPERSISTENTTHREAD runs here; any other is queued to the pool, and the wait leaves the thread
   until it starts. A wait that cannot fire again, WT_EXECUTEONLYONCE, leaves the thread before its
   callback runs, so that a callback that registers the next wait, on the same handle too as
   base::win::ObjectWatcher does, finds the room and does not start another thread */
static VOID
RtlpFireWait(PRTLP_WAIT_THREAD Thread, PRTLP_WAIT Wait, BOOLEAN TimerOrWaitFired)
{
    NTSTATUS Status;
    BOOLEAN Succeeded;

    RtlEnterCriticalSection(&WaitLock);
    if (Wait->Stopped)
    {
        /* The next round takes it off */
        RtlLeaveCriticalSection(&WaitLock);
        return;
    }

    if (Wait->Flags & WAIT_INLINE_FLAGS)
    {
        Wait->Running++;
        Wait->Refs++;
        if (Wait->Flags & WT_EXECUTEONLYONCE)
        {
            RtlpDetachWaitByPointer(Thread, Wait);
            RtlpReleaseWait(Wait);
        }
        RtlLeaveCriticalSection(&WaitLock);

        Succeeded = RtlpCallWaitCallback(Wait, TimerOrWaitFired);

        RtlEnterCriticalSection(&WaitLock);
        if (!Succeeded)
            Wait->Stopped = TRUE;
        else if (Wait->Milliseconds != INFINITE)
            Wait->Deadline = RtlpWaitNow() + (LONGLONG)Wait->Milliseconds * 10000;
        RtlpCallbackFinished(Wait);
        RtlLeaveCriticalSection(&WaitLock);
        return;
    }

    /* The reference of the thread goes to the queue */
    Wait->Queued = TRUE;
    Wait->QueuedFired = TimerOrWaitFired;
    Wait->FireTime = RtlpWaitNow();
    RtlpDetachWaitByPointer(Thread, Wait);
    RtlLeaveCriticalSection(&WaitLock);

    Status = RtlQueueWorkItem(RtlpWaitWorkItem, Wait, Wait->Flags & WAIT_QUEUE_FLAGS);
    if (!NT_SUCCESS(Status))
    {
        /* Better late than never: here */
        DPRINT1("Cannot queue the callback of wait %p: 0x%lx\n", Wait, Status);
        RtlpWaitWorkItem(Wait);
    }
}

/* NtWaitForMultipleObjects failed: one of the objects is no good, and none says which. Find the waits
   that are not good and end them, without a callback, as a wait of a thread of its own ends. A wait
   whose object is signalled meanwhile fires */
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
            RtlpFireWait(Thread, Wait, FALSE);
        }
        else
        {
            RtlEnterCriticalSection(&WaitLock);
            Wait->Stopped = TRUE;
            RtlpDetachWaitByPointer(Thread, Wait);
            RtlpReleaseWait(Wait);
            RtlLeaveCriticalSection(&WaitLock);
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
    LARGE_INTEGER Timeout;
    PLARGE_INTEGER pTimeout;
    LONGLONG Earliest, Now;
    NTSTATUS Status;
    ULONG Count, i, Index;

    for (;;)
    {
        Earliest = MAXLONGLONG;

        RtlEnterCriticalSection(&WaitLock);

        /* The waits that fire no more: cancelled, or a callback raised */
        for (i = 0; i < Thread->Count;)
        {
            PRTLP_WAIT Wait = Thread->Waits[i];

            if (Wait->Stopped)
            {
                RtlpDetachWait(Thread, i);
                RtlpReleaseWait(Wait);
            }
            else
            {
                i++;
            }
        }

        /* The waits to wait for, starting where the last round stopped. They stay on this thread, and
           so valid, until this thread takes them off */
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

                    RtlpFireWait(Thread, Wait, TRUE);
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

                RtlpFireWait(Thread, Wait, TRUE);
            }
            continue;
        }

        /* A wait whose object is signalled (or abandoned) */
        Index = (ULONG)Status & 0x3F;
        if ((Status == STATUS_WAIT_0 + (LONG)Index || Status == STATUS_ABANDONED_WAIT_0 + (LONG)Index) &&
            Index >= 1 && Index <= Count)
        {
            Thread->Next += Index;
            RtlpFireWait(Thread, Snapshot[Index - 1], FALSE);
            continue;
        }

        /* An object that cannot be waited for */
        RtlpWaitThreadProbe(Thread, Snapshot, Count);
    }
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
 *|WT_EXECUTEDEFAULT - Executes the callback in a non-I/O worker thread.
 *|WT_EXECUTEINIOTHREAD - Executes the callback in an I/O worker thread.
 *|WT_EXECUTEINPERSISTENTTHREAD - Executes the callback in a thread that is persistent.
 *|WT_EXECUTEINWAITTHREAD - Executes the callback in the wait thread itself.
 *|WT_EXECUTELONGFUNCTION - Hints that the execution can take a long time.
 *|WT_EXECUTEONLYONCE - The wait fires once only.
 *|WT_TRANSFER_IMPERSONATION - Executes the callback with the access token of the caller.
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

    Wait = RtlAllocateHeap( RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RTLP_WAIT) );
    if (!Wait)
        return STATUS_NO_MEMORY;

    Wait->Object = Object;
    Wait->Callback = Callback;
    Wait->Context = Context;
    Wait->Milliseconds = Milliseconds;
    Wait->Flags = Flags;
    Wait->Refs = 1;   /* the registration, until RtlDeregisterWaitEx */

    if (Flags & WT_TRANSFER_IMPERSONATION)
    {
        /* The callback runs with the token of this thread, as a work item queued from it does */
        Status = NtOpenThreadToken( NtCurrentThread(), TOKEN_IMPERSONATE, TRUE, &Wait->Token );
        if (Status == STATUS_NO_TOKEN || Status == STATUS_CANT_OPEN_ANONYMOUS)
        {
            Wait->Token = NULL;
        }
        else if (!NT_SUCCESS(Status))
        {
            RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
            return Status;
        }
    }

    Status = RtlpEnterWaitLock();
    if (NT_SUCCESS(Status))
    {
        if (Milliseconds != INFINITE)
            Wait->Deadline = RtlpWaitNow() + (LONGLONG)Milliseconds * 10000;

        /* The callback may run, and use the handle, before RtlRegisterWait returns */
        *NewWaitObject = Wait;
        Status = RtlpAttachWait( Wait );
        RtlLeaveCriticalSection( &WaitLock );
    }

    if (!NT_SUCCESS(Status))
    {
        *NewWaitObject = NULL;
        if (Wait->Token) NtClose( Wait->Token );
        RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
        return Status;
    }

    return STATUS_SUCCESS;
}

/***********************************************************************
 *              RtlDeregisterWaitEx
 *
 * Cancels a wait operation and frees the resources associated with calling
 * RtlRegisterWait().
 *
 * PARAMS
 *  WaitObject [I] Handle to the wait object to free.
 *  CompletionEvent [I] NULL: do not wait for the callbacks that run; INVALID_HANDLE_VALUE: wait
 *                      for them; an event: set it once they returned.
 *
 * RETURNS
 *  Success: STATUS_SUCCESS; STATUS_PENDING if callbacks run and CompletionEvent is not
 *           INVALID_HANDLE_VALUE.
 *  Failure: Any NTSTATUS code.
 */
NTSTATUS
NTAPI
RtlDeregisterWaitEx(HANDLE WaitHandle,
                    HANDLE CompletionEvent)
{
    PRTLP_WAIT Wait = (PRTLP_WAIT) WaitHandle;
    NTSTATUS Status;
    HANDLE LocalEvent = NULL;
    BOOLEAN Pending = FALSE;

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

    /* A wait exists, so the lock is ready */
    RtlEnterCriticalSection( &WaitLock );

    /* No callback starts from here on, the queued one either */
    Wait->Cancelled = TRUE;
    Wait->Stopped = TRUE;

    /* Wake the wait thread, so that it takes the wait off. Once the wait is off its thread it has
       no owner, and the thread may be gone */
    if (Wait->Owner)
        NtSetEvent( Wait->Owner->Control, NULL );

    if (Wait->Running)
    {
        /* The last callback to return sets the event */
        Wait->CompletionEvent = CompletionEvent;
        Pending = TRUE;
    }
    else if (CompletionEvent && !LocalEvent)
    {
        /* Complete now, as on Windows: no callback runs */
        NtSetEvent( CompletionEvent, NULL );
    }

    RtlpReleaseWait( Wait );
    RtlLeaveCriticalSection( &WaitLock );

    Status = STATUS_SUCCESS;
    if (Pending)
    {
        if (LocalEvent)
            NtWaitForSingleObject( LocalEvent, FALSE, NULL );
        else
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
