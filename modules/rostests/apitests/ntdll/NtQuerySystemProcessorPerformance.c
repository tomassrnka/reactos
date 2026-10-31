/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for NtQuerySystemInformation(SystemProcessorPerformanceInformation):
 *              the idle time accumulates while the machine is idle, and the
 *              kernel time includes the idle time, as on Windows
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include "precomp.h"

/* The record is 48 bytes on Windows for 32-bit and 64-bit callers; spelled out
   so that a compiler that packs a LONGLONG on 4 bytes cannot change the size */
typedef struct _PROC_PERF
{
    LONGLONG IdleTime;
    LONGLONG KernelTime;
    LONGLONG UserTime;
    LONGLONG DpcTime;
    LONGLONG InterruptTime;
    ULONG InterruptCount;
    ULONG Reserved;
} PROC_PERF;

#define MAX_CPUS 64
/* A freshly booted guest can be busy for a while (the zero page thread runs
   ahead of the idle thread), so look at more than one interval */
#define MAX_INTERVALS 30

static
ULONG
QueryProcessors(
    _Out_ PROC_PERF *Info,
    _In_ ULONG Count)
{
    NTSTATUS Status;
    ULONG ReturnLength = 0;

    RtlZeroMemory(Info, Count * sizeof(*Info));
    Status = NtQuerySystemInformation(SystemProcessorPerformanceInformation,
                                      Info,
                                      Count * sizeof(*Info),
                                      &ReturnLength);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(ReturnLength, Count * sizeof(*Info));
    return ReturnLength;
}

START_TEST(NtQuerySystemProcessorPerformance)
{
    SYSTEM_BASIC_INFORMATION Basic;
    PROC_PERF Before[MAX_CPUS], After[MAX_CPUS], First[MAX_CPUS];
    NTSTATUS Status;
    ULONG Cpus, Cpu, Interval;
    BOOLEAN IdleAdvanced = FALSE;
    LONGLONG IdleDelta, KernelDelta;

    Status = NtQuerySystemInformation(SystemBasicInformation, &Basic, sizeof(Basic), NULL);
    ok_hex(Status, STATUS_SUCCESS);
    Cpus = Basic.NumberOfProcessors;
    ok(Cpus >= 1 && Cpus <= MAX_CPUS, "NumberOfProcessors = %lu\n", Cpus);
    if (Cpus < 1 || Cpus > MAX_CPUS)
        return;

    QueryProcessors(First, Cpus);
    RtlCopyMemory(Before, First, Cpus * sizeof(PROC_PERF));

    for (Interval = 0; Interval < MAX_INTERVALS; Interval++)
    {
        Sleep(1000);
        QueryProcessors(After, Cpus);

        IdleDelta = 0;
        KernelDelta = 0;
        for (Cpu = 0; Cpu < Cpus; Cpu++)
        {
            /* Every counter only grows, and the kernel time includes the idle time */
            ok(After[Cpu].IdleTime >= Before[Cpu].IdleTime,
               "CPU %lu: IdleTime went back: %I64d -> %I64d\n",
               Cpu, Before[Cpu].IdleTime, After[Cpu].IdleTime);
            ok(After[Cpu].KernelTime >= Before[Cpu].KernelTime,
               "CPU %lu: KernelTime went back: %I64d -> %I64d\n",
               Cpu, Before[Cpu].KernelTime, After[Cpu].KernelTime);
            ok(After[Cpu].UserTime >= Before[Cpu].UserTime,
               "CPU %lu: UserTime went back\n", Cpu);
            ok(After[Cpu].KernelTime >= After[Cpu].IdleTime,
               "CPU %lu: KernelTime %I64d < IdleTime %I64d\n",
               Cpu, After[Cpu].KernelTime, After[Cpu].IdleTime);
            ok(After[Cpu].KernelTime >= After[Cpu].DpcTime + After[Cpu].InterruptTime,
               "CPU %lu: KernelTime %I64d < Dpc %I64d + Interrupt %I64d\n",
               Cpu, After[Cpu].KernelTime, After[Cpu].DpcTime, After[Cpu].InterruptTime);
            ok(After[Cpu].InterruptCount >= Before[Cpu].InterruptCount,
               "CPU %lu: InterruptCount went back\n", Cpu);

            IdleDelta += After[Cpu].IdleTime - Before[Cpu].IdleTime;
            KernelDelta += After[Cpu].KernelTime - Before[Cpu].KernelTime;
        }

        /* Idle time is part of the kernel time, so it cannot advance faster */
        ok(KernelDelta >= IdleDelta,
           "interval %lu: KernelTime advanced %I64d, IdleTime %I64d\n",
           Interval, KernelDelta, IdleDelta);

        RtlCopyMemory(Before, After, Cpus * sizeof(PROC_PERF));
        if (IdleDelta > 0 && KernelDelta >= IdleDelta)
        {
            IdleAdvanced = TRUE;
            /* One second on an idle machine: most of it is idle (100 ns units,
               some slack for a busy test machine) */
            ok(IdleDelta >= 10000000LL * Cpus / 10,
               "IdleTime advanced only %I64d in one second on %lu CPUs\n", IdleDelta, Cpus);
            break;
        }
        trace("interval %lu: no idle time (kernel %I64d), waiting\n", Interval, KernelDelta);
    }

    ok(IdleAdvanced, "IdleTime did not advance in %lu seconds on an idle system\n", MAX_INTERVALS);

    /* Over the whole run both counters advanced */
    IdleDelta = 0;
    KernelDelta = 0;
    for (Cpu = 0; Cpu < Cpus; Cpu++)
    {
        IdleDelta += After[Cpu].IdleTime - First[Cpu].IdleTime;
        KernelDelta += After[Cpu].KernelTime - First[Cpu].KernelTime;
    }
    ok(IdleDelta > 0, "IdleTime did not advance: %I64d\n", IdleDelta);
    ok(KernelDelta >= IdleDelta, "KernelTime advanced %I64d, IdleTime %I64d\n", KernelDelta, IdleDelta);
}
