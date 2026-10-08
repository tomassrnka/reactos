/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite KeSetPriorityThread stress test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define VICTIM_CPUS_MAX 3
#define VICTIMS_PER_CPU 2
#define VICTIM_HIGH_PRIORITY (LOW_REALTIME_PRIORITY + 2)
#define VICTIM_LOW_PRIORITY (LOW_REALTIME_PRIORITY + 1)
#define STRESS_TIME (2 * 1000 * 1000 * 10) /* 2 seconds */

typedef struct _VICTIM
{
    HANDLE Handle;
    PKTHREAD Thread;
    KAFFINITY Affinity;
    volatile LONG Started;
    volatile LONG Count;
} VICTIM, *PVICTIM;

static volatile LONG StopVictims;

static KSTART_ROUTINE VictimThread;
static
VOID
NTAPI
VictimThread(
    _In_ PVOID Context)
{
    PVICTIM Victim = Context;

    /* Raise first, so the second victim of a processor gets to run there */
    KeSetPriorityThread(KeGetCurrentThread(), VICTIM_HIGH_PRIORITY);
    KeSetSystemAffinityThread(Victim->Affinity);
    InterlockedExchange(&Victim->Started, 1);

    while (!StopVictims)
        InterlockedIncrement(&Victim->Count);

    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*
 * Two CPU-bound victims share each of up to three processors other than
 * the first. The test thread runs on the first processor and lowers and
 * raises the priority of every victim. Lowering the running victim
 * preempts it on its own processor, which moves it from Running to Ready
 * under that processor's PRCB lock while the next priority change may
 * already be looking at it.
 */
START_TEST(KePriority)
{
    NTSTATUS Status;
    KAFFINITY Active, Mask;
    KPRIORITY OldPriority;
    ULONG Cpu, VictimCount, i, Rounds;
    ULONGLONG End;
    LARGE_INTEGER Timeout;
    OBJECT_ATTRIBUTES ObjectAttributes;
    static VICTIM Victims[VICTIM_CPUS_MAX * VICTIMS_PER_CPU];

    Active = KeQueryActiveProcessors();
    if (skip((Active & (Active - 1)) != 0, "Needs at least two processors\n"))
        return;

    /* Keep the test thread on the lowest active processor */
    Mask = Active & (~Active + 1);
    KeSetSystemAffinityThread(Mask);
    OldPriority = KeSetPriorityThread(KeGetCurrentThread(),
                                      LOW_REALTIME_PRIORITY - 1);

    /* Two victims on each of up to VICTIM_CPUS_MAX other processors */
    StopVictims = 0;
    VictimCount = 0;
    RtlZeroMemory(Victims, sizeof(Victims));
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    for (Cpu = 0, Mask = Active & ~Mask;
         Mask && Cpu < VICTIM_CPUS_MAX && !StopVictims;
         Cpu++, Mask &= Mask - 1)
    {
        for (i = 0; i < VICTIMS_PER_CPU; i++)
        {
            PVICTIM Victim = &Victims[VictimCount];

            Victim->Affinity = Mask & (~Mask + 1);
            Status = PsCreateSystemThread(&Victim->Handle, SYNCHRONIZE,
                                          &ObjectAttributes, NULL, NULL,
                                          VictimThread, Victim);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (!NT_SUCCESS(Status))
                break;

            Status = ObReferenceObjectByHandle(Victim->Handle, SYNCHRONIZE,
                                               *PsThreadType, KernelMode,
                                               (PVOID *)&Victim->Thread, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (!NT_SUCCESS(Status))
            {
                StopVictims = 1;
                ZwWaitForSingleObject(Victim->Handle, FALSE, NULL);
                ZwClose(Victim->Handle);
                break;
            }
            VictimCount++;
        }
    }

    /* Wait until every victim runs on its processor */
    Timeout.QuadPart = -10 * 1000 * 10; /* 10 ms */
    for (i = 0; i < VictimCount; i++)
    {
        for (Rounds = 0; !Victims[i].Started && Rounds < 500; Rounds++)
            KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
        ok(Victims[i].Started, "Victim %lu did not start\n", i);
    }

    /* Lower and raise each victim's priority until the time is up */
    Rounds = 0;
    End = KeQueryInterruptTime() + STRESS_TIME;
    while (!StopVictims && KeQueryInterruptTime() < End)
    {
        for (i = 0; i < VictimCount; i++)
        {
            KeSetPriorityThread(Victims[i].Thread, VICTIM_LOW_PRIORITY);
            KeSetPriorityThread(Victims[i].Thread, VICTIM_HIGH_PRIORITY);
        }
        Rounds++;
    }
    ok(Rounds > 0, "No stress round ran\n");
    trace("%lu victims, %lu rounds\n", VictimCount, Rounds);

    /* Every victim must still be schedulable: stop it and wait for it */
    StopVictims = 1;
    Timeout.QuadPart = -10 * 1000 * 1000 * 10; /* 10 seconds */
    for (i = 0; i < VictimCount; i++)
    {
        Status = KeWaitForSingleObject(Victims[i].Thread, Executive,
                                       KernelMode, FALSE, &Timeout);
        ok(Status == STATUS_SUCCESS,
           "Victim %lu did not exit: 0x%lx, count %ld\n",
           i, Status, Victims[i].Count);
        ok(Victims[i].Count > 0, "Victim %lu never ran\n", i);

        /* The victim runs driver code: hang rather than unload under it */
        if (Status != STATUS_SUCCESS)
        {
            KeWaitForSingleObject(Victims[i].Thread, Executive,
                                  KernelMode, FALSE, NULL);
        }
        ObDereferenceObject(Victims[i].Thread);
        ZwClose(Victims[i].Handle);
    }

    KeSetPriorityThread(KeGetCurrentThread(), OldPriority);
    KeRevertToUserAffinityThread();
}
