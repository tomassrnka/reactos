/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test virtual memory calls on ranges that cross an empty PXE
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifdef _WIN64

/* One PXE maps 512 GB, one PPE 1 GB */
#define PXE_SPAN ((ULONG_PTR)1 << 39)
#define PPE_SPAN ((ULONG_PTR)1 << 30)

static
PVOID
CommitPage(ULONG_PTR Address)
{
    PVOID Base = (PVOID)Address;
    SIZE_T Size = PAGE_SIZE;
    NTSTATUS Status;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size,
                                     MEM_COMMIT, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;

    /* Make the page tables exist */
    *(volatile ULONG *)Base = 1;
    return Base;
}

static
PVOID
Reserve(ULONG_PTR Address, SIZE_T Size, ULONG Type)
{
    PVOID Base = (PVOID)Address;
    NTSTATUS Status;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size,
                                     Type, PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status) ? Base : NULL;
}

static
VOID
Release(PVOID Base)
{
    SIZE_T Size = 0;
    NTSTATUS Status;

    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);
}

#endif /* _WIN64 */

/* NtProtectVirtualMemory checks that a committed range is committed */
START_TEST(EmptyPxeProtect)
{
#ifdef _WIN64
    ULONG_PTR Boundary = 10 * PXE_SPAN;
    PVOID Helper, Base, ProtectBase;
    SIZE_T ProtectSize;
    MEMORY_BASIC_INFORMATION Info;
    ULONG OldProtect;
    NTSTATUS Status;

    /* Make the PXE below the boundary valid, but not the PPE that maps the range */
    Helper = Reserve(Boundary - PXE_SPAN, 0x10000, MEM_RESERVE);
    if (!Helper)
        return;
    CommitPage((ULONG_PTR)Helper);

    /* A committed range across the boundary, never touched */
    Base = Reserve(Boundary - 0x10000, 0x20000, MEM_RESERVE | MEM_COMMIT);
    if (Base)
    {
        ProtectBase = Base;
        ProtectSize = 0x20000;
        OldProtect = 0;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &ProtectBase, &ProtectSize,
                                        PAGE_READONLY, &OldProtect);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_long(OldProtect, PAGE_READWRITE);

        Status = NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)(Boundary + 0x8000),
                                      MemoryBasicInformation, &Info, sizeof(Info), NULL);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_long(Info.State, MEM_COMMIT);
        ok_long(Info.Protect, PAGE_READONLY);

        Release(Base);
    }

    Release(Helper);
#else
    skip("Only 64-bit page tables have PXEs\n");
#endif
}

/* NtFreeVirtualMemory computes the commit charge of a released range */
START_TEST(EmptyPxeFree)
{
#ifdef _WIN64
    ULONG_PTR Boundary = 12 * PXE_SPAN;
    ULONG_PTR First, Rest;
    PVOID Base, FreeBase;
    SIZE_T FreeSize;
    MEMORY_BASIC_INFORMATION Info;
    NTSTATUS Status;

    /*
     * A reservation that spans the whole empty PXE above the boundary.
     * The first page is in PPE 0x1EC of the PXE below: a walk that takes
     * the PDE after it for a PXE resumes at the PTE of address 0.
     */
    First = Boundary - PXE_SPAN + 0x1EC * PPE_SPAN;
    Rest = Boundary + PXE_SPAN + 0x10000;
    Base = Reserve(First, Rest + 0x10000 - First, MEM_RESERVE);
    if (!Base)
        return;
    CommitPage(First);
    CommitPage(Boundary + PXE_SPAN);

    /* Release the beginning, up to and with the second page */
    FreeBase = Base;
    FreeSize = Rest - First;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &FreeBase, &FreeSize, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);

    Status = NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)Rest,
                                  MemoryBasicInformation, &Info, sizeof(Info), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_ptr(Info.AllocationBase, (PVOID)Rest);
    ok_long(Info.State, MEM_RESERVE);

    Release((PVOID)Rest);
#else
    skip("Only 64-bit page tables have PXEs\n");
#endif
}
