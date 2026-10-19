/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite KeGenericCallDpc test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

typedef struct _CALL_CONTEXT
{
    LONG Calls[MAXIMUM_PROCESSORS];
    LONG WrongIrql;
    LONG Winners[2];
    LONG InPhase;
    LONG PhaseViolations;
    LONG Expected;
    LONG Done;
    PKTHREAD Caller;
} CALL_CONTEXT, *PCALL_CONTEXT;

static
VOID
NTAPI
CallRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PCALL_CONTEXT Context = DeferredContext;
    ULONG Processor = KeGetCurrentProcessorNumber();

    UNREFERENCED_PARAMETER(Dpc);

    if (KeGetCurrentIrql() != DISPATCH_LEVEL)
        InterlockedIncrement(&Context->WrongIrql);
    if (Processor < MAXIMUM_PROCESSORS)
        InterlockedIncrement(&Context->Calls[Processor]);

    /* Nobody leaves the first barrier before everybody arrived */
    InterlockedIncrement(&Context->InPhase);
    if (KeSignalCallDpcSynchronize(SystemArgument2))
        InterlockedIncrement(&Context->Winners[0]);
    if (Context->InPhase != Context->Expected)
        InterlockedIncrement(&Context->PhaseViolations);

    if (KeSignalCallDpcSynchronize(SystemArgument2))
        InterlockedIncrement(&Context->Winners[1]);

    /* The other processors finish late, so a caller that does not wait for
       every processor to be done returns before they count themselves */
    if (KeGetCurrentThread() != Context->Caller)
        KeStallExecutionProcessor(200);
    InterlockedIncrement(&Context->Done);

    KeSignalCallDpcDone(SystemArgument1);
}

START_TEST(KeGenericCallDpc)
{
    PCALL_CONTEXT Context;
    KAFFINITY Active, Set;
    ULONG Round, Processor, Missed, Extra, Wait;
    LONG Count;

    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Context), 'tseT');
    if (skip(Context != NULL, "No memory\n"))
        return;

    Active = KeQueryActiveProcessors();
    for (Count = 0, Set = Active; Set; Set &= Set - 1) Count++;
    trace("%u processors, active mask %Ix\n", KeNumberProcessors, Active);

    for (Round = 0; Round < 200; Round++)
    {
        RtlZeroMemory(Context, sizeof(*Context));
        Context->Expected = Count;
        Context->Caller = KeGetCurrentThread();
        KeGenericCallDpc(CallRoutine, Context);

        /* Every processor counted itself before KeGenericCallDpc returned */
        ok(Context->Done == Count, "Round %lu: %ld of %ld processors done at the return\n",
           Round, Context->Done, Count);

        Missed = Extra = 0;
        for (Processor = 0; Processor < MAXIMUM_PROCESSORS; Processor++)
        {
            LONG Expected = (Active & AFFINITY_MASK(Processor)) ? 1 : 0;
            if (Context->Calls[Processor] < Expected) Missed++;
            if (Context->Calls[Processor] > Expected) Extra++;
        }

        ok(Missed == 0, "Round %lu: %lu processors did not run the routine\n", Round, Missed);
        ok(Extra == 0, "Round %lu: %lu processors ran it more than once\n", Round, Extra);
        ok_eq_long(Context->WrongIrql, 0);
        ok_eq_long(Context->Winners[0], 1);
        ok_eq_long(Context->Winners[1], 1);
        ok_eq_long(Context->PhaseViolations, 0);
        if (Missed || Extra || Context->WrongIrql || Context->PhaseViolations ||
            (Context->Winners[0] != 1) || (Context->Winners[1] != 1) ||
            (Context->Done != Count))
        {
            break;
        }
    }

    /* After a failure, processors may still use the context: wait for them,
       and leave it allocated if they do not finish */
    for (Wait = 0; (Wait < 1000) && (*(volatile LONG *)&Context->Done < Count); Wait++)
        KeStallExecutionProcessor(1000);

    if (*(volatile LONG *)&Context->Done >= Count)
        ExFreePoolWithTag(Context, 'tseT');
    else
        ok(FALSE, "Processors still use the context, leaking it\n");
}
