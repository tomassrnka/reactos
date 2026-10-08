/*
 * PROJECT:         ReactOS API Tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for the NtProtectVirtualMemory API
 * PROGRAMMERS:     Jérôme Gardou <jerome.gardou@reactos.org>
 *                  Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"

static
void
TestReadWrite(void)
{
    ULONG* allocationStart = NULL;
    NTSTATUS status;
    SIZE_T allocationSize;
    ULONG oldProtection;

    /* Reserve a page */
    allocationSize = PAGE_SIZE;
    status = NtAllocateVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        0,
        &allocationSize,
        MEM_RESERVE,
        PAGE_NOACCESS);
    ok(NT_SUCCESS(status), "Reserving memory failed\n");

    /* Commit the page (RW) */
    status = NtAllocateVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        0,
        &allocationSize,
        MEM_COMMIT,
        PAGE_READWRITE);
    ok(NT_SUCCESS(status), "Commiting memory failed\n");

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xFF;
    } EndSeh(STATUS_SUCCESS);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0xFF, "Memory was not written\n");
    } EndSeh(STATUS_SUCCESS);

    /* Set it as read only */
    status = NtProtectVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        PAGE_READONLY,
        &oldProtection);
    ok(NT_SUCCESS(status), "NtProtectVirtualMemory failed.\n");
    ok(oldProtection == PAGE_READWRITE, "Expected PAGE_READWRITE, got %08lx.\n", oldProtection);

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xAA;
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0xFF, "read-only memory were changed.\n");
    } EndSeh(STATUS_SUCCESS);

    /* Set it as no access */
    status = NtProtectVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        PAGE_NOACCESS,
        &oldProtection);
    ok(NT_SUCCESS(status), "NtProtectVirtualMemory failed.\n");
    ok(oldProtection == PAGE_READONLY, "Expected PAGE_READONLY, got %08lx.\n", oldProtection);

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xAA;
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0, "Test should not go as far as this.\n");
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Set it as readable again */
    status = NtProtectVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        PAGE_READONLY,
        &oldProtection);
    ok(NT_SUCCESS(status), "NtProtectVirtualMemory failed.\n");
    ok(oldProtection == PAGE_NOACCESS, "Expected PAGE_READONLY, got %08lx.\n", oldProtection);

    /* Try writing it */
    StartSeh()
    {
        *allocationStart = 0xAA;
    } EndSeh(STATUS_ACCESS_VIOLATION);

    /* Try reading it */
    StartSeh()
    {
        ok(*allocationStart == 0xFF, "Memory content was not preserved.\n");
    } EndSeh(STATUS_SUCCESS);

    /* Free memory */
    status = NtFreeVirtualMemory(NtCurrentProcess(),
        (void**)&allocationStart,
        &allocationSize,
        MEM_RELEASE);
    ok(NT_SUCCESS(status), "Failed freeing memory.\n");
}

/* Regression test for CORE-13311 */
static
void
TestFreeNoAccess(void)
{
    PVOID Mem;
    SIZE_T Size;
    NTSTATUS Status;
    ULONG Iteration, PageNumber;
    PUCHAR Page;
    ULONG OldProtection;

    for (Iteration = 0; Iteration < 50000; Iteration++)
    {
        Mem = NULL;
        Size = 16 * PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                         &Mem,
                                         0,
                                         &Size,
                                         MEM_COMMIT,
                                         PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
        {
            break;
        }

        for (PageNumber = 0; PageNumber < 16; PageNumber++)
        {
            Page = Mem;
            Page += PageNumber * PAGE_SIZE;
            ok(*Page == 0,
               "[%lu, %lu] Got non-zero memory. %x at %p\n",
               Iteration, PageNumber, *Page, Page);
            *Page = 123;
        }

        Status = NtProtectVirtualMemory(NtCurrentProcess(),
                                        &Mem,
                                        &Size,
                                        PAGE_NOACCESS,
                                        &OldProtection);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok_hex(OldProtection, PAGE_READWRITE);

        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(),
                                     &Mem,
                                     &Size,
                                     MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }
}

/*
 * A touched page made inaccessible and then accessible again is brought back
 * from the page lists by the next access; it must keep its protection
 */
static
void
TestProtectionAfterInaccessible(void)
{
    static const ULONG Inaccessible[] = { PAGE_NOACCESS, PAGE_READWRITE | PAGE_GUARD };
    static const ULONG Final[] = { PAGE_READWRITE, PAGE_READONLY, PAGE_EXECUTE_READWRITE };
    MEMORY_BASIC_INFORMATION Mbi;
    volatile UCHAR *Page;
    PVOID Base, Address;
    SIZE_T Size;
    ULONG i, j, Old, Locked;
    BOOLEAN StillLocked;
    UCHAR Value = 0;
    NTSTATUS Status;

    for (Locked = 0; Locked < 2; Locked++)
    for (i = 0; i < _countof(Inaccessible); i++)
    for (j = 0; j < _countof(Final); j++)
    {
        Base = NULL;
        Size = PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            continue;
        Page = Base;
        Page[0] = 0x5A;
        Value = 0;
        /* A locked page is kept off the page lists while it is inaccessible */
        if (Locked)
            ok(VirtualLock(Base, PAGE_SIZE), "VirtualLock failed with %lu\n", GetLastError());

        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, Inaccessible[i], &Old);
        /* Windows unlocks the page here */
        ok(Status == STATUS_SUCCESS || (Locked && Status == STATUS_WAS_UNLOCKED),
           "%lx: NtProtectVirtualMemory returned 0x%08lx\n", Inaccessible[i], Status);
        StillLocked = (Locked && Status == STATUS_SUCCESS);
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, Final[j], &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(Old == Inaccessible[i], "%lx -> %lx: old protection %lx\n", Inaccessible[i], Final[j], Old);

        /* The access brings the page back */
        StartSeh()
        {
            Value = Page[0];
        } EndSeh(STATUS_SUCCESS);
        ok(Value == 0x5A, "%lx -> %lx: page holds %x\n", Inaccessible[i], Final[j], Value);

        Status = NtQueryVirtualMemory(NtCurrentProcess(), Base, MemoryBasicInformation, &Mbi, sizeof(Mbi), NULL);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(Mbi.Protect == Final[j], "%lx -> %lx: query reports %lx after the access\n", Inaccessible[i], Final[j], Mbi.Protect);
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READONLY, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(Old == Final[j], "%lx -> %lx: old protection %lx after the access\n", Inaccessible[i], Final[j], Old);
        if (StillLocked)
            ok(VirtualUnlock(Base, PAGE_SIZE), "VirtualUnlock failed with %lu\n", GetLastError());

        Address = Base;
        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }
}

/*
 * A touched guard page goes into transition; the first access consumes the
 * guard and the next one brings the page back: it must have the protection
 * without the guard
 */
static
void
TestProtectionAfterGuard(void)
{
    static const ULONG First[] = { PAGE_READWRITE, PAGE_NOACCESS };
    static const ULONG Guard[] = { PAGE_READWRITE | PAGE_GUARD, PAGE_READONLY | PAGE_GUARD, PAGE_EXECUTE_READ | PAGE_GUARD };
    MEMORY_BASIC_INFORMATION Mbi;
    volatile UCHAR *Page;
    PVOID Base, Address;
    SIZE_T Size;
    ULONG i, j, Old;
    UCHAR Value = 0;
    NTSTATUS Status;

    for (i = 0; i < _countof(First); i++)
    for (j = 0; j < _countof(Guard); j++)
    {
        Base = NULL;
        Size = PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            continue;
        Page = Base;
        Page[0] = 0x5A;
        Value = 0;

        /* Optionally inaccessible first, then a guard page */
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, First[i], &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, Guard[j], &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);

        StartSeh()
        {
            Value = Page[0];
        } EndSeh(STATUS_GUARD_PAGE_VIOLATION);
        StartSeh()
        {
            Value = Page[0];
        } EndSeh(STATUS_SUCCESS);
        ok(Value == 0x5A, "%lx -> %lx: page holds %x\n", First[i], Guard[j], Value);

        Status = NtQueryVirtualMemory(NtCurrentProcess(), Base, MemoryBasicInformation, &Mbi, sizeof(Mbi), NULL);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(Mbi.Protect == (Guard[j] & ~PAGE_GUARD), "%lx -> %lx: query reports %lx after the guard\n", First[i], Guard[j], Mbi.Protect);
        Address = Base;
        Size = PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READWRITE, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(Old == (Guard[j] & ~PAGE_GUARD), "%lx -> %lx: old protection %lx after the guard\n", First[i], Guard[j], Old);

        Address = Base;
        Size = 0;
        Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }
}

START_TEST(NtProtectVirtualMemory)
{
    TestReadWrite();
    TestFreeNoAccess();
    TestProtectionAfterInaccessible();
    TestProtectionAfterGuard();
}
