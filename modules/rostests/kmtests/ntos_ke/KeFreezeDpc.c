/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite tests for the debugger freezing the other processors
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/*
 * These tests enter the kernel debugger from a DPC on another processor than the one that
 * runs the test, which spins at DISPATCH_LEVEL meanwhile: the debugger freezes that
 * processor, and a bug check freezes it as well. They need a kernel debugger and somebody who
 * drives it (continue, or inject NMIs while the session is open), so they are hidden from the
 * test list and run only when named, on a system with at least two processors:
 *   KeFreezeBreakDpc:    DbgBreakPoint in the DPC (on one processor, from the test thread); after the debugger continues, the test
 *                        checks that the DPC got back with the IRQL it had
 *   KeFreezeBugCheckDpc: KeBugCheckEx in the DPC (does not come back). A bug check callback
 *                        prints through DbgPrint, which enters the debugger from inside the
 *                        freeze of the bug check, and checks that a processor that spins in
 *                        a third DPC stays frozen (log line "KeFreezeDpc: nested ...").
 * The DPCs log through DbgPrint, "KeFreezeDpc: ..." lines.
 */

#define WAIT_SECONDS 900

typedef struct _FREEZE_DPC_CONTEXT
{
    KDPC Dpc;
    BOOLEAN BugCheck;
    volatile LONG Done;
    KIRQL IrqlBefore;
    KIRQL IrqlAfter;
    ULONG Processor;
} FREEZE_DPC_CONTEXT, *PFREEZE_DPC_CONTEXT;

static volatile LONG SpinCounter;
static volatile LONG SpinStop;
static KDPC SpinDpc;
static KBUGCHECK_CALLBACK_RECORD CallbackRecord;
static UCHAR CallbackBuffer[16];

static
VOID
NTAPI
SpinDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    while (!SpinStop)
        InterlockedIncrement(&SpinCounter);
}

static
VOID
NTAPI
NestedBugCheckCallback(
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    LONG Before, After;

    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    /* DbgPrint enters the debugger, which is entered by the bug check already */
    Before = SpinCounter;
    DbgPrint("KeFreezeDpc: nested debugger entry at IRQL %u\n", KeGetCurrentIrql());
    After = SpinCounter;
    DbgPrint("KeFreezeDpc: nested %s, IRQL %u\n",
             (Before == After) ? "FROZEN: the spinning processor did not run" : "RAN: the others were thawed",
             KeGetCurrentIrql());
}

static
VOID
NTAPI
FreezeDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PFREEZE_DPC_CONTEXT Context = DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    Context->Processor = KeGetCurrentProcessorNumber();
    Context->IrqlBefore = KeGetCurrentIrql();
    DbgPrint("KeFreezeDpc: CPU %lu at IRQL %u enters the debugger%s\n",
             Context->Processor, Context->IrqlBefore, Context->BugCheck ? " by a bug check" : "");

    if (Context->BugCheck)
        KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);

    DbgBreakPoint();

    Context->IrqlAfter = KeGetCurrentIrql();
    DbgPrint("KeFreezeDpc: CPU %lu is back at IRQL %u\n", Context->Processor, Context->IrqlAfter);
    InterlockedExchange(&Context->Done, 1);
}

static
VOID
RunFreezeDpc(
    _In_ BOOLEAN BugCheck)
{
    static FREEZE_DPC_CONTEXT Context;
    KIRQL OldIrql;
    ULONG Waited;

    if (KeNumberProcessors < 2 && BugCheck)
    {
        skip(FALSE, "Needs at least two processors\n");
        return;
    }

    RtlZeroMemory(&Context, sizeof(Context));
    Context.BugCheck = BugCheck;
    KeInitializeDpc(&Context.Dpc, FreezeDpcRoutine, &Context);

    /* Spin here at DISPATCH_LEVEL, and let the DPC run on another processor */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    if (KeNumberProcessors < 2)
    {
        /* One processor: break in from here, at DISPATCH_LEVEL, and check the IRQL after it */
        FreezeDpcRoutine(&Context.Dpc, &Context, NULL, NULL);
        KeLowerIrql(OldIrql);
        ok(Context.Done == 1, "The break did not come back\n");
        ok_eq_uint(Context.IrqlAfter, Context.IrqlBefore);
        return;
    }
    if (BugCheck && KeNumberProcessors > 2)
    {
        KeInitializeDpc(&SpinDpc, SpinDpcRoutine, NULL);
        KeSetTargetProcessorDpc(&SpinDpc, (CCHAR)((KeGetCurrentProcessorNumber() + 2) % KeNumberProcessors));
        KeRegisterBugCheckCallback(&CallbackRecord, NestedBugCheckCallback, CallbackBuffer,
                                   sizeof(CallbackBuffer), (PUCHAR)"KmtTest");
        KeInsertQueueDpc(&SpinDpc, NULL, NULL);
        KeStallExecutionProcessor(100000);
    }
    KeSetTargetProcessorDpc(&Context.Dpc, (CCHAR)((KeGetCurrentProcessorNumber() + 1) % KeNumberProcessors));
    KeInsertQueueDpc(&Context.Dpc, NULL, NULL);
    for (Waited = 0; Waited < WAIT_SECONDS * 1000 && !Context.Done; Waited++)
        KeStallExecutionProcessor(1000);
    KeLowerIrql(OldIrql);

    ok(Context.Done == 1, "The DPC did not come back from the debugger\n");
    ok_irql(OldIrql);
    if (Context.Done)
        ok_eq_uint(Context.IrqlAfter, Context.IrqlBefore);
}

START_TEST(KeFreezeBreakDpc)
{
    RunFreezeDpc(FALSE);
}

START_TEST(KeFreezeBugCheckDpc)
{
    RunFreezeDpc(TRUE);
}
