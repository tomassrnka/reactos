/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     RtlWaitOnAddress, RtlWakeAddressSingle and RtlWakeAddressAll
 * COPYRIGHT:   Copyright 1996-2003 Marcus Meissner, Alexandre Julliard,
 *                                  Juergen Schmied and Eric Pouech (Wine ntdll/sync.c)
 *
 * The hash buckets, the compare under the bucket lock and the FIFO wake order are
 * taken from Wine's ntdll/sync.c. Wine parks waiters with NtWaitForAlertByThreadId,
 * which ReactOS does not have, so each waiter parks on a private event instead.
 * The keyed event is not used: when a timeout races with a release, the ReactOS
 * kernel returns STATUS_TIMEOUT to the waiter although the release was delivered,
 * and the waker could then not tell whether its wake was consumed.
 */

/* INCLUDES *****************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define WAIT_ADDRESS_BUCKETS 256
#define WAIT_ADDRESS_WAKE_BATCH 64
#define WAIT_ADDRESS_EVENT_CACHE 64

/* A waiter's entry lives on its stack. Address is cleared, under the bucket lock,
 * by the waker that takes the entry off the queue: that waker then owes the waiter
 * exactly one NtSetEvent on the entry's event, and the waiter does not give the
 * event back before it has seen that signal. */
typedef struct _WAIT_ADDRESS_ENTRY
{
    LIST_ENTRY ListEntry;
    const volatile VOID *Address;
    HANDLE Event;
    ULONGLONG Ticket;
} WAIT_ADDRESS_ENTRY, *PWAIT_ADDRESS_ENTRY;

typedef struct _WAIT_ADDRESS_BUCKET
{
    LIST_ENTRY Queue;
    volatile LONG Lock;
    ULONGLONG NextTicket;
} WAIT_ADDRESS_BUCKET, *PWAIT_ADDRESS_BUCKET;

static WAIT_ADDRESS_BUCKET RtlpWaitAddressBuckets[WAIT_ADDRESS_BUCKETS];

/* Auto-reset events that are not signaled, ready for the next waiter. */
static HANDLE RtlpWaitAddressEvents[WAIT_ADDRESS_EVENT_CACHE];
static ULONG RtlpWaitAddressEventCount;
static volatile LONG RtlpWaitAddressEventLock;

/* INTERNAL FUNCTIONS *******************************************************/

static
PWAIT_ADDRESS_BUCKET
RtlpGetWaitAddressBucket(
    _In_ const volatile VOID *Address)
{
    return &RtlpWaitAddressBuckets[((ULONG_PTR)Address >> 4) % WAIT_ADDRESS_BUCKETS];
}

/* The locks are held for a few list operations. A holder that was preempted must be
 * able to run again, so a waiter spins briefly, then yields, then sleeps. */
static
VOID
RtlpAcquireWaitAddressLock(
    _Inout_ volatile LONG *Lock)
{
    LARGE_INTEGER Delay;
    ULONG Spins = 0;

    while (InterlockedCompareExchange(Lock, 1, 0) != 0)
    {
        do
        {
            if (Spins < 128)
            {
                Spins++;
                YieldProcessor();
            }
            else if (Spins < 192)
            {
                Spins++;
                NtYieldExecution();
            }
            else
            {
                Delay.QuadPart = -10 * 1000;
                NtDelayExecution(FALSE, &Delay);
            }
        } while (*Lock != 0);
    }
}

static
VOID
RtlpReleaseWaitAddressLock(
    _Inout_ volatile LONG *Lock)
{
    InterlockedExchange(Lock, 0);
}

static
VOID
RtlpLockWaitAddressBucket(
    _Inout_ PWAIT_ADDRESS_BUCKET Bucket)
{
    RtlpAcquireWaitAddressLock(&Bucket->Lock);

    /* The buckets start zeroed, so the list is set up by its first user. */
    if (Bucket->Queue.Flink == NULL)
        InitializeListHead(&Bucket->Queue);
}

static
VOID
RtlpUnlockWaitAddressBucket(
    _Inout_ PWAIT_ADDRESS_BUCKET Bucket)
{
    RtlpReleaseWaitAddressLock(&Bucket->Lock);
}

static
NTSTATUS
RtlpTakeWaitAddressEvent(
    _Out_ PHANDLE Event)
{
    HANDLE Handle = NULL;

    RtlpAcquireWaitAddressLock(&RtlpWaitAddressEventLock);
    if (RtlpWaitAddressEventCount != 0)
        Handle = RtlpWaitAddressEvents[--RtlpWaitAddressEventCount];
    RtlpReleaseWaitAddressLock(&RtlpWaitAddressEventLock);

    if (Handle == NULL)
    {
        NTSTATUS Status = NtCreateEvent(&Handle, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *Event = Handle;
    return STATUS_SUCCESS;
}

/* The event must not be signaled. */
static
VOID
RtlpGiveBackWaitAddressEvent(
    _In_ HANDLE Event)
{
    BOOLEAN Cached = FALSE;

    RtlpAcquireWaitAddressLock(&RtlpWaitAddressEventLock);
    if (RtlpWaitAddressEventCount < WAIT_ADDRESS_EVENT_CACHE)
    {
        RtlpWaitAddressEvents[RtlpWaitAddressEventCount++] = Event;
        Cached = TRUE;
    }
    RtlpReleaseWaitAddressLock(&RtlpWaitAddressEventLock);

    if (!Cached)
        NtClose(Event);
}

static
ULONGLONG
RtlpLoadWaitAddressValue(
    _In_ const volatile VOID *Address,
    _In_ SIZE_T Size)
{
    switch (Size)
    {
        case 1:
            return *(const volatile UCHAR *)Address;
        case 2:
            return *(const volatile USHORT *)Address;
        case 4:
            return *(const volatile ULONG *)Address;
        default:
            return *(const volatile ULONGLONG *)Address;
    }
}

/* EXPORTED FUNCTIONS ********************************************************/

NTSTATUS
NTAPI
RtlWaitOnAddress(
    _In_ const volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    PWAIT_ADDRESS_BUCKET Bucket;
    WAIT_ADDRESS_ENTRY Entry;
    ULONGLONG CompareValue;
    BOOLEAN Equal = FALSE;
    NTSTATUS Status;

    if (AddressSize != 1 && AddressSize != 2 && AddressSize != 4 && AddressSize != 8)
        return STATUS_INVALID_PARAMETER;

    /* Read the caller's value first, so a bad pointer faults before any lock is held. */
    CompareValue = RtlpLoadWaitAddressValue(CompareAddress, AddressSize);

    Status = RtlpTakeWaitAddressEvent(&Entry.Event);
    if (!NT_SUCCESS(Status))
        return Status;

    Bucket = RtlpGetWaitAddressBucket(Address);
    Entry.Address = Address;

    RtlpLockWaitAddressBucket(Bucket);

    /* Compare under the lock: a waker changes the value before it takes the lock, so
     * either this sees the new value or the entry is queued before the waker looks. */
    _SEH2_TRY
    {
        Equal = (RtlpLoadWaitAddressValue(Address, AddressSize) == CompareValue);
    }
    _SEH2_FINALLY
    {
        if (_SEH2_AbnormalTermination())
        {
            RtlpUnlockWaitAddressBucket(Bucket);
            RtlpGiveBackWaitAddressEvent(Entry.Event);
        }
    }
    _SEH2_END;

    if (!Equal)
    {
        RtlpUnlockWaitAddressBucket(Bucket);
        RtlpGiveBackWaitAddressEvent(Entry.Event);
        return STATUS_SUCCESS;
    }

    Entry.Ticket = Bucket->NextTicket++;
    InsertTailList(&Bucket->Queue, &Entry.ListEntry);
    RtlpUnlockWaitAddressBucket(Bucket);

    Status = NtWaitForSingleObject(Entry.Event, FALSE, Timeout);
    if (Status == STATUS_SUCCESS)
    {
        /* The wake was delivered and the auto-reset event is clear again. */
        RtlpGiveBackWaitAddressEvent(Entry.Event);
        return STATUS_SUCCESS;
    }

    /* Timed out, or the wait failed. Unless a waker has already taken the entry,
     * remove it and report why. */
    RtlpLockWaitAddressBucket(Bucket);
    if (Entry.Address != NULL)
    {
        RemoveEntryList(&Entry.ListEntry);
        RtlpUnlockWaitAddressBucket(Bucket);
        RtlpGiveBackWaitAddressEvent(Entry.Event);
        return Status;
    }
    RtlpUnlockWaitAddressBucket(Bucket);

    /* A waker took the entry and will signal the event, or already has. The signal
     * must be consumed here: the waker uses the event handle, and a signaled event
     * must not go back to the cache. */
    NtWaitForSingleObject(Entry.Event, FALSE, NULL);
    RtlpGiveBackWaitAddressEvent(Entry.Event);
    return STATUS_SUCCESS;
}

VOID
NTAPI
RtlWakeAddressAll(
    _In_ const volatile VOID *Address)
{
    PWAIT_ADDRESS_BUCKET Bucket;
    HANDLE Events[WAIT_ADDRESS_WAKE_BATCH];
    PLIST_ENTRY Link, Next;
    ULONGLONG Limit = 0;
    ULONG Count, i;
    BOOLEAN First = TRUE;

    if (Address == NULL)
        return;

    Bucket = RtlpGetWaitAddressBucket(Address);

    do
    {
        Count = 0;

        RtlpLockWaitAddressBucket(Bucket);

        /* Only the waiters queued when the call began are woken: one that went back to
         * waiting after its wake must not keep this call going. */
        if (First)
        {
            Limit = Bucket->NextTicket;
            First = FALSE;
        }

        for (Link = Bucket->Queue.Flink; Link != &Bucket->Queue; Link = Next)
        {
            PWAIT_ADDRESS_ENTRY Entry = CONTAINING_RECORD(Link, WAIT_ADDRESS_ENTRY, ListEntry);

            Next = Link->Flink;
            if (Entry->Address == Address && Entry->Ticket < Limit)
            {
                Entry->Address = NULL;
                RemoveEntryList(Link);
                Events[Count++] = Entry->Event;
                if (Count == RTL_NUMBER_OF(Events))
                    break;
            }
        }
        RtlpUnlockWaitAddressBucket(Bucket);

        /* Only the handles are used from here on: a waiter that was taken off the queue
         * does not give its event back before this signal, but its entry may be gone. */
        for (i = 0; i < Count; i++)
            NtSetEvent(Events[i], NULL);
    } while (Count == RTL_NUMBER_OF(Events));
}

VOID
NTAPI
RtlWakeAddressSingle(
    _In_ const volatile VOID *Address)
{
    PWAIT_ADDRESS_BUCKET Bucket;
    HANDLE Event = NULL;
    PLIST_ENTRY Link;

    if (Address == NULL)
        return;

    Bucket = RtlpGetWaitAddressBucket(Address);

    RtlpLockWaitAddressBucket(Bucket);
    for (Link = Bucket->Queue.Flink; Link != &Bucket->Queue; Link = Link->Flink)
    {
        PWAIT_ADDRESS_ENTRY Entry = CONTAINING_RECORD(Link, WAIT_ADDRESS_ENTRY, ListEntry);

        if (Entry->Address == Address)
        {
            /* Taking the entry off the queue here keeps two simultaneous calls from
             * waking the same waiter. */
            Entry->Address = NULL;
            RemoveEntryList(Link);
            Event = Entry->Event;
            break;
        }
    }
    RtlpUnlockWaitAddressBucket(Bucket);

    if (Event != NULL)
        NtSetEvent(Event, NULL);
}

/* EOF */
