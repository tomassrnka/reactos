/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor freeze support for x86 and x64
 * COPYRIGHT:   Copyright 2023-2024 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/*

 IpiFrozen state graph (based on Windows behavior):

    +-----------------+     Freeze request      +-----------------+
    | RUNNING         |------------------------>| TARGET_FREEZE   |
    +-----------------+<---------               +-----------------+
            |^                  | Resume                |
     Freeze || Thaw        +-----------+ Thaw request   | Freeze IPI
            v|             | THAW      |<-----------\   v
    +-----------------+    +-----------+        +-----------------+
    | OWNER + ACTIVE  |         ^               | FROZEN          |
    +-----------------+         |               +-----------------+
            ^                   |                       ^
            | Kd proc switch    |                       | Kd proc switch
            v                   |                       v
    +-----------------+         |               +-----------------+
    | OWNER           |---------+               | FROZEN + ACTIVE |
    +-----------------+ Thaw request            +-----------------+

 */


PKPRCB KiFreezeOwner;
static KAFFINITY KiFreezeTargetSet;

/* Number of nested freezes by the owner, which has frozen the others once */
static ULONG KiFreezeDepth;

/* The frames of a processor that reports an unclaimed NMI, which runs the
   debugger from inside its NMI handler: no freeze NMI can interrupt it there */
static struct
{
    PKTRAP_FRAME TrapFrame;
    PKEXCEPTION_FRAME ExceptionFrame;
} volatile KiNmiReport[MAXIMUM_PROCESSORS];

#ifndef _M_IX86
/*
 * An NMI that arrives while a processor is frozen is held until the processor
 * returns from its NMI handler, and is taken right after that, when the freeze
 * is over. It belongs to the freeze. A processor that leaves its freeze notes
 * the time up to which the NMI that follows is still dropped.
 */
#define KI_NMI_GRACE_TICKS 200000ULL
static volatile ULONG64 KiNmiGraceEnd[MAXIMUM_PROCESSORS];
#endif

/* Number of NMIs dropped because a freeze was in progress */
static volatile LONG KiNmiDropped;

/* FUNCTIONS *****************************************************************/

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Make sure this is a freeze request */
    if (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        /* Not a freeze request, return FALSE to signal it is unhandled */
        return FALSE;
    }

    /* We are frozen now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

    /* Save the processor state */
    KiSaveProcessorState(TrapFrame, ExceptionFrame);

    /* Wait for the freeze owner to release us */
    while (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_THAW)
    {
        /* Check for Kd processor switch */
        if (CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE)
        {
            KCONTINUE_STATUS ContinueStatus;

            /* Enter the debugger */
            ContinueStatus = KdReportProcessorChange();

            /* Set the state back to frozen */
            CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

            /* If the status is ContinueSuccess, we need to release the freeze owner */
            if (ContinueStatus == ContinueSuccess)
            {
                /* Release the freeze owner */
                KiFreezeOwner->IpiFrozen = IPI_FROZEN_STATE_THAW;
            }
        }

        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Restore the processor state */
    KiRestoreProcessorState(TrapFrame, ExceptionFrame);

    /* Flush the TLB on this processor */
    KxFlushEntireCurrentTb();

#ifndef _M_IX86
    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* An NMI that was held meanwhile comes right after the return */
    KiNmiGraceEnd[CurrentPrcb->Number] = __rdtsc() + KI_NMI_GRACE_TICKS;
#endif

    /* Return TRUE to signal that we handled the freeze */
    return TRUE;
}

BOOLEAN
KiHandleNmiDuringFreeze(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame,
    _Out_ PBOOLEAN Frozen)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    *Frozen = FALSE;

    /*
     * A kernel without a second processor keeps no IpiFrozen state: the freeze
     * flag tells that the debugger runs on this one, and an NMI that reaches
     * it there must not enter the debugger again from inside the session.
     */
    if (KeNumberProcessors == 1 && KiFreezeFlag != 0)
    {
        InterlockedIncrement(&KiNmiDropped);
        return TRUE;
    }

#ifndef _M_IX86
    /* The NMI that was held while this processor was frozen, unless the
       hardware reports a reason for it */
    if (KiNmiGraceEnd[CurrentPrcb->Number] != 0)
    {
        BOOLEAN Grace = __rdtsc() < KiNmiGraceEnd[CurrentPrcb->Number];

        KiNmiGraceEnd[CurrentPrcb->Number] = 0;
        if (Grace && (READ_PORT_UCHAR((PUCHAR)0x61) & 0xC0) == 0)
        {
            InterlockedIncrement(&KiNmiDropped);
            return TRUE;
        }
    }
#endif

    /*
     * An NMI that is not a freeze request reaches a processor that already is
     * part of a freeze: a repeated NMI (or one that hit all processors at
     * once, like QEMU's inject-nmi) while the debugger runs. The freeze owner,
     * a frozen processor and one that is thawing already stand where the NMI
     * would take them; reporting it would enter the debugger a second time,
     * from inside the freeze, and its thaw would then find processors in the
     * wrong state. Drop the NMI.
     */
    if (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
    {
        /* A real NMI (parity, IOCHK) must not be lost: see KiCheckDroppedNmi.
           Nothing is printed here: that enters the debugger, which a frozen
           processor must not do while the owner waits for it */
        InterlockedIncrement(&KiNmiDropped);
        return TRUE;
    }

    /*
     * The debugger runs on another processor, which freezes this one next
     * (or has just done so). This NMI cannot be reported to the HAL while the
     * owner waits for this processor, and the freeze NMI cannot interrupt the
     * handler that already runs: serve the freeze request from here, then
     * drop the NMI.
     */
    if (KiFreezeOwner != NULL && KiFreezeOwner != CurrentPrcb)
    {
        while (KiFreezeOwner != NULL)
        {
            if (KiProcessorFreezeHandler(TrapFrame, ExceptionFrame))
            {
                *Frozen = TRUE;
                break;
            }

            YieldProcessor();
            KeMemoryBarrier();
        }

        return TRUE;
    }

    return FALSE;
}

ULONG
KiCheckDroppedNmi(
    VOID)
{
    ULONG Count;

    /* Called by the processor that ended a freeze. NMIs that arrived during it
       were dropped; one that the hardware reports (parity error, IOCHK) is
       still pending in the system control port and gets its normal handling.
       NMIs from QEMU and IPIs set no reason bit and are done. */
    Count = InterlockedExchange(&KiNmiDropped, 0);
    if (Count == 0)
        return 0;

#if !defined(SARCH_PC98) && !defined(SARCH_XBOX)
    if ((READ_PORT_UCHAR((PUCHAR)0x61) & 0xC0) == 0)
        return Count;

    if (!KiHandleNmi())
        HalHandleNMI(NULL);
#endif

    return Count;
}

VOID
KiBeginNmiReport(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_ PKEXCEPTION_FRAME ExceptionFrame)
{
    ULONG Number = KeGetCurrentPrcb()->Number;

    KiNmiReport[Number].ExceptionFrame = ExceptionFrame;
    KiNmiReport[Number].TrapFrame = TrapFrame;
}

VOID
KiEndNmiReport(
    VOID)
{
    KiNmiReport[KeGetCurrentPrcb()->Number].TrapFrame = NULL;
}

VOID
KiHandleLostNmi(
    VOID)
{
    KIRQL OldIrql;

    /* An NMI that could not run its handler (see the i386 NMI TSS fault): count
       it and check, as at the end of a freeze, whether the hardware reports it */
    InterlockedIncrement(&KiNmiDropped);
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KiCheckDroppedNmi();
    KeLowerIrql(OldIrql);
}

BOOLEAN
NTAPI
KxFreezeExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Avoid blocking on recursive debug action */
    if (KiFreezeOwner == CurrentPrcb)
    {
        /* The matching KxThawExecution must not thaw the others yet */
        KiFreezeDepth++;
        return FALSE;
    }

    /* Try to acquire the freeze owner */
    while (InterlockedCompareExchangePointer((PVOID*)&KiFreezeOwner, CurrentPrcb, NULL))
    {
        /* Someone else was faster. We expect an NMI to freeze any time.
           Spin here until the freeze owner is available. A processor in its
           NMI handler (an NMI report enters the debugger there) gets no NMI,
           and the owner waits for it: it serves the freeze request itself. */
        while (KiFreezeOwner != NULL)
        {
            PKTRAP_FRAME ReportFrame = KiNmiReport[CurrentPrcb->Number].TrapFrame;

            if (ReportFrame != NULL &&
                CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_TARGET_FREEZE &&
                KiProcessorFreezeHandler(ReportFrame, KiNmiReport[CurrentPrcb->Number].ExceptionFrame))
            {
                CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
            }

            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    /* We are the owner now and active */
    KiFreezeDepth = 0;
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;

    /* A starting processor joins KeActiveProcessors at any time; request,
       signal and thaw exactly the processors of this snapshot */
    KiFreezeTargetSet = KeActiveProcessors & ~CurrentPrcb->SetMember;

    /* Loop all target processors */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (KiFreezeTargetSet & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* Only the active processor is allowed to change IpiFrozen */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING);

            /* Request target to freeze */
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_TARGET_FREEZE;
        }
    }

    /* Send the freeze IPI */
    if (KiFreezeTargetSet) KiIpiSend(KiFreezeTargetSet, IPI_FREEZE);

    /* Wait for all targets to be frozen */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (KiFreezeTargetSet & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* Wait for the target to be frozen */
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }

    /* All targets are frozen, we can continue */
    return TRUE;
}

BOOLEAN
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    KAFFINITY ThawSet = 0;
    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);

    /* A nested freeze of the owner leaves the others frozen for the outer one */
    if (KiFreezeDepth != 0)
    {
        KiFreezeDepth--;
        return FALSE;
    }

    /* Loop all frozen processors */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (KiFreezeTargetSet & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* A processor that already runs has nothing to wait for. Any
               other state is not expected here, but must still be woken */
            if (TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING)
                continue;

            /* Make sure they are still frozen */
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_FROZEN);

            /* Request target to thaw */
            ThawSet |= AFFINITY_MASK(i);
            TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
        }
    }

    /* Wait for all targets to be running */
    for (ULONG i = 0; i < MAXIMUM_PROCESSORS; i++)
    {
        if (ThawSet & AFFINITY_MASK(i))
        {
            PKPRCB TargetPrcb = KiProcessorBlock[i];

            /* Wait for the target to be running again */
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                YieldProcessor();
                KeMemoryBarrier();
            }
        }
    }

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Release the freeze owner */
    InterlockedExchangePointer((PVOID*)&KiFreezeOwner, NULL);
    return TRUE;
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;

    /* Make sure that the processor index is valid */
    ASSERT(ProcessorIndex < KeNumberProcessors);

    /* We are no longer active */
    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);
    CurrentPrcb->IpiFrozen &= ~IPI_FROZEN_FLAG_ACTIVE;

    /* Inform the target processor that it's his turn now */
    TargetPrcb = KiProcessorBlock[ProcessorIndex];
    TargetPrcb->IpiFrozen |= IPI_FROZEN_FLAG_ACTIVE;

    /* If we are not the freeze owner, we return back to the freeze loop */
    if (KiFreezeOwner != CurrentPrcb)
    {
        return ContinueNextProcessor;
    }

    /* Loop until it's our turn again */
    while (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_OWNER)
    {
        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Check if we have been thawed */
    if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
    {
        /* Another CPU has completed, we can leave the debugger now */
        KdpDprintf("[%u] KxSwitchKdProcessor: ContinueSuccess\n", KeGetCurrentProcessorNumber());
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
        return ContinueSuccess;
    }

    /* We have been reselected, return to Kd to continue in the debugger */
    ASSERT(CurrentPrcb->IpiFrozen == (IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE));

    return ContinueProcessorReselected;
}
