/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

#define AP_GDT_ENTRIES 128

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY64 Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY64 Gdt[AP_GDT_ENTRIES];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS64 Tss;
} APINFO, *PAPINFO;

typedef struct _APSTACKS
{
    PVOID KernelStack;
    PVOID DpcStack;
    PVOID DoubleFaultStack;
    PVOID NmiStack;
} APSTACKS, *PAPSTACKS;

VOID
KiInitializeProcessorBootStructures(
    _In_ ULONG ProcessorNumber,
    _Out_ PKIPCR Pcr,
    _In_ PKGDTENTRY64 GdtBase,
    _In_ PKIDTENTRY64 IdtBase,
    _In_ PKTSS64 TssBase,
    _In_ PKTHREAD IdleThread,
    _In_ PVOID KernelStack,
    _In_ PVOID DpcStack,
    _In_ PVOID DoubleFaultStack,
    _In_ PVOID NmiStack);

/* FUNCTIONS *****************************************************************/

static
VOID
KiFreeApStacks(
    _Inout_ PAPSTACKS Stacks)
{
    if (Stacks->KernelStack)
        MmDeleteKernelStack(Stacks->KernelStack, FALSE);
    if (Stacks->DpcStack)
        MmDeleteKernelStack(Stacks->DpcStack, FALSE);
    if (Stacks->DoubleFaultStack)
        MmDeleteKernelStack(Stacks->DoubleFaultStack, FALSE);
    if (Stacks->NmiStack)
        MmDeleteKernelStack(Stacks->NmiStack, FALSE);
    RtlZeroMemory(Stacks, sizeof(*Stacks));
}

static
BOOLEAN
KiAllocateApStacks(
    _Out_ PAPSTACKS Stacks)
{
    RtlZeroMemory(Stacks, sizeof(*Stacks));

    Stacks->KernelStack = MmCreateKernelStack(FALSE, 0);
    Stacks->DpcStack = MmCreateKernelStack(FALSE, 0);
    Stacks->DoubleFaultStack = MmCreateKernelStack(FALSE, 0);
    Stacks->NmiStack = MmCreateKernelStack(FALSE, 0);

    if (!Stacks->KernelStack || !Stacks->DpcStack ||
        !Stacks->DoubleFaultStack || !Stacks->NmiStack)
    {
        KiFreeApStacks(Stacks);
        return FALSE;
    }

    return TRUE;
}

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PAPINFO APInfo = NULL;
    APSTACKS Stacks = {0};
    PKPROCESSOR_STATE ProcessorState;
    KDESCRIPTOR BspGdt, BspIdt;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;
    ULONG64 WaitStart;

    MaximumProcessors = KeMaximumProcessors;

    /* Limit the number of processors we can start at run-time */
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    /* Limit also the number of processors we can start during boot-time */
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    /* Application processors start with copies of the boot processor's tables */
    __sgdt(&BspGdt.Limit);
    __sidt(&BspIdt.Limit);
    if ((BspGdt.Limit + 1u > sizeof(APInfo->Gdt)) ||
        (BspIdt.Limit + 1u > sizeof(APInfo->Idt)))
    {
        DPRINT1("KeStartAllProcessors: descriptor tables too large (GDT %u, IDT %u)\n",
                BspGdt.Limit + 1, BspIdt.Limit + 1);
        return;
    }

    /* Start ProcessorCount at 1 because we already have the boot CPU */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        /* Allocate structures for a new CPU */
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
            break;
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        if (!KiAllocateApStacks(&Stacks))
            break;

        /* Prepare descriptor tables before the TSS descriptor is set up in them */
        RtlCopyMemory(APInfo->Gdt, BspGdt.Base, BspGdt.Limit + 1);
        RtlCopyMemory(APInfo->Idt, BspIdt.Base, BspIdt.Limit + 1);

        /* Initialize the PCR, PRCB and TSS (the TSS descriptor is marked available) */
        KiInitializeProcessorBootStructures(ProcessorCount,
                                            &APInfo->Pcr,
                                            APInfo->Gdt,
                                            APInfo->Idt,
                                            &APInfo->Tss,
                                            &APInfo->Thread.Tcb,
                                            Stacks.KernelStack,
                                            Stacks.DpcStack,
                                            Stacks.DoubleFaultStack,
                                            Stacks.NmiStack);

        /* Fill the processor state the startup code loads */
        ProcessorState = &APInfo->Pcr.Prcb.ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();
        ProcessorState->SpecialRegisters.MsrGsBase = (ULONG64)&APInfo->Pcr;

        ProcessorState->SpecialRegisters.Gdtr.Base = APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = BspGdt.Limit;
        ProcessorState->SpecialRegisters.Idtr.Base = APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = BspIdt.Limit;
        ProcessorState->SpecialRegisters.Tr = KGDT64_SYS_TSS;

        ProcessorState->ContextFrame.SegCs = KGDT64_R0_CODE;
        ProcessorState->ContextFrame.SegSs = KGDT64_R0_DATA;
        ProcessorState->ContextFrame.SegDs = KGDT64_R3_DATA | RPL_MASK;
        ProcessorState->ContextFrame.SegEs = KGDT64_R3_DATA | RPL_MASK;
        ProcessorState->ContextFrame.SegFs = KGDT64_R3_CMTEB | RPL_MASK;
        ProcessorState->ContextFrame.SegGs = KGDT64_R3_DATA | RPL_MASK;

        /* KiSystemStartup(KeLoaderBlock) on the top of the kernel stack, leaving
           room for its home space; the startup code pushes a return address */
        ProcessorState->ContextFrame.Rsp = (ULONG64)Stacks.KernelStack - 0x40;
        ProcessorState->ContextFrame.Rip = (ULONG64)KiSystemStartup;
        ProcessorState->ContextFrame.Rcx = (ULONG64)KeLoaderBlock;

        /* Update the LOADER_PARAMETER_BLOCK structure for the new processor */
        KeLoaderBlock->KernelStack = (ULONG_PTR)Stacks.KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.Prcb;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;
        KeMemoryBarrier();

        /* Start the CPU */
        DPRINT("Attempting to Start a CPU with number: %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
            break;

        /* And wait for it to start; it clears the PRCB once it is in its idle loop */
        WaitStart = __rdtsc();
        while (*(volatile ULONG_PTR*)&KeLoaderBlock->Prcb != 0)
        {
            /* Report a processor that does not come up, roughly after 10^10 cycles */
            if (WaitStart && (__rdtsc() - WaitStart > 10000000000ULL))
            {
                DPRINT1("KeStartAllProcessors: still waiting for processor %lu\n", ProcessorCount);
                WaitStart = 0;
            }
            YieldProcessor();
        }

        /* These now belong to the running processor */
        APInfo = NULL;
        RtlZeroMemory(&Stacks, sizeof(Stacks));
    }

    ProcessorCount--;

    /* Free what was prepared for a processor that did not start */
    if (APInfo)
        ExFreePoolWithTag(APInfo, TAG_KERNEL);
    KiFreeApStacks(&Stacks);

    DPRINT1("KeStartAllProcessors: Successful AP startup count is %lu\n", ProcessorCount);
}
