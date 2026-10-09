/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Test for NtFreeVirtualMemory
 * COPYRIGHT:   Copyright 2011 Jérôme Gardou <jerome.gardou@reactos.org>
 *              Copyright 2017 Serge Gautherie <reactos-git_serge_171003@gautherie.fr>
 */

#include "precomp.h"

static void Test_NtFreeVirtualMemory(void)
{
    PVOID Buffer = NULL, Buffer2;
    SIZE_T Length = PAGE_SIZE;
    NTSTATUS Status;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Buffer,
                                     0,
                                     &Length,
                                     MEM_RESERVE,
                                     PAGE_READWRITE);
    ok(NT_SUCCESS(Status), "NtAllocateVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(((ULONG_PTR)Buffer % PAGE_SIZE) == 0, "The buffer is not aligned to PAGE_SIZE.\n");

    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer,
                                 &Length,
                                 MEM_DECOMMIT);
    ok(Status == STATUS_SUCCESS, "NtFreeVirtualMemory failed : 0x%08lx\n", Status);

    /* Now try to free more than we got */
    Length++;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer,
                                 &Length,
                                 MEM_DECOMMIT);
    ok(Status == STATUS_UNABLE_TO_FREE_VM, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);

    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer,
                                 &Length,
                                 MEM_RELEASE);
    ok(Status == STATUS_UNABLE_TO_FREE_VM, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);

    /* Free out of bounds from the wrong origin */
    Length = PAGE_SIZE;
    Buffer2 = (PVOID)((ULONG_PTR)Buffer+1);

    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_DECOMMIT);
    ok(Status == STATUS_UNABLE_TO_FREE_VM, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);

    Buffer2 = (PVOID)((ULONG_PTR)Buffer+1);
    Length = PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_RELEASE);
    ok(Status == STATUS_UNABLE_TO_FREE_VM, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);

    /* Same but in bounds */
    Length = PAGE_SIZE - 1;
    Buffer2 = (PVOID)((ULONG_PTR)Buffer+1);

    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_DECOMMIT);
    ok(Status == STATUS_SUCCESS, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);
    ok(Buffer2 == Buffer, "NtFreeVirtualMemory set wrong buffer.\n");
    ok(Length == PAGE_SIZE, "NtFreeVirtualMemory did not round Length to PAGE_SIZE.\n");

    Buffer2 = (PVOID)((ULONG_PTR)Buffer+1);
    Length = PAGE_SIZE-1;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_RELEASE);
    ok(Status == STATUS_SUCCESS, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);
    ok(Buffer2 == Buffer, "NtFreeVirtualMemory set wrong buffer.\n");
    ok(Length == PAGE_SIZE, "NtFreeVirtualMemory did not round Length to PAGE_SIZE.\n");

    /* Now allocate two pages and try to free them one after the other */
    Length = 2*PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Buffer,
                                     0,
                                     &Length,
                                     MEM_RESERVE,
                                     PAGE_READWRITE);
    ok(NT_SUCCESS(Status), "NtAllocateVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == 2*PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(((ULONG_PTR)Buffer % PAGE_SIZE) == 0, "The buffer is not aligned to PAGE_SIZE.\n");

    Buffer2 = Buffer;
    Length = PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_RELEASE);
    ok(NT_SUCCESS(Status), "NtFreeVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(Buffer2 == Buffer, "The buffer is not aligned to PAGE_SIZE.\n");

    Buffer2 = (PVOID)((ULONG_PTR)Buffer+PAGE_SIZE);
    Length = PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_RELEASE);
    ok(NT_SUCCESS(Status), "NtFreeVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(Buffer2 == (PVOID)((ULONG_PTR)Buffer+PAGE_SIZE), "The buffer is not aligned to PAGE_SIZE.\n");

    /* Same, but try to free the second page before the first one */
    Length = 2*PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Buffer,
                                     0,
                                     &Length,
                                     MEM_RESERVE,
                                     PAGE_READWRITE);
    ok(NT_SUCCESS(Status), "NtAllocateVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == 2*PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(((ULONG_PTR)Buffer % PAGE_SIZE) == 0, "The buffer is not aligned to PAGE_SIZE.\n");

    Buffer2 = (PVOID)((ULONG_PTR)Buffer+PAGE_SIZE);
    Length = PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_RELEASE);
    ok(NT_SUCCESS(Status), "NtFreeVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(Buffer2 == (PVOID)((ULONG_PTR)Buffer+PAGE_SIZE), "The buffer is not aligned to PAGE_SIZE.\n");

    Buffer2 = Buffer;
    Length = PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_RELEASE);
    ok(NT_SUCCESS(Status), "NtFreeVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(Buffer2 == Buffer, "The buffer is not aligned to PAGE_SIZE.\n");

    /* Now allocate two pages and try to free them in the middle */
    Length = 2*PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Buffer,
                                     0,
                                     &Length,
                                     MEM_RESERVE,
                                     PAGE_READWRITE);
    ok(NT_SUCCESS(Status), "NtAllocateVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == 2*PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(((ULONG_PTR)Buffer % PAGE_SIZE) == 0, "The buffer is not aligned to PAGE_SIZE.\n");

    Buffer2 = (PVOID)((ULONG_PTR)Buffer+1);
    Length = PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                 &Buffer2,
                                 &Length,
                                 MEM_RELEASE);
    ok(NT_SUCCESS(Status), "NtFreeVirtualMemory failed : 0x%08lx\n", Status);
    ok(Length == 2*PAGE_SIZE, "Length mismatch : 0x%08lx\n", (ULONG)Length);
    ok(Buffer2 == Buffer, "The buffer is not aligned to PAGE_SIZE.\n");
}

static void Test_NtFreeVirtualMemory_Parameters(void)
{
    NTSTATUS Status;
    ULONG FreeType;
    int i;

    // 4th parameter: "ULONG FreeType".

    // A type is mandatory.
    Status = NtFreeVirtualMemory(NULL, NULL, NULL, 0ul);
    ok(Status == STATUS_INVALID_PARAMETER_4, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);

    // All but MEM_DECOMMIT and MEM_RELEASE are unsupported.
    // Each bit one by one.
    for (i = 0; i < 32; ++i)
    {
        FreeType = 1 << i;
        if (FreeType == MEM_DECOMMIT || FreeType == MEM_RELEASE)
            continue;

        Status = NtFreeVirtualMemory(NULL, NULL, NULL, FreeType);
        ok(Status == STATUS_INVALID_PARAMETER_4, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);
    }
    // All bits at once.
    // Not testing all other values.
    Status = NtFreeVirtualMemory(NULL, NULL, NULL, ~(MEM_DECOMMIT | MEM_RELEASE));
    ok(Status == STATUS_INVALID_PARAMETER_4, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);
    Status = NtFreeVirtualMemory(NULL, NULL, NULL, ~MEM_DECOMMIT);
    ok(Status == STATUS_INVALID_PARAMETER_4, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);
    Status = NtFreeVirtualMemory(NULL, NULL, NULL, ~MEM_RELEASE);
    ok(Status == STATUS_INVALID_PARAMETER_4, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);
    Status = NtFreeVirtualMemory(NULL, NULL, NULL, ~0ul);
    ok(Status == STATUS_INVALID_PARAMETER_4, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);

    // MEM_DECOMMIT and MEM_RELEASE are exclusive.
    Status = NtFreeVirtualMemory(NULL, NULL, NULL, MEM_DECOMMIT | MEM_RELEASE);
    ok(Status == STATUS_INVALID_PARAMETER_4, "NtFreeVirtualMemory returned status : 0x%08lx\n", Status);
}

static ULONG GetAvailablePages(void)
{
    /* Larger than any version's structure: only AvailablePages is read */
    ULONG Buffer[256];
    PSYSTEM_PERFORMANCE_INFORMATION Info = (PSYSTEM_PERFORMANCE_INFORMATION)Buffer;
    ULONG Length = 0;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemPerformanceInformation, Buffer, sizeof(Buffer), &Length);
    if (!NT_SUCCESS(Status) || Length < FIELD_OFFSET(SYSTEM_PERFORMANCE_INFORMATION, AvailablePages) + sizeof(ULONG))
        return 0;
    return Info->AvailablePages;
}

/* Pages that were touched and then made inaccessible must be freed by MEM_DECOMMIT */
static void Test_NtFreeVirtualMemory_DecommitInaccessible(void)
{
    const ULONG PageCount = 16, Rounds = 512;
    PVOID Base = NULL, Address;
    SIZE_T Size = PageCount * PAGE_SIZE;
    MEMORY_BASIC_INFORMATION Mbi;
    ULONG i, Old, AvailableBefore, AvailableAfter;
    PUCHAR Bytes;
    NTSTATUS Status;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Bytes = Base;
    for (i = 0; i < PageCount; i++)
        Bytes[i * PAGE_SIZE] = (UCHAR)(i + 1);

    /* Half of the pages no-access, half guard pages */
    for (i = 0; i < PageCount; i++)
    {
        Address = Bytes + i * PAGE_SIZE;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size,
                                        (i & 1) ? (PAGE_READWRITE | PAGE_GUARD) : PAGE_NOACCESS, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }

    Address = Base;
    Size = PageCount * PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_DECOMMIT);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtQueryVirtualMemory(NtCurrentProcess(), Base, MemoryBasicInformation, &Mbi, sizeof(Mbi), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(Mbi.State, MEM_RESERVE);
    ok_size_t(Mbi.RegionSize, PageCount * PAGE_SIZE);

    /* Committed again, the pages must read as zero */
    Address = Base;
    Size = PageCount * PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < PageCount; i++)
            ok(Bytes[i * PAGE_SIZE] == 0, "Page %lu holds %u after decommit and commit\n", i, Bytes[i * PAGE_SIZE]);
    }

    /* The physical pages must come back: a rough check of the system-wide count, which a leak of one page per round fails on a quiet system */
    AvailableBefore = GetAvailablePages();
    for (i = 0; i < Rounds; i++)
    {
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
            break;
        Bytes[0] = 1;
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_NOACCESS, &Old);
        if (!NT_SUCCESS(Status))
            break;
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_DECOMMIT);
        if (!NT_SUCCESS(Status))
            break;
    }
    ok(i == Rounds, "Round %lu failed with 0x%08lx\n", i, Status);
    AvailableAfter = GetAvailablePages();
    /* Other activity moves the count on Windows: check the loss on ReactOS only */
    if (!is_reactos())
        skip("Not checking the available page count\n");
    else if (!AvailableBefore || !AvailableAfter)
        ok(0, "SystemPerformanceInformation failed\n");
    else
        ok(AvailableAfter + Rounds / 2 > AvailableBefore,
           "Available pages went from %lu to %lu in %lu rounds\n", AvailableBefore, AvailableAfter, Rounds);

    /* A locked page made inaccessible, then decommitted */
    Address = Base;
    Size = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Bytes[0] = 1;
        if (VirtualLock(Base, PAGE_SIZE))
        {
            Address = Base;
            Size = PAGE_SIZE;
            Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_NOACCESS, &Old);
            /* Windows unlocks the page here or on the decommit, and says so */
            ok(Status == STATUS_SUCCESS || Status == STATUS_WAS_UNLOCKED, "Protecting a locked page returned 0x%08lx\n", Status);
            Address = Base;
            Size = PAGE_SIZE;
            Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_DECOMMIT);
            ok(Status == STATUS_SUCCESS || Status == STATUS_WAS_UNLOCKED, "Decommitting a locked page returned 0x%08lx\n", Status);
            Status = NtQueryVirtualMemory(NtCurrentProcess(), Base, MemoryBasicInformation, &Mbi, sizeof(Mbi), NULL);
            ok_ntstatus(Status, STATUS_SUCCESS);
            ok_hex(Mbi.State, MEM_RESERVE);
            Address = Base;
            Size = PAGE_SIZE;
            Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
            ok_ntstatus(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
                ok(Bytes[0] == 0, "Locked page holds %u after decommit and commit\n", Bytes[0]);
        }
        else
        {
            skip("VirtualLock failed with %lu\n", GetLastError());
        }
    }

    Address = Base;
    Size = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);
}

static ULONG QueryAvailablePages(void)
{
    ULONG Buffer[256];
    PSYSTEM_PERFORMANCE_INFORMATION Info = (PSYSTEM_PERFORMANCE_INFORMATION)Buffer;
    ULONG Length = 0;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemPerformanceInformation, Buffer, sizeof(Buffer), &Length);
    if (!NT_SUCCESS(Status) || Length < FIELD_OFFSET(SYSTEM_PERFORMANCE_INFORMATION, AvailablePages) + sizeof(ULONG))
        return 0;
    return Info->AvailablePages;
}

/* How LockedPagesLeakCheck gets rid of the locked pages */
typedef enum _LOCKED_PAGES_END
{
    LockedRelease,              /* MEM_RELEASE */
    LockedDecommit,             /* MEM_DECOMMIT, then MEM_RELEASE */
    LockedNoAccessRelease       /* PAGE_NOACCESS (pages in transition), then MEM_RELEASE */
} LOCKED_PAGES_END;

static void LockedPagesLeakCheck(LOCKED_PAGES_END End)
{
    const ULONG PageCount = 16, Rounds = 128;
    ULONG i, j, Old, AvailableBefore, AvailableAfter;
    PVOID Base, Address;
    SIZE_T Size;
    PUCHAR Bytes;
    NTSTATUS Status, ReleaseStatus;

    AvailableBefore = QueryAvailablePages();
    for (i = 0; i < Rounds; i++)
    {
        Base = NULL;
        Size = PageCount * PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!NT_SUCCESS(Status))
            break;
        Bytes = Base;
        for (j = 0; j < PageCount; j++)
            Bytes[j * PAGE_SIZE] = 1;
        if (!VirtualLock(Base, PageCount * PAGE_SIZE))
        {
            trace("VirtualLock failed with %lu\n", GetLastError());
            Status = STATUS_UNSUCCESSFUL;
        }
        if (NT_SUCCESS(Status) && End == LockedDecommit)
        {
            Address = Base;
            Size = PageCount * PAGE_SIZE;
            Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_DECOMMIT);
            if (Status == STATUS_WAS_UNLOCKED)
                Status = STATUS_SUCCESS;
        }
        if (NT_SUCCESS(Status) && End == LockedNoAccessRelease)
        {
            Address = Base;
            Size = PageCount * PAGE_SIZE;
            Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_NOACCESS, &Old);
        }
        Size = 0;
        ReleaseStatus = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
        if (NT_SUCCESS(Status) && !NT_SUCCESS(ReleaseStatus))
            Status = ReleaseStatus;
        if (!NT_SUCCESS(Status))
            break;
    }
    ok(i == Rounds, "Round %lu (mode %d) failed with 0x%08lx\n", i, End, Status);
    AvailableAfter = QueryAvailablePages();
    /* Other activity moves the count on Windows: check the loss on ReactOS only */
    if (!is_reactos())
        skip("Not checking the available page count\n");
    else
        ok(AvailableBefore && AvailableAfter && AvailableAfter + PageCount * Rounds / 4 > AvailableBefore,
           "Available pages went from %lu to %lu in %lu rounds of %lu locked pages (mode %d)\n",
           AvailableBefore, AvailableAfter, Rounds, PageCount, End);
}

/* Pages locked with VirtualLock must be freed when they are released or decommitted */
static void Test_NtFreeVirtualMemory_LockedPages(void)
{
    ULONG Old;
    PVOID Base, Address;
    SIZE_T Size;
    MEMORY_BASIC_INFORMATION Mbi;
    PUCHAR Bytes;
    NTSTATUS Status;

    /* Locked, then made inaccessible, then released */
    Base = NULL;
    Size = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Bytes = Base;
        Bytes[0] = 1;
        ok(VirtualLock(Base, PAGE_SIZE), "VirtualLock failed with %lu\n", GetLastError());
        /* Making it inaccessible unlocks it, for good */
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_NOACCESS, &Old);
        ok_ntstatus(Status, STATUS_WAS_UNLOCKED);
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READWRITE, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(Bytes[0] == 1, "Page holds %u\n", Bytes[0]);
        SetLastError(0xdeadbeef);
        ok(!VirtualUnlock(Base, PAGE_SIZE) && GetLastError() == ERROR_NOT_LOCKED,
           "VirtualUnlock of a page that was made inaccessible: error %lu\n", GetLastError());
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_NOACCESS, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }

    /* Locked, then made a guard page, which unlocks it too */
    Base = NULL;
    Size = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Bytes = Base;
        Bytes[0] = 1;
        ok(VirtualLock(Base, PAGE_SIZE), "VirtualLock failed with %lu\n", GetLastError());
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READWRITE | PAGE_GUARD, &Old);
        ok_ntstatus(Status, STATUS_WAS_UNLOCKED);
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READWRITE, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(Bytes[0] == 1, "Page holds %u\n", Bytes[0]);
        SetLastError(0xdeadbeef);
        ok(!VirtualUnlock(Base, PAGE_SIZE) && GetLastError() == ERROR_NOT_LOCKED,
           "VirtualUnlock of a page that was made a guard page: error %lu\n", GetLastError());
        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }

    /* VirtualUnlock fails on a page that is not locked, and on a second unlock */
    Base = NULL;
    Size = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Bytes = Base;
        Bytes[0] = 1;
        SetLastError(0xdeadbeef);
        ok(!VirtualUnlock(Base, PAGE_SIZE) && GetLastError() == ERROR_NOT_LOCKED,
           "VirtualUnlock of a page that was never locked: error %lu\n", GetLastError());
        ok(VirtualLock(Base, PAGE_SIZE), "VirtualLock failed with %lu\n", GetLastError());
        ok(VirtualUnlock(Base, PAGE_SIZE), "VirtualUnlock failed with %lu\n", GetLastError());
        SetLastError(0xdeadbeef);
        ok(!VirtualUnlock(Base, PAGE_SIZE) && GetLastError() == ERROR_NOT_LOCKED,
           "Second VirtualUnlock: error %lu\n", GetLastError());
        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }

    /* Locked, then decommitted: reserved, and zero once committed again */
    Base = NULL;
    Size = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Bytes = Base;
        Bytes[0] = 1;
        ok(VirtualLock(Base, PAGE_SIZE), "VirtualLock failed with %lu\n", GetLastError());
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_DECOMMIT);
        /* Windows unlocks the page and says so */
        ok(Status == STATUS_SUCCESS || Status == STATUS_WAS_UNLOCKED, "Decommitting a locked page returned 0x%08lx\n", Status);
        Status = NtQueryVirtualMemory(NtCurrentProcess(), Base, MemoryBasicInformation, &Mbi, sizeof(Mbi), NULL);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_hex(Mbi.State, MEM_RESERVE);
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Address, 0, &Size, MEM_COMMIT, PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ok(Bytes[0] == 0, "Locked page holds %u after decommit and commit\n", Bytes[0]);
        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }

    /*
     * Allocate, touch and lock, then release, or decommit and release, or
     * make inaccessible and release: rough checks of the system-wide count,
     * which a leak fails on a quiet system
     */
    LockedPagesLeakCheck(LockedRelease);
    LockedPagesLeakCheck(LockedDecommit);
    LockedPagesLeakCheck(LockedNoAccessRelease);
}

START_TEST(NtFreeVirtualMemory)
{
    Test_NtFreeVirtualMemory();
    Test_NtFreeVirtualMemory_Parameters();
    Test_NtFreeVirtualMemory_DecommitInaccessible();
    Test_NtFreeVirtualMemory_LockedPages();
}
