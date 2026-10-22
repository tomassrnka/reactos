/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQuerySystemInformation
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 *              Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

static
void
Test_ThreadStackLimits(void)
{
    NTSTATUS Status;
    ULONG_PTR SystemRangeStart;
    PSYSTEM_PROCESS_INFORMATION Process;
    PSYSTEM_EXTENDED_THREAD_INFORMATION Thread;
    PVOID Buffer = NULL;
    SIZE_T BufferSize = 0x10000;
    ULONG Length, i, Threads = 0, Hidden = 0, Bad = 0;
    BOOL IsWow64 = FALSE;

    if (IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64)
    {
        skip("WOW64 reports kernel stack addresses truncated to 32 bits\n");
        return;
    }

    Status = NtQuerySystemInformation(SystemRangeStartInformation,
                                      &SystemRangeStart,
                                      sizeof(SystemRangeStart),
                                      NULL);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    for (;;)
    {
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
        if (!Buffer)
        {
            skip("Out of memory\n");
            return;
        }
        Status = NtQuerySystemInformation(SystemExtendedProcessInformation,
                                          Buffer,
                                          (ULONG)BufferSize,
                                          &Length);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            break;
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        BufferSize *= 2;
    }

    if (!NT_SUCCESS(Status))
    {
        skip("SystemExtendedProcessInformation: 0x%lx\n", Status);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        return;
    }

    /* Every thread has a kernel stack. The two bounds are read separately
     * while a thread may switch to a large stack, so do not compare them. */
    Process = Buffer;
    for (;;)
    {
        Thread = (PSYSTEM_EXTENDED_THREAD_INFORMATION)(Process + 1);
        for (i = 0; i < Process->NumberOfThreads; i++, Thread++, Threads++)
        {
            if (!Thread->StackBase && !Thread->StackLimit)
            {
                Hidden++;
                continue;
            }
            if ((ULONG_PTR)Thread->StackLimit < SystemRangeStart ||
                (ULONG_PTR)Thread->StackBase < SystemRangeStart)
            {
                trace("Process %p thread %p: stack limit %p, base %p\n",
                      Thread->ThreadInfo.ClientId.UniqueProcess,
                      Thread->ThreadInfo.ClientId.UniqueThread,
                      Thread->StackLimit,
                      Thread->StackBase);
                Bad++;
            }
        }
        if (!Process->NextEntryOffset)
            break;
        Process = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Process + Process->NextEntryOffset);
    }

    ok(Threads > 0, "No threads listed\n");
    if (Hidden == Threads)
        skip("Kernel stack addresses are not disclosed\n");
    else
        ok(Bad == 0, "%lu of %lu threads have a stack bound below %p\n", Bad, Threads - Hidden, (PVOID)SystemRangeStart);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

START_TEST(NtQuerySystemInformation)
{
    NTSTATUS Status;

    Status = NtQuerySystemInformation(0, NULL, 0, NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    Status = NtQuerySystemInformation(0x80000000, NULL, 0, NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);

    Test_ThreadStackLimits();
}
