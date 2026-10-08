/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Timer and APIC enlightenments of a Microsoft-compatible hypervisor
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 * REFERENCES:  Microsoft, "Hypervisor Top Level Functional Specification", v6.0b
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* What the hypervisor grants, offers and recommends (CPUID 0x40000003, 0x40000004) */
static ULONG HalpHvPrivileges;
static ULONG HalpHvFeatures;
static ULONG HalpHvRecommendations;
static BOOLEAN HalpHvSuppressed;

/* The reference TSC page, when the performance counter uses it */
PHV_REFERENCE_TSC_PAGE HalpHvReferenceTscPage;
static BOOLEAN HalpHvReferenceTscInvalid;

/* One VP assist page per processor for EOI assist (7.8.7, 10.3) */
static PUCHAR HalpHvAssistPages;
static ULONG64 HalpHvAssistPagesPhysical;
static ULONG HalpHvAssistPageCount;

#ifdef CONFIG_SMP
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;
#define HalpHvProcessorCount() min(HalpApicInfoTable.ProcessorCount, MAXIMUM_PROCESSORS)
#else
#define HalpHvProcessorCount() 1
#endif

/* PRIVATE FUNCTIONS **********************************************************/

static
BOOLEAN
HalpHvDetect(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    INT CpuInfo[4];

    /* Hypervisor present bit, then the interface signature (2.2, 2.4) */
    __cpuid(CpuInfo, 1);
    if (!(CpuInfo[2] & (1UL << HV_CPUID_HYPERVISOR_PRESENT_BIT)))
        return FALSE;

    __cpuid(CpuInfo, HV_CPUID_VENDOR_AND_MAX_FUNCTIONS);
    if ((ULONG)CpuInfo[0] < HV_CPUID_IMPLEMENTATION_LIMITS)
        return FALSE;

    __cpuid(CpuInfo, HV_CPUID_INTERFACE);
    if ((ULONG)CpuInfo[0] != HV_INTERFACE_SIGNATURE_HV1)
        return FALSE;

    if (LoaderBlock->LoadOptions && strstr(LoaderBlock->LoadOptions, "NOHVENLIGHT"))
    {
        HalpHvSuppressed = TRUE;
        return FALSE;
    }

    __cpuid(CpuInfo, HV_CPUID_FEATURES);
    HalpHvPrivileges = (ULONG)CpuInfo[0];
    HalpHvFeatures = (ULONG)CpuInfo[3];

    __cpuid(CpuInfo, HV_CPUID_ENLIGHTENMENT_INFO);
    HalpHvRecommendations = (ULONG)CpuInfo[0];
    return TRUE;
}

/*
 * Takes pages of RAM for the hypervisor's overlay pages from the top of a
 * free loader block between 1 MB and 4 GB, and removes them from the
 * memory map so that nothing else uses them. HalpAllocPhysicalMemory takes
 * the lowest free memory, which the application processor startup code
 * needs later.
 */
static
CODE_SEG("INIT")
ULONG64
HalpHvAllocatePages(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PFN_COUNT PageCount)
{
    PLIST_ENTRY NextEntry;
    PMEMORY_ALLOCATION_DESCRIPTOR MdBlock;

    for (NextEntry = LoaderBlock->MemoryDescriptorListHead.Flink;
         NextEntry != &LoaderBlock->MemoryDescriptorListHead;
         NextEntry = NextEntry->Flink)
    {
        MdBlock = CONTAINING_RECORD(NextEntry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        if ((MdBlock->MemoryType == LoaderFree) &&
            (MdBlock->BasePage >= (0x100000 >> PAGE_SHIFT)) &&
            (MdBlock->PageCount > PageCount) &&
            ((ULONG64)MdBlock->BasePage + MdBlock->PageCount <= (0x100000000ULL >> PAGE_SHIFT)))
        {
            MdBlock->PageCount -= PageCount;
            return ((ULONG64)MdBlock->BasePage + MdBlock->PageCount) << PAGE_SHIFT;
        }
    }

    return 0;
}

/* The high 64 bits of a 64 x 64 bit product */
FORCEINLINE
ULONG64
HalpMultiplyHigh(
    _In_ ULONG64 A,
    _In_ ULONG64 B)
{
#ifdef _M_AMD64
    return __umulh(A, B);
#else
    ULONG64 LowLow = __emulu((ULONG)A, (ULONG)B);
    ULONG64 LowHigh = __emulu((ULONG)A, (ULONG)(B >> 32));
    ULONG64 HighLow = __emulu((ULONG)(A >> 32), (ULONG)B);
    ULONG64 HighHigh = __emulu((ULONG)(A >> 32), (ULONG)(B >> 32));
    ULONG64 Middle = (LowLow >> 32) + (ULONG)LowHigh + (ULONG)HighLow;

    return HighHigh + (LowHigh >> 32) + (HighLow >> 32) + (Middle >> 32);
#endif
}

/* FUNCTIONS ******************************************************************/

/*
 * Runs on the boot processor before anything reads the performance counter:
 * the counter changes its frequency when it moves to the reference time.
 */
CODE_SEG("INIT")
VOID
NTAPI
HalpHvInitialize(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PHYSICAL_ADDRESS PhysicalAddress;
    PHV_REFERENCE_TSC_PAGE Page;
    ULONG64 Value;

    if (!HalpHvDetect(LoaderBlock))
        return;

    /* The reference TSC page (12.7) needs the reference counter (12.4) for
       the times its sequence is 0 */
    if ((HalpHvPrivileges & HV_ACCESS_PARTITION_REFERENCE_TSC) &&
        (HalpHvPrivileges & HV_ACCESS_PARTITION_REFERENCE_COUNTER))
    {
        /* A page of RAM used for nothing else; the hypervisor overlays it */
        PhysicalAddress.QuadPart = HalpHvAllocatePages(LoaderBlock, 1);
        Page = PhysicalAddress.QuadPart ? HalpMapPhysicalMemory64(PhysicalAddress, 1) : NULL;
        if (Page)
        {
            RtlZeroMemory(Page, PAGE_SIZE);
            Value = __readmsr(HV_X64_MSR_REFERENCE_TSC);
            __writemsr(HV_X64_MSR_REFERENCE_TSC,
                       (Value & HV_X64_MSR_PAGE_RESERVED_MASK) |
                       (ULONG64)PhysicalAddress.QuadPart |
                       HV_X64_MSR_REFERENCE_TSC_ENABLE);

            /* A sequence of 0 means the page is not usable: then every read
               would trap to the counter MSR, slower than the plain TSC */
            if (Page->TscSequence != 0)
            {
                HalpHvReferenceTscPage = Page;
            }
            else
            {
                __writemsr(HV_X64_MSR_REFERENCE_TSC, Value & HV_X64_MSR_PAGE_RESERVED_MASK);
                HalpHvReferenceTscInvalid = TRUE;
            }
        }
    }

    /* The VP assist pages are RAM only the hypervisor and their processor use */
    if (HalpHvPrivileges & HV_ACCESS_INTR_CTRL_REGS)
    {
        ULONG Count = HalpHvProcessorCount();
        PVOID Pages;

        PhysicalAddress.QuadPart = HalpHvAllocatePages(LoaderBlock, Count);
        Pages = PhysicalAddress.QuadPart ? HalpMapPhysicalMemory64(PhysicalAddress, Count) : NULL;
        if (Pages)
        {
            RtlZeroMemory(Pages, Count * PAGE_SIZE);
            HalpHvAssistPages = Pages;
            HalpHvAssistPagesPhysical = PhysicalAddress.QuadPart;
            HalpHvAssistPageCount = Count;
        }
    }
}

/* Reports the enlightenments once the debugger is up */
CODE_SEG("INIT")
VOID
NTAPI
HalpHvReport(VOID)
{
    if (HalpHvSuppressed)
        DPRINT1("Hypervisor enlightenments disabled by NOHVENLIGHT\n");
    if (!HalpHvPrivileges)
        return;

    DPRINT1("Hypervisor interface Hv#1: privileges 0x%lx, features 0x%lx, recommendations 0x%lx\n",
            HalpHvPrivileges, HalpHvFeatures, HalpHvRecommendations);
    if (HalpHvReferenceTscInvalid)
        DPRINT1("The reference TSC page is not valid, keeping the TSC\n");
    DPRINT1("Hypervisor enlightenments (HAL): reference time %s, EOI assist %s\n",
            HalpHvReferenceTscPage ? "yes" : "no",
            HalpHvAssistPages ? "yes" : "no");
}

/*
 * Enables EOI assist on this processor after its local APIC is set up, and
 * publishes the VP assist page to the EOI paths.
 */
VOID
NTAPI
HalpHvInitializeProcessor(
    _In_ ULONG ProcessorNumber)
{
    PVOID Page = NULL;
    ULONG64 Value;

    if (HalpHvAssistPages && (ProcessorNumber < HalpHvAssistPageCount))
    {
        Value = __readmsr(HV_X64_MSR_VP_ASSIST_PAGE);
        __writemsr(HV_X64_MSR_VP_ASSIST_PAGE,
                   (Value & HV_X64_MSR_PAGE_RESERVED_MASK) |
                   (HalpHvAssistPagesPhysical + (ULONG64)ProcessorNumber * PAGE_SIZE) |
                   HV_X64_MSR_VP_ASSIST_PAGE_ENABLE);
        if (__readmsr(HV_X64_MSR_VP_ASSIST_PAGE) & HV_X64_MSR_VP_ASSIST_PAGE_ENABLE)
            Page = HalpHvAssistPages + (ULONG_PTR)ProcessorNumber * PAGE_SIZE;
    }

#ifdef _M_AMD64
    __writegsqword(FIELD_OFFSET(KPCR, HalReserved[HAL_EOI_ASSIST_PAGE]), (ULONG64)Page);
#else
    __writefsdword(FIELD_OFFSET(KPCR, HalReserved[HAL_EOI_ASSIST_PAGE]), (ULONG)Page);
#endif
}

/*
 * The partition reference time in 100 ns units (12.7.3), from the TSC and
 * the scale and offset of the reference TSC page.
 */
ULONG64
NTAPI
HalpHvReadReferenceTime(VOID)
{
    PHV_REFERENCE_TSC_PAGE Page = HalpHvReferenceTscPage;
    ULONG Sequence;
    ULONG64 Tsc, Scale;
    LONG64 Offset;

    do
    {
        Sequence = Page->TscSequence;
        if (Sequence == 0)
        {
            /* Not valid at the moment, e.g. during a migration */
            return __readmsr(HV_X64_MSR_TIME_REF_COUNT);
        }

        Tsc = __rdtsc();
        Scale = Page->TscScale;
        Offset = Page->TscOffset;
    } while (Page->TscSequence != Sequence);

    return HalpMultiplyHigh(Tsc, Scale) + Offset;
}

/*
 * The TSC frequency the hypervisor reports, when it does. Without the
 * frequency MSR, measures the TSC against the reference time.
 */
CODE_SEG("INIT")
BOOLEAN
NTAPI
HalpHvGetTscFrequency(
    _Out_ PULONG64 Frequency)
{
    ULONG64 StartTime, StartTsc, EndTime, EndTsc;

    if ((HalpHvPrivileges & HV_ACCESS_FREQUENCY_REGS) &&
        (HalpHvFeatures & HV_FEATURE_FREQUENCY_REGS))
    {
        *Frequency = __readmsr(HV_X64_MSR_TSC_FREQUENCY);
        if (*Frequency)
            return TRUE;
    }

    if (!HalpHvReferenceTscPage)
        return FALSE;

    /* 10 ms of reference time */
    StartTime = HalpHvReadReferenceTime();
    StartTsc = __rdtsc();
    do
    {
        EndTime = HalpHvReadReferenceTime();
    } while ((EndTime - StartTime) < (HV_REFERENCE_TIME_FREQUENCY / 100));
    EndTsc = __rdtsc();

    *Frequency = (EndTsc - StartTsc) * HV_REFERENCE_TIME_FREQUENCY / (EndTime - StartTime);
    return (*Frequency != 0);
}
