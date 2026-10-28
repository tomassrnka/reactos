/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel mode test for pool quota accounting of a process that owns
 *              a private (limited) quota block, charged and returned across threads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * The test moves the calling process to its own quota block through
 * ProcessQuotaLimits, optionally gives that block a finite limit, charges and
 * returns pool quota from several system threads, and prints one machine
 * comparable "QL ..." line per phase. Run it against two kernels and diff the
 * lines. The process is put back on its original block before the test returns.
 */

#include <kmt_test.h>
#include <ndk/pstypes.h>

#define MAX_QL_THREADS  8

/* ntoskrnl internal return thresholds (ntoskrnl/include/internal/ps.h) */
#define QL_NON_PAGED_RETURN_THRESHOLD   0x10000
#define QL_PAGED_RETURN_THRESHOLD       0x80000

static BOOLEAN QlWarm;

typedef struct _QL_WORKER
{
    PEPROCESS Process;
    ULONG Count;
    SIZE_T SizePaged;
    SIZE_T SizeNonPaged;
    PKEVENT Start;
    PKEVENT Release;
    volatile LONG *Arrived;
    volatile LONG *Returned;
    ULONG OkPaged, OkNonPaged, FailPaged, FailNonPaged;
    NTSTATUS LastFail;
    PVOID ThreadObject;
} QL_WORKER;

static
VOID
NTAPI
QlWorker(
    _In_ PVOID Context)
{
    QL_WORKER *W = Context;
    ULONG i;
    NTSTATUS Status;

    KeWaitForSingleObject(W->Start, Executive, KernelMode, FALSE, NULL);

    for (i = 0; i < W->Count; i++)
    {
        Status = PsChargeProcessPoolQuota(W->Process, PagedPool, W->SizePaged);
        if (NT_SUCCESS(Status))
            W->OkPaged++;
        else
        {
            W->FailPaged++;
            W->LastFail = Status;
        }

        Status = PsChargeProcessPoolQuota(W->Process, NonPagedPool, W->SizeNonPaged);
        if (NT_SUCCESS(Status))
            W->OkNonPaged++;
        else
        {
            W->FailNonPaged++;
            W->LastFail = Status;
        }
    }

    InterlockedIncrement(W->Arrived);
    KeWaitForSingleObject(W->Release, Executive, KernelMode, FALSE, NULL);

    for (i = 0; i < W->OkPaged; i++)
        PsReturnPoolQuota(W->Process, PagedPool, W->SizePaged);
    for (i = 0; i < W->OkNonPaged; i++)
        PsReturnPoolQuota(W->Process, NonPagedPool, W->SizeNonPaged);

    InterlockedIncrement(W->Returned);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
PEPROCESS_QUOTA_BLOCK
QlSwapToOwnBlock(
    _In_ PEPROCESS Process,
    _Out_ PEPROCESS_QUOTA_BLOCK *OldBlock,
    _Out_ PVOID *UserBuf);

static
PEPROCESS_QUOTA_BLOCK
QlSwapToOwnBlockEx(
    _In_ PEPROCESS Process,
    _In_ SIZE_T PagedLimit,
    _In_ SIZE_T NonPagedLimit,
    _Out_ PEPROCESS_QUOTA_BLOCK *OldBlock,
    _Out_ PVOID *UserBuf,
    _Out_ NTSTATUS *SetStatus);

static
VOID
QlRestore(
    _In_ PEPROCESS Process,
    _In_ PEPROCESS_QUOTA_BLOCK OldBlock,
    _In_ PVOID UserBuf)
{
    SIZE_T Size = 0;
    ULONG t;

    /*
     * Moving onto a private block (PspSetQuotaLimits) seeds it with the usage of
     * the process and takes that usage off the default block. Reverse that, or
     * the default block, which every other process shares, ends up under-counted
     * and underflows once the processes holding that usage return it
     * (bugcheck 0x21). The process usage is what has to live on the original
     * block again, charges made while on the private block included.
     */
    for (t = 0; t < PsQuotaTypes; t++)
    {
        InterlockedExchangeAddSizeT(&OldBlock->QuotaEntry[t].Usage, Process->QuotaUsage[t]);
    }

    /*
     * Put the original block back first: the user buffer was charged to it, and
     * freeing it against the private block would underflow that block.
     */
    InterlockedExchangePointer((PVOID *)&Process->QuotaBlock, OldBlock);
    ZwFreeVirtualMemory(NtCurrentProcess(), &UserBuf, &Size, MEM_RELEASE);
}

typedef struct _QL_SNAP
{
    SIZE_T BlkUse[2], BlkPeak[2], BlkLimit[2];
    SIZE_T ProcUse[2], ProcPeak[2];
} QL_SNAP;

static
VOID
QlSnap(
    _In_ PEPROCESS Process,
    _Out_ QL_SNAP *S)
{
    PEPROCESS_QUOTA_BLOCK B = Process->QuotaBlock;
    ULONG t;

    /* PsNonPagedPool = 0, PsPagedPool = 1 in this tree; index by that */
    for (t = 0; t < 2; t++)
    {
        S->BlkUse[t] = B->QuotaEntry[t].Usage;
        S->BlkPeak[t] = B->QuotaEntry[t].Peak;
        S->BlkLimit[t] = B->QuotaEntry[t].Limit;
        S->ProcUse[t] = Process->QuotaUsage[t];
        S->ProcPeak[t] = Process->QuotaPeak[t];
    }
}

/* Paged = index PsPagedPool, NonPaged = index PsNonPagedPool */
static
VOID
QlPhase(
    _In_ PCSTR Name,
    _In_ PEPROCESS Process,
    _In_ ULONG Threads,
    _In_ ULONG Count,
    _In_ SIZE_T SizePaged,
    _In_ SIZE_T SizeNonPaged,
    _In_ SIZE_T HeadroomPaged,   /* 0 = leave the limit alone */
    _In_ SIZE_T HeadroomNonPaged)
{
    QL_WORKER *W;
    QL_SNAP B, H, E;
    KEVENT Start, Release;
    volatile LONG Arrived = 0, Returned = 0;
    HANDLE Handles[MAX_QL_THREADS];
    ULONG i, OkP = 0, OkN = 0, FailP = 0, FailN = 0;
    NTSTATUS Status, Last = STATUS_SUCCESS;
    SIZE_T HeldP, HeldN, ExpPeakP, ExpPeakN;
    PEPROCESS_QUOTA_BLOCK Blk, OldBlk;
    PVOID UserBuf;
    LARGE_INTEGER Delay;

    W = ExAllocatePoolWithTag(NonPagedPool, sizeof(*W) * Threads, 'lQmK');
    if (!W)
        return;
    RtlZeroMemory(W, sizeof(*W) * Threads);
    KeInitializeEvent(&Start, NotificationEvent, FALSE);
    KeInitializeEvent(&Release, NotificationEvent, FALSE);
    Delay.QuadPart = -10 * 1000 * 10; /* 10 ms */

    /*
     * Create the workers while the process still sits on its original block:
     * thread objects are charged to the creator's block and returned to it
     * when they die, which would otherwise pollute the private block.
     */
    for (i = 0; i < Threads; i++)
    {
        W[i].Process = Process;
        W[i].Count = Count;
        W[i].SizePaged = SizePaged;
        W[i].SizeNonPaged = SizeNonPaged;
        W[i].Start = &Start;
        W[i].Release = &Release;
        W[i].Arrived = &Arrived;
        W[i].Returned = &Returned;
        Status = PsCreateSystemThread(&Handles[i], THREAD_ALL_ACCESS, NULL, NULL, NULL, QlWorker, &W[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ObReferenceObjectByHandle(Handles[i], THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &W[i].ThreadObject, NULL);
        ZwClose(Handles[i]);
    }

    /* A fresh private block per phase keeps the (buggy) Return field budget per phase */
    Blk = QlSwapToOwnBlock(Process, &OldBlk, &UserBuf);
    if (!Blk)
    {
        KeSetEvent(&Start, 0, FALSE);
        KeSetEvent(&Release, 0, FALSE);
        for (i = 0; i < Threads; i++)
        {
            KeWaitForSingleObject(W[i].ThreadObject, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(W[i].ThreadObject);
        }
        ExFreePoolWithTag(W, 'lQmK');
        return;
    }
    QlSnap(Process, &B);
    if (HeadroomPaged)
        Blk->QuotaEntry[PsPagedPool].Limit = B.BlkUse[PsPagedPool] + HeadroomPaged;
    if (HeadroomNonPaged)
        Blk->QuotaEntry[PsNonPagedPool].Limit = B.BlkUse[PsNonPagedPool] + HeadroomNonPaged;
    QlSnap(Process, &B);

    KeSetEvent(&Start, 0, FALSE);
    while (Arrived < (LONG)Threads)
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    QlSnap(Process, &H);
    KeSetEvent(&Release, 0, FALSE);
    while (Returned < (LONG)Threads)
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    QlSnap(Process, &E);

    /* Back to the original block before the workers (and the user buffer) go away */
    QlRestore(Process, OldBlk, UserBuf);

    for (i = 0; i < Threads; i++)
    {
        KeWaitForSingleObject(W[i].ThreadObject, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(W[i].ThreadObject);
        OkP += W[i].OkPaged;
        OkN += W[i].OkNonPaged;
        FailP += W[i].FailPaged;
        FailN += W[i].FailNonPaged;
        if (W[i].LastFail != STATUS_SUCCESS)
            Last = W[i].LastFail;
    }

    HeldP = OkP * SizePaged;
    HeldN = OkN * SizeNonPaged;
    ExpPeakP = max(B.BlkPeak[PsPagedPool], B.BlkUse[PsPagedPool] + HeldP);
    ExpPeakN = max(B.BlkPeak[PsNonPagedPool], B.BlkUse[PsNonPagedPool] + HeldN);

    /* Machine comparable line: everything relative to the pre-phase baseline */
    if (!QlWarm) trace("QL %s thr=%lu cnt=%lu szP=%Iu szN=%Iu okP=%lu failP=%lu okN=%lu failN=%lu last=%08lx\n",
          Name, Threads, Count, SizePaged, SizeNonPaged, OkP, FailP, OkN, FailN, (ULONG)Last);
    trace("QL %s held blk(P=%Id N=%Id) proc(P=%Id N=%Id) limit(P=%Id N=%Id) peak(P=%Id N=%Id) procpeak(P=%Id N=%Id)\n",
          Name,
          (SSIZE_T)(H.BlkUse[PsPagedPool] - B.BlkUse[PsPagedPool]), (SSIZE_T)(H.BlkUse[PsNonPagedPool] - B.BlkUse[PsNonPagedPool]),
          (SSIZE_T)(H.ProcUse[PsPagedPool] - B.ProcUse[PsPagedPool]), (SSIZE_T)(H.ProcUse[PsNonPagedPool] - B.ProcUse[PsNonPagedPool]),
          (SSIZE_T)(H.BlkLimit[PsPagedPool] - B.BlkUse[PsPagedPool]), (SSIZE_T)(H.BlkLimit[PsNonPagedPool] - B.BlkUse[PsNonPagedPool]),
          (SSIZE_T)(H.BlkPeak[PsPagedPool] - B.BlkUse[PsPagedPool]), (SSIZE_T)(H.BlkPeak[PsNonPagedPool] - B.BlkUse[PsNonPagedPool]),
          (SSIZE_T)(H.ProcPeak[PsPagedPool] - B.ProcUse[PsPagedPool]), (SSIZE_T)(H.ProcPeak[PsNonPagedPool] - B.ProcUse[PsNonPagedPool]));
    trace("QL %s end  blk(P=%Id N=%Id) proc(P=%Id N=%Id) limit(P=%Id N=%Id) peak(P=%Id N=%Id) procpeak(P=%Id N=%Id)\n",
          Name,
          (SSIZE_T)(E.BlkUse[PsPagedPool] - B.BlkUse[PsPagedPool]), (SSIZE_T)(E.BlkUse[PsNonPagedPool] - B.BlkUse[PsNonPagedPool]),
          (SSIZE_T)(E.ProcUse[PsPagedPool] - B.ProcUse[PsPagedPool]), (SSIZE_T)(E.ProcUse[PsNonPagedPool] - B.ProcUse[PsNonPagedPool]),
          (SSIZE_T)(E.BlkLimit[PsPagedPool] - B.BlkUse[PsPagedPool]), (SSIZE_T)(E.BlkLimit[PsNonPagedPool] - B.BlkUse[PsNonPagedPool]),
          (SSIZE_T)(E.BlkPeak[PsPagedPool] - B.BlkUse[PsPagedPool]), (SSIZE_T)(E.BlkPeak[PsNonPagedPool] - B.BlkUse[PsNonPagedPool]),
          (SSIZE_T)(E.ProcPeak[PsPagedPool] - B.ProcUse[PsPagedPool]), (SSIZE_T)(E.ProcPeak[PsNonPagedPool] - B.ProcUse[PsNonPagedPool]));

    /* Invariants that must hold on any correct kernel (the cold first phase only warms lazily charged state) */
    if (!QlWarm)
    {
    ok_eq_size(H.BlkUse[PsPagedPool], B.BlkUse[PsPagedPool] + HeldP);
    ok_eq_size(H.BlkUse[PsNonPagedPool], B.BlkUse[PsNonPagedPool] + HeldN);
    ok_eq_size(H.ProcUse[PsPagedPool], B.ProcUse[PsPagedPool] + HeldP);
    ok_eq_size(H.ProcUse[PsNonPagedPool], B.ProcUse[PsNonPagedPool] + HeldN);
    ok_eq_size(H.BlkPeak[PsPagedPool], ExpPeakP);
    ok_eq_size(H.BlkPeak[PsNonPagedPool], ExpPeakN);
    ok_eq_size(E.BlkUse[PsPagedPool], B.BlkUse[PsPagedPool]);
    ok_eq_size(E.BlkUse[PsNonPagedPool], B.BlkUse[PsNonPagedPool]);
    ok_eq_size(E.ProcUse[PsPagedPool], B.ProcUse[PsPagedPool]);
    ok_eq_size(E.ProcUse[PsNonPagedPool], B.ProcUse[PsNonPagedPool]);
    ok_eq_size(E.BlkPeak[PsPagedPool], H.BlkPeak[PsPagedPool]);
    ok_eq_size(E.BlkPeak[PsNonPagedPool], H.BlkPeak[PsNonPagedPool]);
    ok(H.BlkUse[PsPagedPool] <= H.BlkLimit[PsPagedPool], "paged usage %Iu above limit %Iu\n",
       H.BlkUse[PsPagedPool], H.BlkLimit[PsPagedPool]);
    ok(H.BlkUse[PsNonPagedPool] <= H.BlkLimit[PsNonPagedPool], "nonpaged usage %Iu above limit %Iu\n",
       H.BlkUse[PsNonPagedPool], H.BlkLimit[PsNonPagedPool]);
    }

    ExFreePoolWithTag(W, 'lQmK');
}

static
PEPROCESS_QUOTA_BLOCK
QlSwapToOwnBlock(
    _In_ PEPROCESS Process,
    _Out_ PEPROCESS_QUOTA_BLOCK *OldBlock,
    _Out_ PVOID *UserBuf)
{
    PEPROCESS_QUOTA_BLOCK Own;
    NTSTATUS SetStatus;

    Own = QlSwapToOwnBlockEx(Process, 0, 0, OldBlock, UserBuf, &SetStatus);
    if (!Own && *UserBuf)
    {
        SIZE_T Size = 0;

        ZwFreeVirtualMemory(NtCurrentProcess(), UserBuf, &Size, MEM_RELEASE);
        *UserBuf = NULL;
    }
    return Own;
}

/*
 * Moves the process to a private block through ProcessQuotaLimits, asking for
 * the given paged / non paged pool limits (0 = leave unlimited). Returns the new
 * block, or NULL if the process did not move (SetStatus has the reason). The
 * user buffer is returned in both cases when it was allocated.
 */
static
PEPROCESS_QUOTA_BLOCK
QlSwapToOwnBlockEx(
    _In_ PEPROCESS Process,
    _In_ SIZE_T PagedLimit,
    _In_ SIZE_T NonPagedLimit,
    _Out_ PEPROCESS_QUOTA_BLOCK *OldBlock,
    _Out_ PVOID *UserBuf,
    _Out_ NTSTATUS *SetStatus)
{
    PVOID Buf = NULL;
    SIZE_T Size = PAGE_SIZE;
    NTSTATUS Status;
    QUOTA_LIMITS *Limits;

    *UserBuf = NULL;
    *SetStatus = STATUS_UNSUCCESSFUL;

    /* PspSetQuotaLimits probes its input unconditionally, so it needs user memory */
    Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &Buf, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;
    RtlZeroMemory(Buf, sizeof(QUOTA_LIMITS));
    Limits = Buf;
    Limits->PagedPoolLimit = PagedLimit;
    Limits->NonPagedPoolLimit = NonPagedLimit;
    *OldBlock = Process->QuotaBlock;
    Status = ZwSetInformationProcess(NtCurrentProcess(), ProcessQuotaLimits, Buf, sizeof(QUOTA_LIMITS));
    *SetStatus = Status;
    *UserBuf = Buf;

    if (Process->QuotaBlock == *OldBlock)
    {
        trace("QL process did not move to its own quota block (0x%08lx)\n", Status);
        return NULL;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);

    /*
     * A new block starts at the usage of the process and with nothing waiting
     * to be returned. Report any garbage and clear Return. The usage must not
     * be touched: other threads charge objects to this block (Process == NULL)
     * while the test runs, and wiping the seeded usage loses those charges, so
     * their return underflows the block later (bugcheck 0x21 after the test).
     */
    {
        PEPROCESS_QUOTA_BLOCK N = Process->QuotaBlock;
        ULONG t;

        for (t = 0; t < 2; t++)
        {
            trace("QL fresh block type=%lu Usage=%Iu Return=%Iu Limit=%Iu Peak=%Iu\n", t,
                  N->QuotaEntry[t].Usage, N->QuotaEntry[t].Return, N->QuotaEntry[t].Limit, N->QuotaEntry[t].Peak);
            /* A new private block is seeded with the process' own usage, not zero. */
            ok_eq_size(N->QuotaEntry[t].Usage, Process->QuotaUsage[t]);
            ok_eq_size(N->QuotaEntry[t].Return, 0);
            N->QuotaEntry[t].Return = 0;
        }
    }
    return Process->QuotaBlock;
}

START_TEST(PsQuotaLimited)
{
    PEPROCESS Process = PsGetCurrentProcess();
    ULONG T;
    static const ULONG Tc[] = { 1, 2, 4, 8 };

    /* 0. Warm-up: first use charges some lazily created state; not part of the comparison */
    QlWarm = TRUE;
    QlPhase("warm", Process, 1, 50, 0x80, 0x100, 0, 0);
    QlWarm = FALSE;

    /* 1. Private block, limit as created (no finite limit): accounting only */
    for (T = 0; T < RTL_NUMBER_OF(Tc); T++)
        QlPhase("nolimit", Process, Tc[T], 50, 0x80, 0x100, 0, 0);

    /* 2. Finite limit, charges fit: no failure */
    for (T = 0; T < RTL_NUMBER_OF(Tc); T++)
        QlPhase("fit", Process, Tc[T], 50, 0x80, 0x100, 0x400000, 0x400000);

    /* 3. Finite limit, demand beyond the headroom: Mm raises the limit */
    for (T = 0; T < RTL_NUMBER_OF(Tc); T++)
        QlPhase("raise", Process, Tc[T], 25, 0x100, 0x100, 0x4000, 0x1000);

    /* 4. Single oversized charge: must be rejected with STATUS_QUOTA_EXCEEDED */
    for (T = 0; T < RTL_NUMBER_OF(Tc); T++)
        QlPhase("reject", Process, Tc[T], 1, 0x40000000, 0x20000000, 0x4000, 0x1000);
}

/*
 * Deliberately returns more than the return threshold to a block whose limit is
 * still far above its usage. Kept separate because it is expected to crash.
 */
START_TEST(PsQuotaLimitedTrim)
{
    PEPROCESS Process = PsGetCurrentProcess();
    PEPROCESS_QUOTA_BLOCK Old, Own;
    PVOID UserBuf;
    NTSTATUS Status;
    ULONG i;

    Own = QlSwapToOwnBlock(Process, &Old, &UserBuf);
    if (skip(Own != NULL, "no private quota block\n"))
        return;

    for (i = 0; i < 24; i++)
    {
        Status = PsChargeProcessPoolQuota(Process, PagedPool, 0x1000);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    trace("QLTRIM charged 0x18000 paged, returning in 0x1000 pieces\n");
    for (i = 0; i < 24; i++)
    {
        trace("QLTRIM return %lu Return=%Iu\n", i + 1, Own->QuotaEntry[PsPagedPool].Return);
        PsReturnPoolQuota(Process, PagedPool, 0x1000);
    }
    trace("QLTRIM survived\n");
    QlRestore(Process, Old, UserBuf);
}

/*
 * The requested pool limits must be honored when the block is created: a finite
 * limit lands in the block and is enforced, zero and SIZE_T_MAX stay unlimited,
 * and a finite limit below what the process already holds is rejected.
 */
START_TEST(PsQuotaLimitedSetLimits)
{
    PEPROCESS Process = PsGetCurrentProcess();
    PEPROCESS_QUOTA_BLOCK Old, Own;
    PVOID UserBuf;
    NTSTATUS SetStatus, Status;
    const SIZE_T WantPaged = 0x1000000, WantNonPaged = 0x800000;

    /* Finite limits are stored */
    Own = QlSwapToOwnBlockEx(Process, WantPaged, WantNonPaged, &Old, &UserBuf, &SetStatus);
    if (!skip(Own != NULL, "no private quota block\n"))
    {
        trace("QLSET limits paged=%Iu nonpaged=%Iu\n",
              Own->QuotaEntry[PsPagedPool].Limit, Own->QuotaEntry[PsNonPagedPool].Limit);
        ok_eq_size(Own->QuotaEntry[PsPagedPool].Limit, WantPaged);
        ok_eq_size(Own->QuotaEntry[PsNonPagedPool].Limit, WantNonPaged);
        ok_eq_size(Own->QuotaEntry[PsPageFile].Limit, (SIZE_T)-1);

        /* and enforced: a charge far beyond the limit must fail, a small one fit */
        Status = PsChargeProcessPoolQuota(Process, PagedPool, 0x1000);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            PsReturnPoolQuota(Process, PagedPool, 0x1000);
        Status = PsChargeProcessPoolQuota(Process, PagedPool, 0x40000000);
        trace("QLSET paged 0x40000000 charge -> 0x%08lx\n", Status);
        ok_eq_hex(Status, STATUS_QUOTA_EXCEEDED);
        if (NT_SUCCESS(Status))
            PsReturnPoolQuota(Process, PagedPool, 0x40000000);
        Status = PsChargeProcessPoolQuota(Process, NonPagedPool, 0x20000000);
        trace("QLSET nonpaged 0x20000000 charge -> 0x%08lx\n", Status);
        ok_eq_hex(Status, STATUS_QUOTA_EXCEEDED);
        if (NT_SUCCESS(Status))
            PsReturnPoolQuota(Process, NonPagedPool, 0x20000000);
        QlRestore(Process, Old, UserBuf);
    }
    else if (UserBuf)
    {
        SIZE_T Size = 0;
        ZwFreeVirtualMemory(NtCurrentProcess(), &UserBuf, &Size, MEM_RELEASE);
    }

    /* SIZE_T_MAX means unlimited */
    Own = QlSwapToOwnBlockEx(Process, (SIZE_T)-1, (SIZE_T)-1, &Old, &UserBuf, &SetStatus);
    if (!skip(Own != NULL, "no private quota block\n"))
    {
        ok_eq_size(Own->QuotaEntry[PsPagedPool].Limit, (SIZE_T)-1);
        ok_eq_size(Own->QuotaEntry[PsNonPagedPool].Limit, (SIZE_T)-1);
        QlRestore(Process, Old, UserBuf);
    }

    /* Zero means unlimited */
    Own = QlSwapToOwnBlockEx(Process, 0, 0, &Old, &UserBuf, &SetStatus);
    if (!skip(Own != NULL, "no private quota block\n"))
    {
        ok_eq_size(Own->QuotaEntry[PsPagedPool].Limit, (SIZE_T)-1);
        ok_eq_size(Own->QuotaEntry[PsNonPagedPool].Limit, (SIZE_T)-1);
        QlRestore(Process, Old, UserBuf);
    }

    /* A finite limit below the process' current usage is invalid and changes nothing */
    if (!skip(Process->QuotaUsage[PsNonPagedPool] > 1, "process holds no nonpaged quota\n"))
    {
        Old = Process->QuotaBlock;
        Own = QlSwapToOwnBlockEx(Process, 0, 1, &Old, &UserBuf, &SetStatus);
        ok(Own == NULL, "process moved to a block whose limit is below its usage\n");
        ok_eq_hex(SetStatus, STATUS_INVALID_PARAMETER);
        ok(Process->QuotaBlock == Old, "quota block changed on a rejected request\n");
        if (Own)
            QlRestore(Process, Old, UserBuf);
        else if (UserBuf)
        {
            SIZE_T Size = 0;
            ZwFreeVirtualMemory(NtCurrentProcess(), &UserBuf, &Size, MEM_RELEASE);
        }
    }
}

/*
 * The paged and non paged pools trim their limit by a different amount of
 * returned quota: 0x80000 for paged, 0x10000 for non paged. Return one chunk
 * larger than both and check how much was queued for Mm. Stays below the
 * threshold so it does not depend on the Return counter reset.
 */
START_TEST(PsQuotaLimitedThreshold)
{
    PEPROCESS Process = PsGetCurrentProcess();
    PEPROCESS_QUOTA_BLOCK Old, Own;
    PVOID UserBuf;
    NTSTATUS Status;

    Own = QlSwapToOwnBlock(Process, &Old, &UserBuf);
    if (skip(Own != NULL, "no private quota block\n"))
        return;

    Status = PsChargeProcessPoolQuota(Process, NonPagedPool, 0x20000);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = PsChargeProcessPoolQuota(Process, PagedPool, 0x100000);
    ok_eq_hex(Status, STATUS_SUCCESS);

    PsReturnPoolQuota(Process, NonPagedPool, 0x20000);
    PsReturnPoolQuota(Process, PagedPool, 0x100000);
    trace("QLTHR Return nonpaged=0x%Ix paged=0x%Ix\n",
          Own->QuotaEntry[PsNonPagedPool].Return, Own->QuotaEntry[PsPagedPool].Return);
    ok_eq_size(Own->QuotaEntry[PsNonPagedPool].Return, QL_NON_PAGED_RETURN_THRESHOLD);
    ok_eq_size(Own->QuotaEntry[PsPagedPool].Return, QL_PAGED_RETURN_THRESHOLD);

    QlRestore(Process, Old, UserBuf);
}
