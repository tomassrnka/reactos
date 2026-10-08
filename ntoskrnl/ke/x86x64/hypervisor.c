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

#endif /* CONFIG_SMP */
