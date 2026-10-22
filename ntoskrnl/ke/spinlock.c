/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/spinlock.c
 * PURPOSE:         Spinlock and Queued Spinlock Support
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define LQ_WAIT     1
#define LQ_OWN      2

/* PRIVATE FUNCTIONS *********************************************************/

/*
 * T72: kill-switch for routing the global dispatcher lock through the MCS
 * queued spinlock (below) instead of the default test-and-set path.
 *
 * Default OFF (plain TTAS == the pre-T72 behaviour) so that landing this is a
 * pure no-op by default; the dispatcher lock is the hottest lock in the kernel
 * and the MCS path is flipped on (via the MCSDISPATCH boot option, or by poking
 * this global from the debugger) only after soak. It is *latched at boot*: it
 * selects a mutual-exclusion protocol, so it must never be toggled at runtime
 * (doing so could let one CPU use the MCS queue while another uses TTAS on the
 * same lock -- i.e. lose mutual exclusion). See Ki*DispatcherLock in ke_x.h.
 */
BOOLEAN KiDispatcherLockMcsEnabled = FALSE;

#ifdef CONFIG_SMP
/*
 * Tail of the dispatcher-lock MCS wait queue (NULL == lock is free). This is a
 * dedicated word, distinct from KiDispatcherLock (which the OFF/TTAS path uses),
 * so the two protocols never share state -- which is why the kill-switch is only
 * safe to set at boot.
 */
PKSPIN_LOCK_QUEUE volatile KiDispatcherMcsTail = NULL;

/*
 * T72: repaired MCS (Mellor-Crummey/Scott) queued spinlock core.
 *
 * This replaces the long-disabled (#if 0) scaffold, which was broken (it tried
 * to pack owner/wait state into the low bits of the shared lock word and could
 * not express a multi-waiter FIFO queue). The implementation below is the
 * textbook MCS lock and gives bounded, FIFO, cache-friendly hand-off -- exactly
 * what de-livelocks the dispatcher lock at high core counts.
 *
 * Model:
 *   - Tail points at the last queued node, or NULL when the lock is free.
 *   - Each CPU owns one node (its PRCB LockQueue[] entry). Node->Next links the
 *     FIFO queue; Node->Lock is repurposed as that node's "still waiting" flag
 *     (LQ_WAIT while queued, 0 once the predecessor grants the lock).
 *
 * ABA-safety is by construction: a CPU's node is enqueued at most once at a time
 * (the owner must release before it can acquire again), so the release-side
 * CompareExchange(Tail, Node, NULL) can never observe Node reappear at the tail
 * underneath it.
 *
 * Memory ordering: x86/amd64 are TSO and every Interlocked* here carries a full
 * barrier. In particular the hand-off (InterlockedExchangePointer on the
 * successor's flag) publishes the releaser's critical-section writes before the
 * successor sees the grant, so no wakeup is ever lost or reordered ahead of the
 * data it protects.
 */
VOID
FASTCALL
KiAcquireQueuedSpinLockMcs(IN PKSPIN_LOCK_QUEUE volatile *Tail,
                           IN PKSPIN_LOCK_QUEUE Node)
{
    PKSPIN_LOCK_QUEUE Prev;

    /* We have no successor yet, and we start out waiting. Publish this before
       we become reachable via the Tail exchange below. */
    Node->Next = NULL;
    Node->Lock = (PKSPIN_LOCK)LQ_WAIT;

    /* Atomically append ourselves; Prev is whoever was tail before us. */
    Prev = (PKSPIN_LOCK_QUEUE)
           InterlockedExchangePointer((PVOID volatile *)Tail, Node);
    if (Prev == NULL)
    {
        /* Queue was empty -- the lock is ours immediately. */
        return;
    }

    /* There is a predecessor. Link ourselves as its successor, then spin on
       our own flag until it hands the lock to us (clears LQ_WAIT). Spinning on
       a node-local flag is what keeps this fair and off the global cache line. */
    InterlockedExchangePointer((PVOID volatile *)&Prev->Next, Node);
    while (*(PKSPIN_LOCK volatile *)&Node->Lock == (PKSPIN_LOCK)LQ_WAIT)
    {
        YieldProcessor();
    }
}

VOID
FASTCALL
KiReleaseQueuedSpinLockMcs(IN PKSPIN_LOCK_QUEUE volatile *Tail,
                           IN PKSPIN_LOCK_QUEUE Node)
{
    PKSPIN_LOCK_QUEUE Succ;

    /* Is there a successor queued behind us? */
    Succ = *(PKSPIN_LOCK_QUEUE volatile *)&Node->Next;
    if (Succ == NULL)
    {
        /* None visible. Try to free the lock by clearing the tail. If we were
           genuinely last in line this succeeds and we are done. */
        if ((PKSPIN_LOCK_QUEUE)
            InterlockedCompareExchangePointer((PVOID volatile *)Tail,
                                              NULL,
                                              Node) == Node)
        {
            return;
        }

        /* The CompareExchange failed: a successor won the Tail exchange but has
           not published Prev->Next (== our Next) yet. Wait for it -- bounded,
           it is mid-acquire. This is what prevents a lost wakeup. */
        do
        {
            YieldProcessor();
            Succ = *(PKSPIN_LOCK_QUEUE volatile *)&Node->Next;
        } while (Succ == NULL);
    }

    /* Hand the lock to the successor in strict FIFO order by clearing its wait
       flag. The interlocked store also carries the release barrier for our
       critical section. */
    InterlockedExchangePointer((PVOID volatile *)&Succ->Lock, NULL);
}
#endif /* CONFIG_SMP */

//
// HACK: non-dispatcher queued locks -- and the dispatcher lock when the MCS
// kill-switch (KiDispatcherLockMcsEnabled) is off -- fall back to acting like
// plain test-and-set spinlocks.
//

_IRQL_requires_min_(DISPATCH_LEVEL)
_Acquires_nonreentrant_lock_(*LockHandle->Lock)
_Acquires_exclusive_lock_(*LockHandle->Lock)
VOID
FASTCALL
KeAcquireQueuedSpinLockAtDpcLevel(_Inout_ PKSPIN_LOCK_QUEUE LockHandle)
{
#if defined(CONFIG_SMP) || DBG
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)LockHandle->Lock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }
#endif

    /* Do the inlined function */
    KxAcquireSpinLock(LockHandle->Lock);
}

_IRQL_requires_min_(DISPATCH_LEVEL)
_Releases_nonreentrant_lock_(*LockHandle->Lock)
_Releases_exclusive_lock_(*LockHandle->Lock)
VOID
FASTCALL
KeReleaseQueuedSpinLockFromDpcLevel(_Inout_ PKSPIN_LOCK_QUEUE LockHandle)
{
#if defined(CONFIG_SMP) || DBG
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)LockHandle->Lock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }
#endif

    /* Do the inlined function */
    KxReleaseSpinLock(LockHandle->Lock);
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
KIRQL
NTAPI
KeAcquireInterruptSpinLock(IN PKINTERRUPT Interrupt)
{
    KIRQL OldIrql;

    /* Raise IRQL */
    KeRaiseIrql(Interrupt->SynchronizeIrql, &OldIrql);

    /* Acquire spinlock on MP */
    KeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);
    return OldIrql;
}

/*
 * @implemented
 */
VOID
NTAPI
KeReleaseInterruptSpinLock(IN PKINTERRUPT Interrupt,
                           IN KIRQL OldIrql)
{
    /* Release lock on MP */
    KeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);

    /* Lower IRQL */
    KeLowerIrql(OldIrql);
}

/*
 * @implemented
 */
VOID
NTAPI
_KeInitializeSpinLock(IN PKSPIN_LOCK SpinLock)
{
    /* Clear it */
    *SpinLock = 0;
}

/*
 * @implemented
 */
#undef KeAcquireSpinLockAtDpcLevel
VOID
NTAPI
KeAcquireSpinLockAtDpcLevel(IN PKSPIN_LOCK SpinLock)
{
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)SpinLock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }

    /* Do the inlined function */
    KxAcquireSpinLock(SpinLock);
}

/*
 * @implemented
 */
#undef KeReleaseSpinLockFromDpcLevel
VOID
NTAPI
KeReleaseSpinLockFromDpcLevel(IN PKSPIN_LOCK SpinLock)
{
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)SpinLock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }

    /* Do the inlined function */
    KxReleaseSpinLock(SpinLock);
}

/*
 * @implemented
 */
VOID
FASTCALL
KefAcquireSpinLockAtDpcLevel(IN PKSPIN_LOCK SpinLock)
{
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)SpinLock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }

    /* Do the inlined function */
    KxAcquireSpinLock(SpinLock);
}

/*
 * @implemented
 */
VOID
FASTCALL
KefReleaseSpinLockFromDpcLevel(IN PKSPIN_LOCK SpinLock)
{
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)SpinLock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }

    /* Do the inlined function */
    KxReleaseSpinLock(SpinLock);
}

/*
 * @implemented
 */
VOID
FASTCALL
KiAcquireSpinLock(IN PKSPIN_LOCK SpinLock)
{
    /* Do the inlined function */
    KxAcquireSpinLock(SpinLock);
}

/*
 * @implemented
 */
VOID
FASTCALL
KiReleaseSpinLock(IN PKSPIN_LOCK SpinLock)
{
    /* Do the inlined function */
    KxReleaseSpinLock(SpinLock);
}

/*
 * @implemented
 */
BOOLEAN
FASTCALL
KeTryToAcquireSpinLockAtDpcLevel(IN OUT PKSPIN_LOCK SpinLock)
{
#if DBG
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)SpinLock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }

    /* Make sure that we don't own the lock already */
    if (((KSPIN_LOCK)KeGetCurrentThread() | 1) == *SpinLock)
    {
        /* We do, bugcheck! */
        KeBugCheckEx(SPIN_LOCK_ALREADY_OWNED, (ULONG_PTR)SpinLock, 0, 0, 0);
    }
#endif

#ifdef CONFIG_SMP
    /* Check if it's already acquired */
    if (!(*SpinLock))
    {
        /* Try to acquire it */
        if (InterlockedBitTestAndSet((PLONG)SpinLock, 0))
        {
            /* Someone else acquired it */
            return FALSE;
        }
    }
    else
    {
        /* It was already acquired */
        return FALSE;
    }
#endif

#if DBG
    /* On debug builds, we OR in the KTHREAD */
    *SpinLock = (ULONG_PTR)KeGetCurrentThread() | 1;
#endif

    /* All is well, return TRUE */
    return TRUE;
}

/*
 * @implemented
 */
VOID
FASTCALL
KeAcquireInStackQueuedSpinLockAtDpcLevel(IN PKSPIN_LOCK SpinLock,
                                         IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    /* Set it up properly */
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;
#ifdef CONFIG_SMP
#if 0
    KeAcquireQueuedSpinLockAtDpcLevel(LockHandle->LockQueue.Next);
#else
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)LockHandle->LockQueue.Lock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }
#endif
#endif

    /* Acquire the lock */
    KxAcquireSpinLock(LockHandle->LockQueue.Lock); // HACK
}

/*
 * @implemented
 */
VOID
FASTCALL
KeReleaseInStackQueuedSpinLockFromDpcLevel(IN PKLOCK_QUEUE_HANDLE LockHandle)
{
#ifdef CONFIG_SMP
#if 0
    /* Call the internal function */
    KeReleaseQueuedSpinLockFromDpcLevel(LockHandle->LockQueue.Next);
#else
    /* Make sure we are at DPC or above! */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL)
    {
        /* We aren't -- bugcheck */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     (ULONG_PTR)LockHandle->LockQueue.Lock,
                     KeGetCurrentIrql(),
                     0,
                     0);
    }
#endif
#endif

    /* Release the lock */
    KxReleaseSpinLock(LockHandle->LockQueue.Lock); // HACK
}

/*
 * @unimplemented
 */
KIRQL
FASTCALL
KeAcquireSpinLockForDpc(IN PKSPIN_LOCK SpinLock)
{
    UNIMPLEMENTED;
    return 0;
}

/*
 * @unimplemented
 */
VOID
FASTCALL
KeReleaseSpinLockForDpc(IN PKSPIN_LOCK SpinLock,
                        IN KIRQL OldIrql)
{
    UNIMPLEMENTED;
}

/*
 * @implemented
 */
VOID
FASTCALL
KeAcquireInStackQueuedSpinLockForDpc(IN PKSPIN_LOCK SpinLock,
                                     IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    LockHandle->OldIrql = KeGetCurrentIrql();
    if (LockHandle->OldIrql >= DISPATCH_LEVEL)
        KeAcquireInStackQueuedSpinLockAtDpcLevel(SpinLock, LockHandle);
    else
        KeAcquireInStackQueuedSpinLock(SpinLock, LockHandle);
}

/*
 * @implemented
 */
VOID
FASTCALL
KeReleaseInStackQueuedSpinLockForDpc(IN PKLOCK_QUEUE_HANDLE LockHandle)
{
    if (LockHandle->OldIrql >= DISPATCH_LEVEL)
        KeReleaseInStackQueuedSpinLockFromDpcLevel(LockHandle);
    else
        KeReleaseInStackQueuedSpinLock(LockHandle);

}

/*
 * @implemented
 */
BOOLEAN
FASTCALL
KeTestSpinLock(IN PKSPIN_LOCK SpinLock)
{
    /* Test this spinlock */
    if (*SpinLock)
    {
        /* Spinlock is busy, yield execution */
        YieldProcessor();

        /* Return busy flag */
        return FALSE;
    }

    /* Spinlock appears to be free */
    return TRUE;
}

#ifdef _M_IX86
VOID
NTAPI
Kii386SpinOnSpinLock(PKSPIN_LOCK SpinLock, ULONG Flags)
{
    // FIXME: Handle flags
    UNREFERENCED_PARAMETER(Flags);

    /* Spin until it's unlocked */
    while (*(volatile KSPIN_LOCK *)SpinLock & 1)
    {
        // FIXME: Check for timeout

        /* Yield and keep looping */
        YieldProcessor();
    }
}
#endif
