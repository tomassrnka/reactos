/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/ipi.c
 * PURPOSE:         Inter-Processor Packet Interface
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

/* PRIVATE FUNCTIONS *********************************************************/

#ifndef _M_AMD64

/*
 * Packet protocol. A sender publishes the worker and its parameters in its
 * own PRCB, then for each target claims the target's single packet slot
 * (SignalDone), adds the target to its TargetSet and sends IPI_PACKET_READY.
 * The target runs the worker at IPI_LEVEL, removes itself from the sender's
 * TargetSet and only then frees its slot, so a late removal can never hit a
 * later packet of the same sender. Senders wait below IPI_LEVEL, so they keep
 * serving packets sent to them and two processors sending to each other do
 * not deadlock.
 */

#ifdef CONFIG_SMP

static
VOID
KiIpiPublishPacket(
    _In_ PKPRCB Prcb,
    _In_ PKIPI_WORKER WorkerRoutine,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2,
    _In_opt_ PVOID Parameter3)
{
    ASSERT(Prcb->TargetSet == 0);

    Prcb->WorkerRoutine = WorkerRoutine;
    Prcb->CurrentPacket[0] = Parameter1;
    Prcb->CurrentPacket[1] = Parameter2;
    Prcb->CurrentPacket[2] = Parameter3;
    KeMemoryBarrier();
}

static
VOID
KiIpiDeliverPacket(
    _In_ PKPRCB Prcb,
    _In_ KAFFINITY TargetSet)
{
    KAFFINITY Remaining = TargetSet;
    PKPRCB TargetPrcb;
    ULONG Processor;

    while (Remaining)
    {
        BitScanForwardAffinity(&Processor, Remaining);
        Remaining &= Remaining - 1;
        TargetPrcb = KiProcessorBlock[Processor];

        /* A processor serves one packet at a time. Claim its slot and signal it
           without interruption: a claimed slot whose IPI waits behind a
           broadcast this processor joined would block that broadcast */
        while (TRUE)
        {
            BOOLEAN Enable = KeDisableInterrupts();

            if (InterlockedCompareExchangePointer((PVOID*)&TargetPrcb->SignalDone, Prcb, NULL) == NULL)
            {
                InterlockedOr((PLONG)&Prcb->TargetSet, (LONG)AFFINITY_MASK(Processor));
                InterlockedOr((PLONG)&TargetPrcb->RequestSummary, IPI_PACKET_READY);
                HalRequestIpi(AFFINITY_MASK(Processor));
                KeRestoreInterrupts(Enable);
                break;
            }

            KeRestoreInterrupts(Enable);
            YieldProcessor();
        }
    }
}

static
LONG
KiIpiCountProcessors(
    _In_ KAFFINITY Set)
{
    LONG Count = 0;

    for (; Set; Set &= Set - 1)
        Count++;

    return Count;
}

static
VOID
KiIpiWaitForPacketDone(
    _In_ PKPRCB Prcb)
{
    while (*(volatile KAFFINITY*)&Prcb->TargetSet != 0)
    {
        YieldProcessor();
    }
}

#endif // CONFIG_SMP

VOID
NTAPI
KiIpiGenericCallTarget(IN PKIPI_CONTEXT PacketContext,
                       IN PVOID BroadcastFunction,
                       IN PVOID Argument,
                       IN PVOID Count)
{
    volatile LONG *Barrier = (volatile LONG*)Count;

    /* Report this processor ready, then wait until every processor is */
    InterlockedDecrement((PLONG)Barrier);
    while (*Barrier != 0)
    {
        YieldProcessor();
    }

    ((PKIPI_BROADCAST_WORKER)BroadcastFunction)((ULONG_PTR)Argument);
}

VOID
FASTCALL
KiIpiSend(IN KAFFINITY TargetProcessors,
          IN ULONG IpiRequest)
{
#ifdef CONFIG_SMP
    KAFFINITY Remaining = TargetProcessors;
    ULONG Processor;

    /* Freezing must reach processors running with interrupts disabled */
    if (IpiRequest & IPI_FREEZE)
    {
        HalSendNMI(TargetProcessors);
        IpiRequest &= ~IPI_FREEZE;
        if (!IpiRequest) return;
    }

    /* Mark the requests before interrupting, the interrupt takes them all */
    while (Remaining)
    {
        BitScanForwardAffinity(&Processor, Remaining);
        Remaining &= Remaining - 1;
        InterlockedOr((PLONG)&KiProcessorBlock[Processor]->RequestSummary, (LONG)IpiRequest);
    }

    HalRequestIpi(TargetProcessors);
#else
    /* Uniprocessor systems have no other processor to signal */
    ASSERT(FALSE);
#endif
}

VOID
NTAPI
KiIpiSendPacket(IN KAFFINITY TargetProcessors,
                IN PKIPI_WORKER WorkerFunction,
                IN PKIPI_BROADCAST_WORKER BroadcastFunction,
                IN ULONG_PTR Context,
                IN PULONG Count)
{
    /* FIXME: TODO */
    ASSERTMSG("Not yet implemented\n", FALSE);
}

VOID
FASTCALL
KiIpiSignalPacketDone(IN PKIPI_CONTEXT PacketContext)
{
#ifdef CONFIG_SMP
    PKPRCB Sender = (PKPRCB)PacketContext;

    /* Lets the sender go on early; the slot is freed once the worker returns */
    InterlockedAnd((PLONG)&Sender->TargetSet, ~(LONG)KeGetCurrentPrcb()->SetMember);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDoneAndStall(IN PKIPI_CONTEXT PacketContext,
                              IN volatile PULONG ReverseStall)
{
    /* FIXME: TODO */
    ASSERTMSG("Not yet implemented\n", FALSE);
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
KiIpiServiceRoutine(IN PKTRAP_FRAME TrapFrame,
                    IN PKEXCEPTION_FRAME ExceptionFrame)
{
#ifdef CONFIG_SMP
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKPRCB Sender;
    ULONG Request;

    ASSERT(KeGetCurrentIrql() == IPI_LEVEL);

    Request = InterlockedExchange((PLONG)&Prcb->RequestSummary, 0);

    if (Request & IPI_APC)
    {
        HalRequestSoftwareInterrupt(APC_LEVEL);
    }

    if (Request & IPI_DPC)
    {
        Prcb->DpcInterruptRequested = TRUE;
        HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
    }

    if (Request & IPI_PACKET_READY)
    {
        Sender = (PKPRCB)Prcb->SignalDone;
        ASSERT(Sender != NULL);

        Sender->WorkerRoutine((PKIPI_CONTEXT)Sender,
                              Sender->CurrentPacket[0],
                              Sender->CurrentPacket[1],
                              Sender->CurrentPacket[2]);

        /* Leave the sender's target set before freeing the slot, see the protocol above */
        InterlockedAnd((PLONG)&Sender->TargetSet, ~(LONG)Prcb->SetMember);
        InterlockedExchangePointer((PVOID*)&Prcb->SignalDone, NULL);
    }
#endif
    return TRUE;
}

/*
 * @implemented
 */
ULONG_PTR
NTAPI
KeIpiGenericCall(IN PKIPI_BROADCAST_WORKER Function,
                 IN ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql, OldIrql2;
#ifdef CONFIG_SMP
    KAFFINITY Affinity;
    volatile LONG Barrier;
    PKPRCB Prcb = KeGetCurrentPrcb();
#endif

    /* An interrupt below SYNCH_LEVEL could otherwise send a request through
       the packet of this processor while it is published */
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    OldIrql = KeRaiseIrqlToSynchLevel();

    /* Acquire the IPI lock */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#ifdef CONFIG_SMP
    /* The other active processors run the function together with this one */
    Prcb = KeGetCurrentPrcb();
    Affinity = KeActiveProcessors & ~Prcb->SetMember;
    Barrier = 1;
    if (Affinity)
    {
        Barrier += KiIpiCountProcessors(Affinity);
        KiIpiPublishPacket(Prcb,
                           KiIpiGenericCallTarget,
                           (PVOID)Function,
                           (PVOID)Argument,
                           (PVOID)&Barrier);
        KiIpiDeliverPacket(Prcb, Affinity);

        /* Spin until the other processors are ready */
        while (Barrier != 1)
        {
            YieldProcessor();
        }
    }
#endif

    /* Raise to IPI level */
    KeRaiseIrql(IPI_LEVEL, &OldIrql2);

#ifdef CONFIG_SMP
    /* Let the other processors know it is time */
    Barrier = 0;
#endif

    /* Call the function */
    Status = Function(Argument);

#ifdef CONFIG_SMP
    /* Wait for the other processors to finish; their requests stay pending meanwhile */
    if (Affinity)
    {
        ASSERT(Prcb == KeGetCurrentPrcb());
        KiIpiWaitForPacketDone(Prcb);
    }
#endif

    /* Release the lock */
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);

    /* Lower IRQL back */
    KeLowerIrql(OldIrql);
    return Status;
}

VOID
NTAPI
KiIpiSendRequest(
    _In_ KAFFINITY TargetSet,
    _In_ PKIPI_WORKER WorkerRoutine,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
    KIRQL OldIrql, WorkerIrql;
#ifdef CONFIG_SMP
    PKPRCB Prcb;
    KAFFINITY Remote;
#endif

    /* Above SYNCH_LEVEL no other processor may be waited for. That happens
       while this is the only one running, and in the debugger with the others
       frozen; it requests TLB flushes, and frozen processors flush their
       entire TLB when they thaw */
    if (KeGetCurrentIrql() > SYNCH_LEVEL)
    {
#ifdef CONFIG_SMP
        ASSERT((KiFreezeOwner == KeGetCurrentPrcb()) ||
               ((TargetSet & KeActiveProcessors & ~KeGetCurrentPrcb()->SetMember) == 0));
#endif
        if (TargetSet & KeGetCurrentPrcb()->SetMember)
            WorkerRoutine((PKIPI_CONTEXT)KeGetCurrentPrcb(), Parameter1, Parameter2, Parameter3);
        return;
    }

    /* Waiting at SYNCH_LEVEL keeps this processor fixed and still serves IPIs */
    OldIrql = KeRaiseIrqlToSynchLevel();

#ifdef CONFIG_SMP
    Prcb = KeGetCurrentPrcb();
    Remote = TargetSet & KeActiveProcessors & ~Prcb->SetMember;
    if (Remote)
    {
        KiIpiPublishPacket(Prcb, WorkerRoutine, Parameter1, Parameter2, Parameter3);
        KiIpiDeliverPacket(Prcb, Remote);
    }
#endif

    /* Run the worker here too, at the IRQL the targets run it */
    if (TargetSet & KeGetCurrentPrcb()->SetMember)
    {
        KeRaiseIrql(IPI_LEVEL, &WorkerIrql);
        WorkerRoutine((PKIPI_CONTEXT)KeGetCurrentPrcb(), Parameter1, Parameter2, Parameter3);
        KeLowerIrql(WorkerIrql);
    }

#ifdef CONFIG_SMP
    if (Remote)
    {
        KiIpiWaitForPacketDone(Prcb);
    }
#endif

    KeLowerIrql(OldIrql);
}

#endif // !_M_AMD64
