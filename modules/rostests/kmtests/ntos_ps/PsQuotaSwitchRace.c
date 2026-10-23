/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel mode test: moving a process from the default quota block to
 *              its own block while other threads of that process keep charging and
 *              returning pool quota must not lose or invent any quota
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * Charges against the default block do not take the quota lock, so a charge that
 * is in flight on another thread can overlap the snapshot of the process usage and
 * the exchange of the block. A private block that ends up with a different usage
 * than the process holds underflows or leaks when the process returns its quota
 * (ASSERT(QuotaUsage == 0) in PspDereferenceQuotaBlock on a Debug kernel).
 *
 * One process can only make the switch once, so a single run is one attempt. Run
 * the test repeatedly (every run is a new process) on a multiprocessor system.
 */

#include <kmt_test.h>
#include <ndk/pstypes.h>

#define QS_THREADS      6
/*
 * The charges are far larger than any object the process may create on its own
 * meanwhile (a shared charge for a handle lands on the block only, a few hundred
 * bytes), so a lost or invented charge cannot be mistaken for such noise.
 */
#define QS_CHARGE_SIZE  0x1000
#define QS_NOISE        0x800

typedef struct _QS_WORKER
{
    PEPROCESS Process;
    volatile LONG *Stop;
    volatile LONG *Running;
    ULONG Loops;
    ULONG Failed;
    PVOID ThreadObject;
} QS_WORKER;

static
VOID
NTAPI
QsWorker(
    _In_ PVOID Context)
{
    QS_WORKER *W = Context;

    InterlockedIncrement(W->Running);
    while (!*W->Stop)
    {
        /* Each charge is returned before the next one, so the net effect is zero */
        if (NT_SUCCESS(PsChargeProcessPoolQuota(W->Process, PagedPool, QS_CHARGE_SIZE)))
            PsReturnPoolQuota(W->Process, PagedPool, QS_CHARGE_SIZE);
        else
            W->Failed++;
        if (NT_SUCCESS(PsChargeProcessPoolQuota(W->Process, NonPagedPool, QS_CHARGE_SIZE)))
            PsReturnPoolQuota(W->Process, NonPagedPool, QS_CHARGE_SIZE);
        else
            W->Failed++;
        W->Loops++;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(PsQuotaSwitchRace)
{
    PEPROCESS Process = PsGetCurrentProcess();
    PEPROCESS_QUOTA_BLOCK Old, Own;
    QS_WORKER Workers[QS_THREADS];
    HANDLE Handles[QS_THREADS];
    volatile LONG Stop = 0, Running = 0;
    LARGE_INTEGER Delay;
    PVOID UserBuf = NULL;
    SIZE_T Size = PAGE_SIZE;
    NTSTATUS Status;
    ULONG i, t;
    ULONG Created = 0;
    ULONG64 Loops = 0;
    SIZE_T Failed = 0;
    SIZE_T Diff;

    if (skip(KeNumberProcessors > 1, "needs a multiprocessor system\n"))
        return;

    /* PspSetQuotaLimits probes its input unconditionally, so it needs user memory */
    Status = ZwAllocateVirtualMemory(NtCurrentProcess(), &UserBuf, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    RtlZeroMemory(UserBuf, sizeof(QUOTA_LIMITS));

    /* Create the workers while the process is still on its original block */
    RtlZeroMemory(Workers, sizeof(Workers));
    for (i = 0; i < QS_THREADS; i++)
    {
        Workers[i].Process = Process;
        Workers[i].Stop = &Stop;
        Workers[i].Running = &Running;
        Status = PsCreateSystemThread(&Handles[i], THREAD_ALL_ACCESS, NULL, NULL, NULL, QsWorker, &Workers[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            break;
        ObReferenceObjectByHandle(Handles[i], THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                  &Workers[i].ThreadObject, NULL);
        ZwClose(Handles[i]);
        Created++;
    }

    Delay.QuadPart = -10 * 1000 * 5; /* 5 ms */
    while (Running < (LONG)Created)
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    /* Let the workers get going, then switch while they are mid charge */
    Old = Process->QuotaBlock;
    Status = ZwSetInformationProcess(NtCurrentProcess(), ProcessQuotaLimits, UserBuf, sizeof(QUOTA_LIMITS));
    ok_eq_hex(Status, STATUS_SUCCESS);
    Own = Process->QuotaBlock;

    KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    InterlockedExchange((volatile LONG *)&Stop, 1);
    for (i = 0; i < Created; i++)
    {
        KeWaitForSingleObject(Workers[i].ThreadObject, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Workers[i].ThreadObject);
        Loops += Workers[i].Loops;
        Failed += Workers[i].Failed;
    }

    if (!skip(Own != Old, "process did not move to its own quota block\n"))
    {
        ok_eq_size(Failed, 0);
        ok_eq_ulong((ULONG)Own->ProcessCount, 1);
        for (t = 0; t < 2; t++)
        {
            trace("QS type=%lu loops=%I64u block usage=%Iu process usage=%Iu old block usage=%Iu\n",
                  t, Loops, Own->QuotaEntry[t].Usage, Process->QuotaUsage[t], Old->QuotaEntry[t].Usage);
            /*
             * All workers are idle: the private block holds what the process holds,
             * plus at most the shared charge of an object the process creates
             * meanwhile. A charge lost or invented by the switch is a multiple of
             * QS_CHARGE_SIZE off (below the process usage it wraps around).
             */
            Diff = Own->QuotaEntry[t].Usage - Process->QuotaUsage[t];
            ok(Diff < QS_NOISE, "block usage %Iu, process usage %Iu (off by %Id)\n",
               Own->QuotaEntry[t].Usage, Process->QuotaUsage[t], (SSIZE_T)Diff);
        }

        /*
         * Put the process back on the original block, handing its usage back to
         * that block, so later tests and the exit of this process see the state
         * they expect. The private block is left behind (it cannot be released
         * from outside the kernel).
         */
        InterlockedExchangePointer((PVOID *)&Process->QuotaBlock, Old);
        for (t = 0; t < 2; t++)
            InterlockedExchangeAddSizeT(&Old->QuotaEntry[t].Usage, Process->QuotaUsage[t]);
    }

    Size = 0;
    ZwFreeVirtualMemory(NtCurrentProcess(), &UserBuf, &Size, MEM_RELEASE);
}
