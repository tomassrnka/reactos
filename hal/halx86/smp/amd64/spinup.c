/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup setup
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern BOOLEAN HalpOnlyBootProcessor;
extern PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
extern PVOID HalpLowStub;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* The startup code, copied into the low stub (apentry.S) */
extern UCHAR HalpAPEntry16[];
extern UCHAR HalpAPEntryData[];
extern UCHAR HalpAPEntryJump64[];
extern UCHAR HalpAPEntryLow64[];
extern UCHAR HalpAPEntry16End[];
VOID HalpAPEntry64(VOID);

ULONG HalpStartedProcessorCount = 1;

#include <pshpack1.h>
/* Must match HalpAPEntryData in apentry.S */
typedef struct _AP_ENTRY_DATA
{
    ULONG64 Gdt[3];
    USHORT GdtPad;
    USHORT GdtLimit;
    ULONG GdtBase;
    ULONG Cr3;
    ULONG Cr4;
    ULONG64 Efer;
    PKPROCESSOR_STATE ProcessorState;
    PVOID Entry64;
} AP_ENTRY_DATA, *PAP_ENTRY_DATA;
#include <poppack.h>

C_ASSERT(sizeof(AP_ENTRY_DATA) == 64);

/* Pages of the low stub */
#define AP_STUB_CODE_PAGE   0
#define AP_STUB_PML4_PAGE   1
#define AP_STUB_PDPT_PAGE   2
#define AP_STUB_PD_PAGE     3

C_ASSERT(AP_STUB_PD_PAGE < HALP_LOW_STUB_SIZE_IN_PAGES);

#define AP_PTE_PRESENT      0x001ULL
#define AP_PTE_WRITE        0x002ULL
#define AP_PTE_LARGE        0x080ULL

/* FUNCTIONS *****************************************************************/

/*
 * The processor starts in real mode below 1 MB and enables paging before it
 * can reach the kernel. Its first page tables live in the stub: a copy of the
 * current top level table, which shares the kernel's upper half, whose first
 * entry maps the first 2 MB (and so the stub) one to one.
 */
static
ULONG
HalpSetupTemporaryMappings(VOID)
{
    PULONG64 Pml4 = (PULONG64)((PUCHAR)HalpLowStub + AP_STUB_PML4_PAGE * PAGE_SIZE);
    PULONG64 Pdpt = (PULONG64)((PUCHAR)HalpLowStub + AP_STUB_PDPT_PAGE * PAGE_SIZE);
    PULONG64 Pd = (PULONG64)((PUCHAR)HalpLowStub + AP_STUB_PD_PAGE * PAGE_SIZE);
    ULONG64 StubPhysical = HalpLowStubPhysicalAddress.QuadPart;

    /* The stub must be reachable from real mode and covered by the first 2 MB */
    if ((StubPhysical == 0) ||
        (StubPhysical + HALP_LOW_STUB_SIZE_IN_PAGES * PAGE_SIZE > 0x100000))
    {
        return 0;
    }

    RtlCopyMemory(Pml4, MiAddressToPxe(NULL), PAGE_SIZE);
    RtlZeroMemory(Pdpt, PAGE_SIZE);
    RtlZeroMemory(Pd, PAGE_SIZE);

    Pml4[0] = (StubPhysical + AP_STUB_PDPT_PAGE * PAGE_SIZE) | AP_PTE_PRESENT | AP_PTE_WRITE;
    Pdpt[0] = (StubPhysical + AP_STUB_PD_PAGE * PAGE_SIZE) | AP_PTE_PRESENT | AP_PTE_WRITE;
    Pd[0] = 0 | AP_PTE_PRESENT | AP_PTE_WRITE | AP_PTE_LARGE;

    return (ULONG)(StubPhysical + AP_STUB_PML4_PAGE * PAGE_SIZE);
}

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    PAP_ENTRY_DATA ApEntryData;
    ULONG StubPhysical;
    ULONG InitialCr3;

    /* Bail out if we only use the boot CPU */
    if (HalpOnlyBootProcessor)
        return FALSE;

    /* Bail out if we have started all available CPUs */
    if (HalpStartedProcessorCount == HalpApicInfoTable.ProcessorCount)
        return FALSE;

    /* The startup code must fit in the first page of the stub */
    ASSERT((ULONG_PTR)(HalpAPEntry16End - HalpAPEntry16) <= PAGE_SIZE);

    InitialCr3 = HalpSetupTemporaryMappings();
    if (!InitialCr3)
        return FALSE;
    StubPhysical = HalpLowStubPhysicalAddress.LowPart;

    /* Put the startup code into low memory */
    RtlCopyMemory(HalpLowStub, HalpAPEntry16, HalpAPEntry16End - HalpAPEntry16);

    /* Patch the far jump to the 64-bit code in the stub */
    *(PULONG)((PUCHAR)HalpLowStub + (HalpAPEntryJump64 - HalpAPEntry16)) =
        StubPhysical + (ULONG)(HalpAPEntryLow64 - HalpAPEntry16);

    /* Fill in the data the startup code uses */
    ApEntryData = (PAP_ENTRY_DATA)((PUCHAR)HalpLowStub + (HalpAPEntryData - HalpAPEntry16));
    ApEntryData->GdtLimit = sizeof(ApEntryData->Gdt) - 1;
    ApEntryData->GdtBase = StubPhysical + (ULONG)(HalpAPEntryData - HalpAPEntry16) +
                           FIELD_OFFSET(AP_ENTRY_DATA, Gdt);
    ApEntryData->Cr3 = InitialCr3;
    ApEntryData->Cr4 = CR4_PAE;
    ApEntryData->Efer = (__readmsr(MSR_EFER) & ~MSR_LMA) | MSR_LME;
    ApEntryData->ProcessorState = ProcessorState;
    ApEntryData->Entry64 = (PVOID)HalpAPEntry64;

    /* The stub must be complete before the processor starts */
    KeMemoryBarrier();

    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);

    HalpStartedProcessorCount++;

    return TRUE;
}
