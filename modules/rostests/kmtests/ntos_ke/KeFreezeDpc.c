/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite tests for the debugger freezing the other processors
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include <ndk/kdfuncs.h>

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
 *   KeFreezeNmiDpc:      like KeFreezeBreakDpc, and the test registers the KDBG command "knmi":
 *                        typed at the kdb prompt, it runs on the freeze owner and sends an NMI
 *                        (HalSendNMI) to every frozen processor. The NMI is held by the frozen
 *                        processor, which is inside its freeze NMI handler, and is taken right
 *                        after the thaw: a deterministic repeat of the NMI that QEMU's inject-nmi
 *                        leaves behind. The test passes if the system does not hang or stop
 *                        after "cont". A drop shows as "KE: ... held across a debugger thaw"
 *                        in the log, on the next debugger entry (the test enters it with a
 *                        DbgPrint). Needs KDBG; a real NMI (reason bits in port 0x61) cannot
 *                        be provoked in a VM, that case is not covered here.
 * The DPCs log through DbgPrint, "KeFreezeDpc: ..." lines.
 */

NTHALAPI
VOID
NTAPI
HalSendNMI(
    _In_ KAFFINITY TargetSet);

#define WAIT_SECONDS 900

typedef struct _FREEZE_DPC_CONTEXT
{
    KDPC Dpc;
    BOOLEAN BugCheck;
    BOOLEAN NmiCommand;
    volatile LONG Done;
    KIRQL IrqlBefore;
    KIRQL IrqlAfter;
    ULONG Processor;
} FREEZE_DPC_CONTEXT, *PFREEZE_DPC_CONTEXT;

static volatile LONG NmiCommandRuns;
static volatile LONG NmiCommandCalls;

static NTSTATUS
(NTAPI *pKdSystemDebugControl)(
    _In_ SYSDBG_COMMAND Command,
    _In_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Inout_ PULONG ReturnLength,
    _In_ KPROCESSOR_MODE PreviousMode);

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

    if (Context->NmiCommand)
        DbgPrint("KeFreezeDpc: type \"knmi\" at the kdb prompt, then \"cont\"\n");

    DbgBreakPoint();

    Context->IrqlAfter = KeGetCurrentIrql();
    if (Context->NmiCommand)
    {
        /* Give the NMIs that the owner sent time to arrive on the others,
           then enter the debugger once more: the thaw of that entry says
           whether NMIs were dropped */
        KeStallExecutionProcessor(200000);
        DbgPrint("KeFreezeDpc: CPU %lu: knmi ran %ld time(s), callback called %ld time(s)\n", Context->Processor, NmiCommandRuns, NmiCommandCalls);
    }
    DbgPrint("KeFreezeDpc: CPU %lu is back at IRQL %u\n", Context->Processor, Context->IrqlAfter);
    InterlockedExchange(&Context->Done, 1);
}

static
BOOLEAN
NTAPI
KnmiCliCallback(
    _In_ PCHAR Command,
    _In_ ULONG Argc,
    _In_ PCH Argv[])
{
    UNREFERENCED_PARAMETER(Command);
    InterlockedIncrement(&NmiCommandCalls);

    /* Runs on the freeze owner, in the debugger session, at HIGH_LEVEL: the
       other processors are frozen, each inside its freeze NMI handler */
    if (Argc < 1 || strcmp(Argv[0], "knmi") != 0)
        return FALSE;

    HalSendNMI(KeQueryActiveProcessors() & ~((KAFFINITY)1 << KeGetCurrentProcessorNumber()));
    InterlockedIncrement(&NmiCommandRuns);
    return TRUE;
}

static
VOID
RunFreezeDpc(
    _In_ BOOLEAN BugCheck,
    _In_ BOOLEAN NmiCommand)
{
    static FREEZE_DPC_CONTEXT Context;
    KIRQL OldIrql;
    ULONG Waited;

    if (KeNumberProcessors < 2 && (BugCheck || NmiCommand))
    {
        skip(FALSE, "Needs at least two processors\n");
        return;
    }

    if (NmiCommand)
    {
        UNICODE_STRING Name;
        NTSTATUS RegStatus;

        RtlInitUnicodeString(&Name, L"KdSystemDebugControl");
        pKdSystemDebugControl = MmGetSystemRoutineAddress(&Name);
        if (!pKdSystemDebugControl)
        {
            skip(FALSE, "KdSystemDebugControl is not available\n");
            return;
        }

        /* 'RbdK': register a KDBG command. Only a kernel with KDBG takes it: any other
           answers with an error status. A success returns the BOOLEAN of the registration
           (FALSE if the table of 10 callbacks is full), which has no status class */
        RegStatus = pKdSystemDebugControl((SYSDBG_COMMAND)'RbdK', KnmiCliCallback, FALSE, NULL, 0, NULL, KernelMode);
        if (!NT_SUCCESS(RegStatus) || RegStatus == 0)
        {
            skip(FALSE, "Needs a kernel with KDBG and a free CLI callback entry (status 0x%lx)\n", RegStatus);
            return;
        }
    }

    RtlZeroMemory(&Context, sizeof(Context));
    Context.BugCheck = BugCheck;
    Context.NmiCommand = NmiCommand;
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

    if (NmiCommand)
    {
        pKdSystemDebugControl((SYSDBG_COMMAND)'RbdK', KnmiCliCallback, TRUE, NULL, 0, NULL, KernelMode);
        DbgPrint("KeFreezeDpc: nmi test done, knmi ran %ld time(s)\n", NmiCommandRuns);
    }
}

START_TEST(KeFreezeBreakDpc)
{
    RunFreezeDpc(FALSE, FALSE);
}

START_TEST(KeFreezeBugCheckDpc)
{
    RunFreezeDpc(TRUE, FALSE);
}

START_TEST(KeFreezeNmiDpc)
{
    RunFreezeDpc(FALSE, TRUE);
}
