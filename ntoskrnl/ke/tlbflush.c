/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Support routines for flushing the TLB
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>

/* FUNCTIONS ******************************************************************/

#ifdef CONFIG_SMP
#if defined(_M_IX86) || defined(_M_AMD64)
/*
 * Flushes the other target processors with one hypercall when the hypervisor
 * recommends it (no IPI and no wait for the targets), and this processor
 * with the IPI worker. Returns FALSE when the IPI path must do the flush,
 * including the cases where it waits for no other processor.
 */
static
BOOLEAN
KiFlushTbWithHypercall(
    _In_ KAFFINITY TargetSet,
    _In_ BOOLEAN NonGlobalOnly,
    _In_opt_ PVOID Address,
    _In_ ULONG NumberOfPages,
    _In_ PKIPI_WORKER WorkerRoutine,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2)
{
    KIRQL OldIrql, WorkerIrql;
    PKPRCB Prcb;
    KAFFINITY Remote;

    if (!(KiHvEnlightenments & KI_HV_REMOTE_FLUSH) ||
        (KeGetCurrentIrql() > SYNCH_LEVEL))
    {
        return FALSE;
    }

    /* Stay on this processor, as KiIpiSendRequest does */
    OldIrql = KeRaiseIrqlToSynchLevel();
    Prcb = KeGetCurrentPrcb();
    if ((KiFreezeOwner == Prcb) || (Prcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE))
    {
        KeLowerIrql(OldIrql);
        return FALSE;
    }

    /* Starting processors included, see KiIpiSendRequest */
    Remote = TargetSet & KiGetTbFlushProcessors() & ~Prcb->SetMember;
    if (Remote && !KiHvFlushTb(Remote, NonGlobalOnly, Address, NumberOfPages))
    {
        KeLowerIrql(OldIrql);
        return FALSE;
    }

    if (TargetSet & Prcb->SetMember)
    {
        KeRaiseIrql(IPI_LEVEL, &WorkerIrql);
        WorkerRoutine((PKIPI_CONTEXT)Prcb, Parameter1, Parameter2, NULL);
        KeLowerIrql(WorkerIrql);
    }

    KeLowerIrql(OldIrql);
    return TRUE;
}
#else
#define KiFlushTbWithHypercall(TargetSet, NonGlobalOnly, Address, NumberOfPages, \
                               WorkerRoutine, Parameter1, Parameter2) FALSE
#endif

static
VOID
NTAPI
KiFlushSingleTbIpiWorker(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
    KxFlushSingleCurrentTb(Parameter1);
}
#endif

VOID
NTAPI
KeFlushSingleTb(
    _In_ PVOID Address,
    _In_ BOOLEAN AllProcessors)
{
#ifdef CONFIG_SMP
    KAFFINITY TargetSet;

    /* A processor that joins the process after this point sees the new PTEs */
    KeMemoryBarrier();
    TargetSet = AllProcessors ?
        KiGetTbFlushProcessors() : KeGetCurrentProcess()->ActiveProcessors;
    if (KiFlushTbWithHypercall(TargetSet, FALSE, Address, 1,
                               KiFlushSingleTbIpiWorker, Address, NULL))
    {
        return;
    }
    KiIpiSendRequest(TargetSet,
                     KiFlushSingleTbIpiWorker,
                     Address,
                     NULL,
                     NULL);
#else
    KxFlushSingleCurrentTb(Address);
#endif
}

#ifdef CONFIG_SMP
static
VOID
NTAPI
KiFlushRangeTbIpiWorker(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
    KxFlushRangeCurrentTb(Parameter1, PtrToUlong(Parameter2));
}
#endif

VOID
NTAPI
KeFlushRangeTb(
    _In_ PVOID Address,
    _In_ ULONG NumberOfPages,
    _In_ BOOLEAN Global)
{
    /* If Global is true it means all TLB entries on all processors,
       otherwise local TLB entries on the process's active processors. */
    if (Global)
    {
        if (NumberOfPages > KxFlushIndividualGlobalPagesMaximum)
        {
            /* Flush the entire TLB instead */
            KeFlushEntireTb(TRUE, TRUE);
            return;
        }
    }
    else
    {
        if (NumberOfPages > KxFlushIndividualProcessPagesMaximum)
        {
            /* Flush the entire TLB instead */
            KeFlushProcessTb();
            return;
        }
    }

#ifdef CONFIG_SMP
    KAFFINITY TargetSet;

    /* A processor that joins the process after this point sees the new PTEs */
    KeMemoryBarrier();
    TargetSet = Global ?
        KiGetTbFlushProcessors() : KeGetCurrentProcess()->ActiveProcessors;
    if (KiFlushTbWithHypercall(TargetSet, FALSE, Address, NumberOfPages,
                               KiFlushRangeTbIpiWorker, Address,
                               ULongToPtr(NumberOfPages)))
    {
        return;
    }
    KiIpiSendRequest(TargetSet,
                     KiFlushRangeTbIpiWorker,
                     Address,
                     ULongToPtr(NumberOfPages),
                     NULL);
#else
    KxFlushRangeCurrentTb(Address, NumberOfPages);
#endif
}

#ifdef CONFIG_SMP
static
VOID
NTAPI
KiFlushProcessTbIpiWorker(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
    KxFlushProcessCurrentTb();
}
#endif

VOID
NTAPI
KeFlushProcessTb(VOID)
{
#ifdef CONFIG_SMP
    /* A processor that joins the process after this point sees the new PTEs */
    KeMemoryBarrier();
    if (KiFlushTbWithHypercall(KeGetCurrentProcess()->ActiveProcessors,
                               TRUE, NULL, 0,
                               KiFlushProcessTbIpiWorker, NULL, NULL))
    {
        return;
    }
    KiIpiSendRequest(KeGetCurrentProcess()->ActiveProcessors,
                     KiFlushProcessTbIpiWorker,
                     NULL,
                     NULL,
                     NULL);
#else
    KxFlushProcessCurrentTb();
#endif
}

#ifdef CONFIG_SMP
static
VOID
NTAPI
KiFlushEntireTbIpiWorker(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_ PVOID Parameter3)
{
    /* Flush the current TB */
    KxFlushEntireCurrentTb();
}
#endif

VOID
NTAPI
KeFlushEntireTb(
    _In_ BOOLEAN Invalid,
    _In_ BOOLEAN AllProcessors)
{
#ifdef CONFIG_SMP
    KAFFINITY TargetSet;

    /* A processor that joins the process after this point sees the new PTEs */
    KeMemoryBarrier();
    TargetSet = AllProcessors ?
        KiGetTbFlushProcessors() : KeGetCurrentProcess()->ActiveProcessors;
    if (KiFlushTbWithHypercall(TargetSet, FALSE, NULL, 0,
                               KiFlushEntireTbIpiWorker, NULL, NULL))
    {
        return;
    }
    KiIpiSendRequest(TargetSet,
                     KiFlushEntireTbIpiWorker,
                     NULL,
                     NULL,
                     NULL);
#else
    KxFlushEntireCurrentTb();
#endif
}
