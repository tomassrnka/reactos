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

typedef struct _RTLP_WAIT
{
    HANDLE Object;
    LONG CallbackState;
    HANDLE CancelEvent;
    LONG DeleteCount;
    HANDLE CompletionEvent;
    ULONG Flags;
    WAITORTIMERCALLBACKFUNC Callback;
    PVOID Context;
    ULONG Milliseconds;
} RTLP_WAIT, *PRTLP_WAIT;

/* PRIVATE FUNCTIONS *******************************************************/

static inline PLARGE_INTEGER get_nt_timeout( PLARGE_INTEGER pTime, ULONG timeout )
{
    if (timeout == INFINITE) return NULL;
    pTime->QuadPart = (ULONGLONG)timeout * -10000;
    return pTime;
}

static VOID
NTAPI
Wait_thread_proc(LPVOID Arg)
{
    PRTLP_WAIT Wait = (PRTLP_WAIT) Arg;
    NTSTATUS Status;
    BOOLEAN alertable = (Wait->Flags & WT_EXECUTEINIOTHREAD) != 0;
    HANDLE handles[2] = { Wait->CancelEvent, Wait->Object };
    LARGE_INTEGER timeout;
    HANDLE completion_event;

//    TRACE("\n");

    while (TRUE)
    {
        Status = NtWaitForMultipleObjects( 2,
                                           handles,
                                           WaitAny,
                                           alertable,
                                           get_nt_timeout( &timeout, Wait->Milliseconds ) );

        if (Status == STATUS_WAIT_1 || Status == STATUS_TIMEOUT)
        {
            BOOLEAN TimerOrWaitFired;
            BOOLEAN Failed = FALSE;

            if (Status == STATUS_WAIT_1)
            {
   //             TRACE( "object %p signaled, calling callback %p with context %p\n",
   //                 Wait->Object, Wait->Callback,
   //                 Wait->Context );
                TimerOrWaitFired = FALSE;
            }
            else
            {
    //            TRACE( "wait for object %p timed out, calling callback %p with context %p\n",
    //                Wait->Object, Wait->Callback,
    //                Wait->Context );
                TimerOrWaitFired = TRUE;
            }
            /* Do not start a callback once RtlDeregisterWaitEx has cancelled the wait */
            if (InterlockedCompareExchange( &Wait->CallbackState,
                                            WAIT_IN_CALLBACK,
                                            WAIT_IDLE ) != WAIT_IDLE)
                break;

            /* As on a thread of the pool, an exception in the callback ends only the wait. It must
               not skip the rest: RtlDeregisterWaitEx counts on the callback state and on the
               handshake below, or it blocks for good and the wait leaks */
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

            if (Failed || (Wait->Flags & WT_EXECUTEONLYONCE))
                break;
        }
        else if (Status != STATUS_USER_APC)
            break;
    }

    /* Whichever of this thread and RtlDeregisterWaitEx comes last signals */
    if (InterlockedIncrement( &Wait->DeleteCount ) == 2 )
    {
       completion_event = Wait->CompletionEvent;
       if (completion_event) NtSetEvent( completion_event, NULL );

       NtClose( Wait->CancelEvent );
       RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
    }
}


/* A wait blocks the thread it runs on until the object is signalled, possibly for good, so it does
   not take a thread of the worker pool: the pool has a fixed number of threads, and once registered
   waits hold all of them, a new wait never starts and every work item of the process starves.
   Windows does not limit the number of registered waits either. Waits run on wait threads of their
   own instead, which are not limited; a wait thread whose wait is gone stays a while for the next
   one, so that a new wait reuses it, as on Windows. */
static HANDLE WaitThreadPort;          /* hands waits to idle wait threads */
static volatile LONG WaitThreadsIdle;  /* idle wait threads not yet promised a wait */
#define WAIT_THREAD_IDLE_TIMEOUT (-10 * 1000 * 10000LL) /* 10 seconds */

/* Takes one of the idle wait threads, if there is one */
static BOOLEAN
RtlpTakeIdleWaitThread(VOID)
{
    LONG Idle;

    do
    {
        Idle = WaitThreadsIdle;
        if (Idle == 0)
            return FALSE;
    } while (InterlockedCompareExchange(&WaitThreadsIdle, Idle - 1, Idle) != Idle);

    return TRUE;
}

static ULONG
NTAPI
Wait_thread_start(PVOID Arg)
{
    PRTLP_WAIT Wait = (PRTLP_WAIT)Arg;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER Timeout;
    PVOID Unused;
    NTSTATUS Status;

    for (;;)
    {
        Wait_thread_proc(Wait);

        InterlockedIncrement(&WaitThreadsIdle);
        Timeout.QuadPart = WAIT_THREAD_IDLE_TIMEOUT;
        for (;;)
        {
            Status = NtRemoveIoCompletion(WaitThreadPort, (PVOID*)&Wait, &Unused, &IoStatusBlock, &Timeout);
            if (Status == STATUS_SUCCESS)
                break;

            /* Leave, unless every idle thread is promised a wait already: then one of the waiting threads,
               maybe this one, gets it any moment, so go back to waiting with a timeout */
            if (RtlpTakeIdleWaitThread())
                goto Exit;
        }
    }

Exit:
    RtlpExitThreadFunc(STATUS_SUCCESS);
    return 0;
}

/* The flags that need a thread of the pool: an I/O or persistent thread outlives the callback, so
   that I/O it started is not cancelled, and the pool hands the impersonation token over */
#define WAIT_POOL_FLAGS (WT_EXECUTEINIOTHREAD | WT_EXECUTEINPERSISTENTTHREAD | WT_TRANSFER_IMPERSONATION)

static NTSTATUS
RtlpStartWait(PRTLP_WAIT Wait, ULONG Flags)
{
    HANDLE Port, Thread;
    NTSTATUS Status;

    if (Flags & WAIT_POOL_FLAGS)
        return RtlQueueWorkItem(Wait_thread_proc, Wait, Flags);

    if (!WaitThreadPort)
    {
        /* No limit on how many wait threads run at once */
        Status = NtCreateIoCompletion(&Port, IO_COMPLETION_ALL_ACCESS, NULL, MAXLONG);
        if (!NT_SUCCESS(Status))
            return Status;
        if (InterlockedCompareExchangePointer(&WaitThreadPort, Port, NULL) != NULL)
            NtClose(Port);
    }

    if (RtlpTakeIdleWaitThread())
    {
        Status = NtSetIoCompletion(WaitThreadPort, Wait, NULL, STATUS_SUCCESS, 0);
        if (NT_SUCCESS(Status))
            return STATUS_SUCCESS;
        /* Give the idle thread back and start a new one */
        InterlockedIncrement(&WaitThreadsIdle);
    }

    Status = RtlpStartThreadFunc(Wait_thread_start, Wait, &Thread);
    if (!NT_SUCCESS(Status))
        return Status;

    NtResumeThread(Thread, NULL);
    NtClose(Thread);
    return STATUS_SUCCESS;
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

    Status = NtCreateEvent( &Wait->CancelEvent,
                             EVENT_ALL_ACCESS,
                             NULL,
                             NotificationEvent,
                             FALSE );

    if (Status != STATUS_SUCCESS)
    {
        RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
        return Status;
    }

    Flags = Flags & (WT_EXECUTEINIOTHREAD | WT_EXECUTEINPERSISTENTTHREAD |
                     WT_EXECUTELONGFUNCTION | WT_TRANSFER_IMPERSONATION);

    /* The callback may run, and use the handle, before RtlpStartWait returns */
    *NewWaitObject = Wait;

    Status = RtlpStartWait( Wait, Flags );

    if (Status != STATUS_SUCCESS)
    {
        *NewWaitObject = NULL;
        NtClose( Wait->CancelEvent );
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
    NtSetEvent( Wait->CancelEvent, NULL );

    if (InterlockedIncrement( &Wait->DeleteCount ) == 2 )
    {
        /* The wait thread has exited: no callback can run any more */
        if (CompletionEvent) NtSetEvent( CompletionEvent, NULL );

        NtClose( Wait->CancelEvent );
        RtlFreeHeap( RtlGetProcessHeap(), 0, Wait );
    }
    else if (LocalEvent)
    {
        /* The wait thread sets the event after its last callback returned */
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
