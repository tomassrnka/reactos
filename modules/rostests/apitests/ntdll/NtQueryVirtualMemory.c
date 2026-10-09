/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQueryVirtualMemory
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static
VOID
CheckViewPage(
    PVOID BaseAddress,
    PVOID Address,
    ULONG State,
    ULONG Protect,
    SIZE_T RegionSize)
{
    NTSTATUS Status;
    MEMORY_BASIC_INFORMATION Info;
    SIZE_T ReturnLength = 0;

    RtlFillMemory(&Info, sizeof(Info), 0x55);
    Status = NtQueryVirtualMemory(NtCurrentProcess(),
                                  Address,
                                  MemoryBasicInformation,
                                  &Info,
                                  sizeof(Info),
                                  &ReturnLength);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_size_t(ReturnLength, sizeof(Info));
    ok_ptr(Info.BaseAddress, Address);
    ok_ptr(Info.AllocationBase, BaseAddress);
    ok_hex(Info.State, State);
    ok_hex(Info.Protect, Protect);
    ok_hex(Info.Type, MEM_MAPPED);
    ok_size_t(Info.RegionSize, RegionSize);
}

static
VOID
Test_SecReserveAccessedPage(VOID)
{
    NTSTATUS Status;
    HANDLE hSection;
    LARGE_INTEGER MaximumSize;
    PVOID BaseAddress = NULL;
    PVOID CommitAddress;
    SIZE_T ViewSize = 0;
    SIZE_T CommitSize;
    PUCHAR Page1;
    volatile UCHAR Value = 0;

    MaximumSize.QuadPart = 3 * PAGE_SIZE;
    Status = NtCreateSection(&hSection,
                             SECTION_ALL_ACCESS,
                             NULL,
                             &MaximumSize,
                             PAGE_READWRITE,
                             SEC_RESERVE,
                             NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtMapViewOfSection(hSection,
                                NtCurrentProcess(),
                                &BaseAddress,
                                0,
                                0,
                                NULL,
                                &ViewSize,
                                ViewUnmap,
                                0,
                                PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtClose(hSection);
        return;
    }
    ok_size_t(ViewSize, 3 * PAGE_SIZE);

    /* Pages 1 and 2 of the view are reserved */
    Page1 = (PUCHAR)BaseAddress + PAGE_SIZE;
    CheckViewPage(BaseAddress, Page1, MEM_RESERVE, 0, 2 * PAGE_SIZE);

    /* Touching a reserved page faults and leaves a prototype-format PTE */
    Status = STATUS_SUCCESS;
    _SEH2_TRY
    {
        Value = *Page1;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);

    /* The page is still reserved, like the one after it */
    CheckViewPage(BaseAddress, Page1, MEM_RESERVE, 0, 2 * PAGE_SIZE);

    /* Commit the touched page: now it is committed on its own */
    CommitAddress = Page1;
    CommitSize = PAGE_SIZE;
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &CommitAddress,
                                     0,
                                     &CommitSize,
                                     MEM_COMMIT,
                                     PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        CheckViewPage(BaseAddress, Page1, MEM_COMMIT, PAGE_READWRITE, PAGE_SIZE);

        Status = STATUS_SUCCESS;
        _SEH2_TRY
        {
            *Page1 = 0x5A;
            Value = *Page1;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_hex(Value, 0x5A);
    }

    Status = NtUnmapViewOfSection(NtCurrentProcess(), BaseAddress);
    ok_ntstatus(Status, STATUS_SUCCESS);
    NtClose(hSection);
}

START_TEST(NtQueryVirtualMemory)
{
    Test_SecReserveAccessedPage();
}
