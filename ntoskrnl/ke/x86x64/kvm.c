/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KVM paravirtual remote TLB flush
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * The KVM guest interface used here is documented in the Linux kernel
 * (Documentation/virt/kvm/x86/cpuid.rst and msr.rst) and in the KVM user API
 * header (arch/x86/include/uapi/asm/kvm_para.h):
 *
 * - CPUID 0x40000000 returns "KVMKVMKVM\0\0\0" in EBX, ECX and EDX, and the
 *   highest KVM leaf in EAX (0 on old hosts, which means 0x40000001).
 * - CPUID 0x40000001 returns the feature bits in EAX (STEAL_TIME is bit 5,
 *   PV_TLB_FLUSH bit 9) and hints in EDX (REALTIME, bit 0: vCPUs are never
 *   preempted for long, so there is nothing to gain).
 * - A vCPU registers its steal-time area by writing the area's guest physical
 *   address, 64-byte aligned, with bit 0 set, to MSR 0x4B564D03. The area is
 *   zeroed by the guest, stays in guest RAM and the host may write it until
 *   it is unregistered.
 * - Byte 16 of the area is "preempted". The host sets KVM_VCPU_PREEMPTED in it
 *   when it deschedules the vCPU. Before it runs the vCPU again it exchanges
 *   the byte with zero and, if KVM_VCPU_FLUSH_TLB was set, flushes the vCPU's
 *   TLB first.
 *
 * So a remote TLB flush need not interrupt, and wait for, a processor whose
 * byte reads PREEMPTED: a compare-exchange from that value to the value with
 * FLUSH_TLB added asks the host to flush the TLB at the vCPU's next entry. If
 * the exchange succeeds, the vCPU has not run since the byte was read and will
 * not run an instruction before the flush. If it fails, the vCPU resumed in
 * between (the host cleared the byte), and the caller sends the IPI as before.
 * The page table changes were stored before the exchange, a locked instruction,
 * so the host's flush comes after them.
 *
 * Only TLB flushes use this: the flush at entry replaces the worker exactly.
 * Requests that need the target to run code (freeze, DPCs, APCs, the generic
 * call) always send the IPI.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifdef CONFIG_SMP

/* DEFINITIONS ****************************************************************/

#define CPUID_HYPERVISOR_PRESENT    0x80000000  /* CPUID 1, ECX bit 31 */
#define KVM_CPUID_SIGNATURE         0x40000000
#define KVM_CPUID_FEATURES          0x40000001
#define KVM_SIGNATURE_EBX           0x4B4D564B  /* "KVMK" */
#define KVM_SIGNATURE_ECX           0x564B4D56  /* "VMKV" */
#define KVM_SIGNATURE_EDX           0x0000004D  /* "M\0\0\0" */
#define KVM_FEATURE_STEAL_TIME      5
#define KVM_FEATURE_PV_TLB_FLUSH    9
#define KVM_HINTS_REALTIME          0
#define MSR_KVM_STEAL_TIME          0x4B564D03
#define KVM_MSR_ENABLED             1
#define KVM_VCPU_PREEMPTED          0x01
#define KVM_VCPU_FLUSH_TLB          0x02

typedef struct _KI_KVM_STEAL_TIME
{
    ULONG64 Steal;
    ULONG Version;
    ULONG Flags;
    UCHAR Preempted;
    UCHAR Reserved0[3];
    ULONG Reserved1[11];
} KI_KVM_STEAL_TIME, *PKI_KVM_STEAL_TIME;

C_ASSERT(sizeof(KI_KVM_STEAL_TIME) == 64);
C_ASSERT(FIELD_OFFSET(KI_KVM_STEAL_TIME, Preempted) == 16);

/* GLOBALS ********************************************************************/

/* Nonpaged and never freed, one cache line per processor; the host writes it
   at every entry of the vCPU */
static DECLSPEC_ALIGN(64) KI_KVM_STEAL_TIME KiKvmStealTime[MAXIMUM_PROCESSORS];

BOOLEAN KiKvmPvTlbFlush;

/* FUNCTIONS ******************************************************************/

static
BOOLEAN
KiKvmHasPvTlbFlush(VOID)
{
    INT Registers[4];
    ULONG MaximumLeaf;

    /* Leaves 0x40000000 and up only mean something under a hypervisor */
    __cpuid(Registers, 1);
    if (!(Registers[2] & CPUID_HYPERVISOR_PRESENT))
        return FALSE;

    __cpuid(Registers, KVM_CPUID_SIGNATURE);
    if (((ULONG)Registers[1] != KVM_SIGNATURE_EBX) ||
        ((ULONG)Registers[2] != KVM_SIGNATURE_ECX) ||
        ((ULONG)Registers[3] != KVM_SIGNATURE_EDX))
    {
        return FALSE;
    }

    MaximumLeaf = Registers[0] ? (ULONG)Registers[0] : KVM_CPUID_FEATURES;
    if (MaximumLeaf < KVM_CPUID_FEATURES)
        return FALSE;

    __cpuid(Registers, KVM_CPUID_FEATURES);
    if (!(Registers[0] & (1 << KVM_FEATURE_STEAL_TIME)) ||
        !(Registers[0] & (1 << KVM_FEATURE_PV_TLB_FLUSH)) ||
        (Registers[3] & (1 << KVM_HINTS_REALTIME)))
    {
        return FALSE;
    }

    return TRUE;
}

static
VOID
KiKvmRegisterStealTime(
    _In_ PKPRCB Prcb)
{
    PKI_KVM_STEAL_TIME Area = &KiKvmStealTime[Prcb->Number];
    PHYSICAL_ADDRESS Physical;

    /* A processor whose area is not registered keeps a zero preempted byte
       and so always gets the IPI */
    RtlZeroMemory(Area, sizeof(*Area));
    KeMemoryBarrier();

    Physical = MmGetPhysicalAddress(Area);
    if ((Physical.QuadPart == 0) || (Physical.QuadPart & (sizeof(*Area) - 1)))
    {
        DPRINT1("KVM steal-time area of processor %u at %I64x is unusable\n",
                Prcb->Number, Physical.QuadPart);
        return;
    }

    __writemsr(MSR_KVM_STEAL_TIME, Physical.QuadPart | KVM_MSR_ENABLED);
}

/*!
 * \brief Enables the paravirtual TLB flush and registers the boot processor.
 *        Called once on the boot processor before the others start.
 *
 * \param[in] LoadOptions - The boot options; NOPVTLBFLUSH keeps IPIs for
 *            every flush and leaves the steal-time MSR alone.
 */
CODE_SEG("INIT")
VOID
NTAPI
KiKvmInitializePvTlbFlush(
    _In_opt_ PCSTR LoadOptions)
{
    if (LoadOptions && strstr(LoadOptions, "NOPVTLBFLUSH"))
    {
        DPRINT1("KVM paravirtual TLB flush disabled by NOPVTLBFLUSH\n");
        return;
    }

    if (!KiKvmHasPvTlbFlush())
        return;

    KiKvmRegisterStealTime(KeGetCurrentPrcb());

    /* Published before any other processor starts */
    KiKvmPvTlbFlush = TRUE;
    DPRINT1("KVM paravirtual TLB flush enabled\n");
}

/*!
 * \brief Registers the steal-time area of a starting processor, before it
 *        becomes a target of TLB flushes.
 */
VOID
NTAPI
KiKvmInitializeProcessor(
    _In_ PKPRCB Prcb)
{
    if (KiKvmPvTlbFlush)
        KiKvmRegisterStealTime(Prcb);
}

/*!
 * \brief Asks the host to flush the TLB of a descheduled vCPU when it runs
 *        again.
 *
 * \return TRUE if the host will flush that processor's TLB before it runs
 *         another instruction, so no IPI is needed. FALSE if the processor
 *         is running or resumed meanwhile; the caller must send the IPI.
 */
BOOLEAN
FASTCALL
KiKvmTryFlushTbOnEntry(
    _In_ ULONG Processor)
{
    PKI_KVM_STEAL_TIME Area = &KiKvmStealTime[Processor];
    UCHAR State;

    State = *(volatile UCHAR *)&Area->Preempted;
    if (!(State & KVM_VCPU_PREEMPTED))
        return FALSE;

    /* Fails if the host cleared the byte for a new entry since the read */
    return (UCHAR)_InterlockedCompareExchange8((volatile char *)&Area->Preempted,
                                               (char)(State | KVM_VCPU_FLUSH_TLB),
                                               (char)State) == State;
}

/*!
 * \brief Takes the descheduled vCPUs out of the target set of a TLB flush,
 *        after asking the host to flush them at their next entry.
 *
 * \return The processors that still need the flush IPI.
 */
KAFFINITY
FASTCALL
KiKvmFlushPreemptedTb(
    _In_ KAFFINITY TargetSet)
{
    KAFFINITY Remaining = TargetSet;
    KAFFINITY Signal = TargetSet;
    ULONG Processor;

    while (Remaining)
    {
        BitScanForwardAffinity(&Processor, Remaining);
        Remaining &= Remaining - 1;

        if (KiKvmTryFlushTbOnEntry(Processor))
            Signal &= ~AFFINITY_MASK(Processor);
    }

    return Signal;
}

#endif // CONFIG_SMP
