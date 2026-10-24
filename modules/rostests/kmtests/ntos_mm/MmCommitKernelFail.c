/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Induce kernel-tier commit charge failures (T38 observability check)
 */

#include <kmt_test.h>

#define MAX_CHUNKS 4096

START_TEST(MmCommitKernelFail)
{
    static PVOID UserChunks[MAX_CHUNKS];
    static PVOID PoolChunks[MAX_CHUNKS];
    ULONG UserCount = 0, PoolCount = 0, Fails = 0, Attempts, i;
    SIZE_T Size;
    NTSTATUS Status;
    LARGE_INTEGER Delay;

    /* Eat user-tier commit until it refuses (limit minus the kernel reserve) */
    for (Size = 64 * 1024 * 1024; Size >= 1024 * 1024; Size /= 2)
    {
        for (;;)
        {
            PVOID Base = NULL;
            SIZE_T Sz = Size;
            if (UserCount >= MAX_CHUNKS) break;
            Status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &Base, 0, &Sz,
                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!NT_SUCCESS(Status)) break;
            UserChunks[UserCount++] = Base;
        }
    }
    trace("user tier exhausted after %lu chunks\n", UserCount);

    /* Kernel-tier: 1 MB paged pool blocks until 200 failures have been seen */
    for (Attempts = 0; Attempts < MAX_CHUNKS && Fails < 200; Attempts++)
    {
        PVOID P = ExAllocatePoolWithTag(PagedPool, 1024 * 1024, 'FcmK');
        if (P)
            PoolChunks[PoolCount++] = P;
        else
            Fails++;
    }
    trace("pool: %lu ok, %lu failed in %lu attempts\n", PoolCount, Fails, Attempts);
    ok(PoolCount > 0, "kernel reserve should allow some pool\n");

    for (i = 0; i < PoolCount; i++)
        ExFreePoolWithTag(PoolChunks[i], 'FcmK');
    for (i = 0; i < UserCount; i++)
    {
        SIZE_T Sz = 0;
        PVOID Base = UserChunks[i];
        ZwFreeVirtualMemory(ZwCurrentProcess(), &Base, &Sz, MEM_RELEASE);
    }
    Delay.QuadPart = -10 * 1000 * 1000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}
