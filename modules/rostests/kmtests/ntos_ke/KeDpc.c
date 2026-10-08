/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite Deferred Procedure Call test
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#include <kmt_test.h>

//#define NDEBUG
#include <debug.h>

/* TODO: DPC importance */

static volatile LONG DpcCount;
static volatile UCHAR DpcImportance;

static KDEFERRED_ROUTINE DpcHandler;

static
VOID
NTAPI
DpcHandler(
    IN PRKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2)
{
    PKPRCB Prcb = KeGetCurrentPrcb();

    ok_irql(DISPATCH_LEVEL);
    InterlockedIncrement(&DpcCount);
    ok(DeferredContext == Dpc, "DeferredContext = %p, Dpc = %p, expected equal\n", DeferredContext, Dpc);
    ok_eq_pointer(SystemArgument1, (PVOID)0xabc123);
    ok_eq_pointer(SystemArgument2, (PVOID)0x5678);

    /* KDPC object contents */
    ok_eq_uint(Dpc->Type, DpcObject);
    ok_eq_uint(Dpc->Importance, DpcImportance);
    ok_eq_uint(Dpc->Number, 0);
    ok(Dpc->DpcListEntry.Blink != NULL, "\n");
    ok(Dpc->DpcListEntry.Blink != &Dpc->DpcListEntry, "\n");
    if (!skip(Dpc->DpcListEntry.Blink != NULL, "DpcListEntry.Blink == NULL\n"))
        ok_eq_pointer(Dpc->DpcListEntry.Flink, Dpc->DpcListEntry.Blink->Flink);

    ok(Dpc->DpcListEntry.Flink != NULL, "\n");
    ok(Dpc->DpcListEntry.Flink != &Dpc->DpcListEntry, "\n");
    if (!skip(Dpc->DpcListEntry.Flink != NULL, "DpcListEntry.Flink == NULL\n"))
        ok_eq_pointer(Dpc->DpcListEntry.Blink, Dpc->DpcListEntry.Flink->Blink);

    ok_eq_pointer(Dpc->DeferredRoutine, DpcHandler);
    ok_eq_pointer(Dpc->DeferredContext, DeferredContext);
    ok_eq_pointer(Dpc->SystemArgument1, SystemArgument1);
    ok_eq_pointer(Dpc->SystemArgument2, SystemArgument2);
    ok_eq_pointer(Dpc->DpcData, NULL);

    if (GetNTVersion() == _WIN32_WINNT_WS03)
    {
        ok_eq_uint(Prcb->DpcRoutineActive, 1);
        /* this DPC is not in the list anymore, but it was at the head! */
        ok_eq_pointer(Prcb->DpcData[DPC_NORMAL].DpcListHead.Flink, Dpc->DpcListEntry.Flink);
        ok_eq_pointer(Prcb->DpcData[DPC_NORMAL].DpcListHead.Blink, Dpc->DpcListEntry.Blink);
    }
}

#ifdef _M_IX86
static KEVENT IdleDpcEvent;
static volatile LONG IdleDpcSamples, IdleDpcOnDpcStack;

static
VOID
NTAPI
IdleDpcHandler(
    IN PRKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    volatile UCHAR Local[512];
    ULONG_PTR Top = (ULONG_PTR)Prcb->DpcStack;

    Local[0] = 1;
    if (KeGetCurrentThread() == Prcb->IdleThread)
    {
        /* Retired by the idle loop: this must run on the DPC stack, not on the idle thread's stack */
        InterlockedIncrement(&IdleDpcSamples);
        if (((ULONG_PTR)Local < Top) && ((ULONG_PTR)Local >= Top - KERNEL_STACK_SIZE))
            InterlockedIncrement(&IdleDpcOnDpcStack);
    }
    KeSetEvent(&IdleDpcEvent, IO_NO_INCREMENT, FALSE);
}

static
VOID
TestIdleDpcStack(VOID)
{
    KDPC Dpc;
    KTIMER Timer;
    LARGE_INTEGER DueTime, Timeout;
    ULONG i;
    NTSTATUS Status;

    /* The stack a DPC runs on is a ReactOS design choice: only check it there */
    if (*(volatile ULONG *)((ULONG_PTR)SharedUserData + 0xFFC) != 0x8EAC705)
    {
        skip(FALSE, "Not ReactOS\n");
        return;
    }

    IdleDpcSamples = IdleDpcOnDpcStack = 0;
    KeInitializeEvent(&IdleDpcEvent, SynchronizationEvent, FALSE);
    KeInitializeDpc(&Dpc, IdleDpcHandler, NULL);
    KeSetTargetProcessorDpc(&Dpc, 0);
    KeInitializeTimer(&Timer);
    /* Wait on processor 0 so that it is idle when the timer expires */
    KeSetSystemAffinityThread(1);
    for (i = 0; i < 200 && IdleDpcSamples < 10; i++)
    {
        DueTime.QuadPart = -10 * 1000 * 10;
        KeSetTimer(&Timer, DueTime, &Dpc);
        Timeout.QuadPart = -2000 * 1000 * 10;
        Status = KeWaitForSingleObject(&IdleDpcEvent, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (Status != STATUS_SUCCESS)
            break;
    }
    KeRevertToUserAffinityThread();
    KeCancelTimer(&Timer);
    KeFlushQueuedDpcs();

    trace("%ld of %lu timer DPCs were retired by the idle loop\n", IdleDpcSamples, i);
    /* Processor 0 is idle only when nothing else is ready to run there */
    if (skip(IdleDpcSamples != 0, "No DPC was retired by the idle loop in %lu tries\n", i))
        return;
    ok(IdleDpcOnDpcStack == IdleDpcSamples, "%ld of %ld DPCs retired by the idle loop ran on the DPC stack\n",
       IdleDpcOnDpcStack, IdleDpcSamples);
}
#endif

START_TEST(KeDpc)
{
    NTSTATUS Status = STATUS_SUCCESS;
    KDPC Dpc;
    KIRQL Irql, Irql2, Irql3;
    LONG ExpectedDpcCount = 0;
    BOOLEAN Ret;
    int i;

    DpcCount = 0;
    DpcImportance = MediumImportance;

#define ok_dpccount() ok(DpcCount == ExpectedDpcCount, "DpcCount = %ld, expected %ld\n", DpcCount, ExpectedDpcCount);
    trace("Dpc = %p\n", &Dpc);
    memset(&Dpc, 0x55, sizeof Dpc);
    KeInitializeDpc(&Dpc, DpcHandler, &Dpc);
    /* check the Dpc object's fields */
    ok_eq_uint(Dpc.Type, DpcObject);
    ok_eq_uint(Dpc.Importance, DpcImportance);
    ok_eq_uint(Dpc.Number, 0);
    ok_eq_pointer(Dpc.DpcListEntry.Flink, (LIST_ENTRY *)0x5555555555555555LL);
    if (Dpc.DpcListEntry.Blink)
        ok_eq_pointer(Dpc.DpcListEntry.Blink, (LIST_ENTRY *)0x5555555555555555LL);
    ok_eq_pointer(Dpc.DeferredRoutine, DpcHandler);
    ok_eq_pointer(Dpc.DeferredContext, &Dpc);
    ok_eq_pointer(Dpc.SystemArgument1, (PVOID)0x5555555555555555LL);
    ok_eq_pointer(Dpc.SystemArgument2, (PVOID)0x5555555555555555LL);
    ok_eq_pointer(Dpc.DpcData, NULL);

    if (GetNTVersion() < _WIN32_WINNT_WIN8)
    {
        // Windows 8+ is stricter about misusing DPC, these tests bugcheck there.

        /* simply run the Dpc a few times */
        for (i = 0; i < 5; ++i)
        {
            ok_dpccount();
            Ret = KeInsertQueueDpc(&Dpc, (PVOID)0xabc123, (PVOID)0x5678);
            ok_bool_true(Ret, "KeInsertQueueDpc returned");
            ++ExpectedDpcCount;
            ok_dpccount();
        }

        /* insert into queue at high irql
         * -> should only run when lowered to APC_LEVEL,
         *    inserting a second time should fail
         */
        KeRaiseIrql(APC_LEVEL, &Irql);
        for (i = 0; i < 5; ++i)
        {
            KeRaiseIrql(DISPATCH_LEVEL, &Irql2);
            ok_dpccount();
            Ret = KeInsertQueueDpc(&Dpc, (PVOID)0xabc123, (PVOID)0x5678);
            ok_bool_true(Ret, "KeInsertQueueDpc returned");
            Ret = KeInsertQueueDpc(&Dpc, (PVOID)0xdef, (PVOID)0x123);
            ok_bool_false(Ret, "KeInsertQueueDpc returned");
            ok_dpccount();
            KeRaiseIrql(HIGH_LEVEL, &Irql3);
            ok_dpccount();
            KeLowerIrql(Irql3);
            ok_dpccount();
            KeLowerIrql(Irql2);
            ++ExpectedDpcCount;
            ok_dpccount();
        }
        KeLowerIrql(Irql);

        /* now test removing from the queue */
        KeRaiseIrql(APC_LEVEL, &Irql);
        for (i = 0; i < 5; ++i)
        {
            KeRaiseIrql(DISPATCH_LEVEL, &Irql2);
            ok_dpccount();
            Ret = KeRemoveQueueDpc(&Dpc);
            ok_bool_false(Ret, "KeRemoveQueueDpc returned");
            Ret = KeInsertQueueDpc(&Dpc, (PVOID)0xabc123, (PVOID)0x5678);
            ok_bool_true(Ret, "KeInsertQueueDpc returned");
            ok_dpccount();
            KeRaiseIrql(HIGH_LEVEL, &Irql3);
            ok_dpccount();
            KeLowerIrql(Irql3);
            ok_dpccount();
            Ret = KeRemoveQueueDpc(&Dpc);
            ok_bool_true(Ret, "KeRemoveQueueDpc returned");
            KeLowerIrql(Irql2);
            ok_dpccount();
        }
        KeLowerIrql(Irql);
    }

    /* parameter checks */
    Status = STATUS_SUCCESS;
    _SEH2_TRY {
        KeInitializeDpc(&Dpc, NULL, NULL);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        Status = _SEH2_GetExceptionCode();
    } _SEH2_END;
    ok_eq_hex(Status, STATUS_SUCCESS);

    if (!skip(Status == STATUS_SUCCESS, "KeInitializeDpc failed\n") &&
        GetNTVersion() < _WIN32_WINNT_WIN8)
    {
        // Inserting NULL in a DPC gives a TIMER_OR_DPC_INVALID bugcheck on Windows 8+.
        KeRaiseIrql(HIGH_LEVEL, &Irql);
          Ret = KeInsertQueueDpc(&Dpc, NULL, NULL);
          ok_bool_true(Ret, "KeInsertQueueDpc returned");
          Ret = KeRemoveQueueDpc(&Dpc);
          ok_bool_true(Ret, "KeRemoveQueueDpc returned");
        KeLowerIrql(Irql);
    }

    Status = STATUS_SUCCESS;
    _SEH2_TRY {
        KeInitializeDpc(NULL, NULL, NULL);
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        Status = _SEH2_GetExceptionCode();
    } _SEH2_END;
    ok_eq_hex(Status, STATUS_ACCESS_VIOLATION);

    /* These result in IRQL_NOT_LESS_OR_EQUAL on 2k3 -- IRQLs 0x1f and 0xff (?)
    Ret = KeInsertQueueDpc(NULL, NULL, NULL);
    Ret = KeRemoveQueueDpc(NULL);*/

    ok_dpccount();
    ok_irql(PASSIVE_LEVEL);
    trace("Final Dpc count: %ld, expected %ld\n", DpcCount, ExpectedDpcCount);

#ifdef _M_IX86
    TestIdleDpcStack();
#endif
}
