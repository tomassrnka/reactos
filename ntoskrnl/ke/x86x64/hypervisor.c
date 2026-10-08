/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     TLB flushes and IPIs through hypercalls of a Microsoft-compatible hypervisor
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 * REFERENCES:  Microsoft, "Hypervisor Top Level Functional Specification", v6.0b
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#include <x86x64/HvTlfs.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Called by the i386 hypercall stub */
PVOID KiHvHypercallPage;

#ifdef CONFIG_SMP

/*
 * Hypercall input of each processor: a flush header, a sparse processor set
 * and one GVA range. A block aligned to its size never crosses a page, as
 * the specification requires (3.6).
 */
#define KI_HV_INPUT_SIZE    128
#define KI_HV_MAX_BANKS     ((KI_HV_INPUT_SIZE / sizeof(ULONG64)) - 5)

/* The enlightenments in use, KI_HV_* */
ULONG KiHvEnlightenments;

/* What the hypervisor grants and recommends, read on the boot processor */
static ULONG KiHvPrivileges;
static ULONG KiHvRecommendations;
static BOOLEAN KiHvSuppressed;

/* Hypervisor index of each processor; HV_ANY_VP until the processor read its own */
static volatile ULONG KiHvVpIndex[MAXIMUM_PROCESSORS];

static DECLSPEC_ALIGN(KI_HV_INPUT_SIZE) UCHAR KiHvInput[MAXIMUM_PROCESSORS][KI_HV_INPUT_SIZE];
static ULONG64 KiHvInputGpa[MAXIMUM_PROCESSORS];

/* Set while a processor fills or passes its input block; an NMI that needs the
   block meanwhile finds it taken, and its caller takes the path without
   hypercalls */
static volatile LONG KiHvInputBusy[MAXIMUM_PROCESSORS];

/* PRIVATE FUNCTIONS **********************************************************/

#ifdef _M_AMD64
typedef ULONG64 (*PKI_HV_HYPERCALL)(ULONG64 Control, ULONG64 Input, ULONG64 Output);

/* The hypercall page takes the input value in RCX, the parameters in RDX and R8 */
FORCEINLINE
ULONG64
KiHvHypercall(
    _In_ ULONG64 Control,
    _In_ ULONG64 Input,
    _In_ ULONG64 Output)
{
    return ((PKI_HV_HYPERCALL)KiHvHypercallPage)(Control, Input, Output);
}
#else
ULONG64
NTAPI
KiHvHypercall(
    _In_ ULONG64 Control,
    _In_ ULONG64 Input,
    _In_ ULONG64 Output);
#endif

static
VOID
KiHvDetect(VOID)
{
    INT CpuInfo[4];

    /* Hypervisor present bit, then the interface signature (2.2, 2.4) */
    __cpuid(CpuInfo, 1);
    if (!(CpuInfo[2] & (1UL << HV_CPUID_HYPERVISOR_PRESENT_BIT)))
        return;

    __cpuid(CpuInfo, HV_CPUID_VENDOR_AND_MAX_FUNCTIONS);
    if ((ULONG)CpuInfo[0] < HV_CPUID_IMPLEMENTATION_LIMITS)
        return;

    __cpuid(CpuInfo, HV_CPUID_INTERFACE);
    if ((ULONG)CpuInfo[0] != HV_INTERFACE_SIGNATURE_HV1)
        return;

    if (KeLoaderBlock->LoadOptions &&
        strstr(KeLoaderBlock->LoadOptions, "NOHVENLIGHT"))
    {
        KiHvSuppressed = TRUE;
        return;
    }

    __cpuid(CpuInfo, HV_CPUID_FEATURES);
    KiHvPrivileges = (ULONG)CpuInfo[0];

    __cpuid(CpuInfo, HV_CPUID_ENLIGHTENMENT_INFO);
    KiHvRecommendations = (ULONG)CpuInfo[0];
}

/*
 * Takes this processor's hypercall input block, with interrupts disabled.
 * Returns NULL when an interrupted user on this processor holds it.
 */
static
PULONG64
KiHvAcquireInput(
    _Out_ PULONG Processor,
    _Out_ PBOOLEAN Enable)
{
    *Enable = KeDisableInterrupts();
    *Processor = KeGetCurrentPrcb()->Number;
    if (InterlockedExchange(&KiHvInputBusy[*Processor], 1))
    {
        KeRestoreInterrupts(*Enable);
        return NULL;
    }

    return (PULONG64)KiHvInput[*Processor];
}

static
VOID
KiHvReleaseInput(
    _In_ ULONG Processor,
    _In_ BOOLEAN Enable)
{
    InterlockedExchange(&KiHvInputBusy[Processor], 0);
    KeRestoreInterrupts(Enable);
}

/*
 * Gets the processor mask of the hypercalls that take one: every target
 * has a known VP index below 64.
 */
static
BOOLEAN
KiHvGetVpMask(
    _In_ KAFFINITY TargetSet,
    _Out_ PULONG64 Mask)
{
    ULONG Processor, VpIndex;

    *Mask = 0;
    for (; TargetSet; TargetSet &= TargetSet - 1)
    {
        BitScanForwardAffinity(&Processor, TargetSet);
        VpIndex = KiHvVpIndex[Processor];
        if (VpIndex >= HV_VP_SET_BANK_SIZE)
            return FALSE;
        *Mask |= 1ULL << VpIndex;
    }

    return TRUE;
}

/*
 * Writes a sparse processor set (7.8.7.4) at Set: the format, the mask of
 * valid banks and the banks in increasing order. Returns the number of
 * banks, or 0 when a VP index is unknown or the set does not fit.
 */
static
ULONG
KiHvBuildVpSet(
    _In_ KAFFINITY TargetSet,
    _Out_writes_(2 + KI_HV_MAX_BANKS) PULONG64 Set)
{
    KAFFINITY Remaining;
    ULONG64 ValidBanks = 0, Banks;
    ULONG Processor, VpIndex, Bank, Count = 0;

    for (Remaining = TargetSet; Remaining; Remaining &= Remaining - 1)
    {
        BitScanForwardAffinity(&Processor, Remaining);
        VpIndex = KiHvVpIndex[Processor];
        if (VpIndex >= HV_VP_SET_BANK_SIZE * 64)
            return 0;
        ValidBanks |= 1ULL << (VpIndex / HV_VP_SET_BANK_SIZE);
    }

    Set[0] = HV_GENERIC_SET_SPARSE_4K;
    Set[1] = ValidBanks;
    for (Banks = ValidBanks; Banks; Banks &= Banks - 1)
    {
        if (Count == KI_HV_MAX_BANKS)
            return 0;

        /* The lowest valid bank */
        for (Bank = 0; !(Banks & (1ULL << Bank)); Bank++);

        Set[2 + Count] = 0;
        for (Remaining = TargetSet; Remaining; Remaining &= Remaining - 1)
        {
            BitScanForwardAffinity(&Processor, Remaining);
            VpIndex = KiHvVpIndex[Processor];
            if ((VpIndex / HV_VP_SET_BANK_SIZE) == Bank)
                Set[2 + Count] |= 1ULL << (VpIndex % HV_VP_SET_BANK_SIZE);
        }
        Count++;
    }

    return Count;
}

static
BOOLEAN
KiHvCheckResult(
    _In_ ULONG64 Result,
    _In_ ULONG Enlightenment)
{
    if ((Result & HV_HYPERCALL_RESULT_MASK) == HV_STATUS_SUCCESS)
        return TRUE;

    /* Give up on the enlightenment; the callers fall back to IPIs */
    DPRINT1("Hypercall failed with status 0x%I64x, disabling enlightenment 0x%lx\n",
            Result & HV_HYPERCALL_RESULT_MASK, Enlightenment);
    InterlockedAnd((PLONG)&KiHvEnlightenments, ~(LONG)Enlightenment);
    return FALSE;
}

/* FUNCTIONS ******************************************************************/

/*
 * Runs on every processor after HalInitializeProcessor, before it can be
 * the target of TLB flushes. The boot processor first looks for the
 * hypervisor interface.
 */
VOID
NTAPI
KiHvInitializeProcessor(
    _In_ ULONG Processor)
{
    ULONG i;

    if (Processor == 0)
    {
        for (i = 0; i < MAXIMUM_PROCESSORS; i++)
            KiHvVpIndex[i] = HV_ANY_VP;
        KiHvDetect();
    }

    if ((KiHvPrivileges & HV_ACCESS_VP_INDEX) && (Processor < MAXIMUM_PROCESSORS))
        KiHvVpIndex[Processor] = (ULONG)__readmsr(HV_X64_MSR_VP_INDEX);
}

/*
 * Establishes the hypercall interface (3.13) on the boot processor, before
 * the other processors start, and enables the flush and IPI hypercalls the
 * hypervisor recommends.
 */
CODE_SEG("INIT")
VOID
NTAPI
KiHvInitializeHypercalls(VOID)
{
    ULONG64 Value;
    PHYSICAL_ADDRESS PhysicalAddress;
    PVOID Page;
    ULONG Enlightenments = 0;
    ULONG i;

    /* Detected before the debugger was up, reported now */
    if (KiHvSuppressed)
        DPRINT1("Hypervisor enlightenments disabled by NOHVENLIGHT\n");
    if (KiHvPrivileges)
    {
        DPRINT1("Hypervisor interface Hv#1: privileges 0x%lx, recommendations 0x%lx\n",
                KiHvPrivileges, KiHvRecommendations);
    }

    if (!(KiHvPrivileges & HV_ACCESS_HYPERCALL_MSRS) ||
        !(KiHvPrivileges & HV_ACCESS_VP_INDEX) ||
        (KiHvVpIndex[0] == HV_ANY_VP))
    {
        return;
    }

    if (KiHvRecommendations & HV_RECOMMEND_HYPERCALL_REMOTE_FLUSH)
        Enlightenments |= KI_HV_REMOTE_FLUSH;
    if (KiHvRecommendations & HV_RECOMMEND_CLUSTER_IPI)
        Enlightenments |= KI_HV_CLUSTER_IPI;
    if (!Enlightenments)
        return;
    if (KiHvRecommendations & HV_RECOMMEND_EX_PROCESSOR_MASKS)
        Enlightenments |= KI_HV_EX_PROCESSOR_SETS;

    /* The hypercall page can only be enabled after the OS identified itself (2.6) */
    if (__readmsr(HV_X64_MSR_GUEST_OS_ID) == 0)
    {
        __writemsr(HV_X64_MSR_GUEST_OS_ID,
                   HV_GUEST_OS_ID_OPEN_SOURCE |
                   ((ULONG64)NtMajorVersion << 24) |
                   ((ULONG64)NtMinorVersion << 16) |
                   (NtBuildNumber & 0xFFFF));
    }

    Value = __readmsr(HV_X64_MSR_HYPERCALL);
    if (Value & HV_X64_MSR_HYPERCALL_LOCKED)
    {
        /* The page cannot move: use it where it is */
        if (!(Value & HV_X64_MSR_HYPERCALL_ENABLE))
            return;
        PhysicalAddress.QuadPart = Value & ~(ULONG64)(PAGE_SIZE - 1);
        Page = MmMapIoSpace(PhysicalAddress, PAGE_SIZE, MmCached);
        if (!Page)
            return;
    }
    else
    {
        /* A page of our own, used for nothing else (3.13 step 6). Nonpaged
           pool is executable, and a page-sized block starts a page */
        Page = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, TAG_KERNEL);
        if (!Page)
            return;
        if (BYTE_OFFSET(Page))
        {
            ExFreePoolWithTag(Page, TAG_KERNEL);
            return;
        }

        PhysicalAddress = MmGetPhysicalAddress(Page);
        __writemsr(HV_X64_MSR_HYPERCALL,
                   (Value & HV_X64_MSR_HYPERCALL_RESERVED) |
                   (ULONG64)PhysicalAddress.QuadPart |
                   HV_X64_MSR_HYPERCALL_ENABLE);

        /* The enable bit stays clear when the hypervisor refused */
        if (!(__readmsr(HV_X64_MSR_HYPERCALL) & HV_X64_MSR_HYPERCALL_ENABLE))
        {
            DPRINT1("The hypervisor did not enable the hypercall page\n");
            ExFreePoolWithTag(Page, TAG_KERNEL);
            return;
        }
    }

    for (i = 0; i < MAXIMUM_PROCESSORS; i++)
        KiHvInputGpa[i] = MmGetPhysicalAddress(KiHvInput[i]).QuadPart;

    KiHvHypercallPage = Page;
    KeMemoryBarrier();
    KiHvEnlightenments = Enlightenments;

    DPRINT1("Hypervisor enlightenments: remote TLB flush %s, cluster IPI %s, Ex processor sets %s\n",
            (Enlightenments & KI_HV_REMOTE_FLUSH) ? "yes" : "no",
            (Enlightenments & KI_HV_CLUSTER_IPI) ? "yes" : "no",
            (Enlightenments & KI_HV_EX_PROCESSOR_SETS) ? "yes" : "no");
}

/*
 * Flushes the TLB of the target processors in every address space (9.4):
 * the range at Address, or with Address NULL every mapping, or only the
 * non-global ones. The call returns once the flushes took effect. The
 * caller stays on the current processor; the target set must not contain
 * it. Returns FALSE when the caller must flush with IPIs.
 */
BOOLEAN
NTAPI
KiHvFlushTb(
    _In_ KAFFINITY TargetSet,
    _In_ BOOLEAN NonGlobalOnly,
    _In_opt_ PVOID Address,
    _In_ ULONG NumberOfPages)
{
    PULONG64 Input;
    ULONG64 Control, Mask, Result;
    ULONG Processor, Banks, Gva;
    BOOLEAN Enable;

    if (!(KiHvEnlightenments & KI_HV_REMOTE_FLUSH))
        return FALSE;

    ASSERT(TargetSet != 0);
    ASSERT(!Address || !NonGlobalOnly);
    ASSERT(!Address || ((NumberOfPages >= 1) && (NumberOfPages <= HV_GVA_RANGE_MAX_ADDITIONAL_PAGES + 1)));

    Input = KiHvAcquireInput(&Processor, &Enable);
    if (!Input)
        return FALSE;

    Input[0] = 0;
    Input[1] = HV_FLUSH_ALL_VIRTUAL_ADDRESS_SPACES;
    if (NonGlobalOnly)
        Input[1] |= HV_FLUSH_NON_GLOBAL_MAPPINGS_ONLY;
    if (KiHvGetVpMask(TargetSet, &Mask))
    {
        Input[2] = Mask;
        Gva = 3;
        Control = Address ? HvCallFlushVirtualAddressList : HvCallFlushVirtualAddressSpace;
    }
    else if ((KiHvEnlightenments & KI_HV_EX_PROCESSOR_SETS) &&
             ((Banks = KiHvBuildVpSet(TargetSet, &Input[2])) != 0))
    {
        /* The banks are the variable part of the header (3.7.1) */
        Gva = 4 + Banks;
        Control = (Address ? HvCallFlushVirtualAddressListEx : HvCallFlushVirtualAddressSpaceEx) |
                  ((ULONG64)Banks << HV_HYPERCALL_VARIABLE_HEADER_SHIFT);
    }
    else
    {
        KiHvReleaseInput(Processor, Enable);
        return FALSE;
    }

    if (Address)
    {
        /* One range: the low 12 bits count the pages after the first */
        Input[Gva] = (ULONG64)(ULONG_PTR)PAGE_ALIGN(Address) | (NumberOfPages - 1);
        Control |= 1ULL << HV_HYPERCALL_REP_COUNT_SHIFT;
    }

    Result = KiHvHypercall(Control, KiHvInputGpa[Processor], 0);
    KiHvReleaseInput(Processor, Enable);

    return KiHvCheckResult(Result, KI_HV_REMOTE_FLUSH);
}

#endif /* CONFIG_SMP */
