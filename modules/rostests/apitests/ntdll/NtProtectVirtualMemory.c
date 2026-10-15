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

#ifdef _WIN64
#define TOP_LEVEL_REGION_SIZE 0x8000000000ULL

/*
 * Commit 16 pages in a 512 GB region that this process never used, so that
 * it has no page map level 4 entry. Returns NULL if no such region is free.
 */
static
PVOID
AllocateInUntouchedTopLevelRegion(ULONG_PTR *Next)
{
    static const ULONG_PTR Candidates[] = { 0x50000000000ULL, 0x58000000000ULL, 0x60000000000ULL,
                                            0x68000000000ULL, 0x70000000000ULL, 0x78000000000ULL };
    MEMORY_BASIC_INFORMATION Mbi;
    PVOID Base;
    SIZE_T Size;
    NTSTATUS Status;

    for (; *Next < _countof(Candidates); (*Next)++)
    {
        Status = NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)Candidates[*Next],
                                      MemoryBasicInformation, &Mbi, sizeof(Mbi), NULL);
        if (!NT_SUCCESS(Status) || Mbi.State != MEM_FREE ||
            (ULONG_PTR)Mbi.BaseAddress + Mbi.RegionSize < Candidates[*Next] + TOP_LEVEL_REGION_SIZE)
            continue;
        Base = (PVOID)Candidates[*Next];
        Size = 16 * PAGE_SIZE;
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (NT_SUCCESS(Status))
        {
            (*Next)++;
            return Base;
        }
    }
    return NULL;
}

/*
 * Committed memory that was never touched has no page tables. Releasing part
 * of it counts its committed pages, and protecting it checks that the whole
 * range is committed: both walks skip the absent top-level entry, which used
 * to make them continue from a PTE address computed from the wrong pointer.
 */
static
void
TestUntouchedTopLevelRegion(void)
{
    MEMORY_BASIC_INFORMATION Mbi;
    ULONG_PTR Next = 0;
    PVOID Base, Address;
    SIZE_T Size;
    ULONG OldProtect;
    NTSTATUS Status;

    /* Release the middle of an untouched allocation */
    Base = AllocateInUntouchedTopLevelRegion(&Next);
    if (!Base)
    {
        skip("No untouched 512 GB region is free\n");
        return;
    }
    Address = (PUCHAR)Base + 4 * PAGE_SIZE;
    Size = 4 * PAGE_SIZE;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Status = NtQueryVirtualMemory(NtCurrentProcess(), (PUCHAR)Base + 4 * PAGE_SIZE,
                                  MemoryBasicInformation, &Mbi, sizeof(Mbi), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(Mbi.State, MEM_FREE);
    Address = Base;
    Size = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    Address = (PUCHAR)Base + 8 * PAGE_SIZE;
    Size = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);

    /* Protect an untouched allocation in another region */
    Base = AllocateInUntouchedTopLevelRegion(&Next);
    if (!Base)
    {
        skip("No second untouched 512 GB region is free\n");
        return;
    }
    Address = Base;
    Size = 16 * PAGE_SIZE;
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &Address, &Size, PAGE_READONLY, &OldProtect);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(OldProtect, PAGE_READWRITE);
    Address = Base;
    Size = 0;
    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Address, &Size, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);
}
#endif

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

#define CACHE_REGION_PAGES  16
#define CACHE_READ_PASSES   2000
#define CACHE_LOOP_COUNT    1000000UL

/* mov ecx, imm32; xor eax, eax; L: add eax, 3; dec ecx; jnz L; ret (same bytes in 32 and 64 bit mode) */
static const UCHAR LoopCode[] = { 0xB9, 0, 0, 0, 0, 0x31, 0xC0, 0x83, 0xC0, 0x03, 0xFF, 0xC9, 0x75, 0xF9, 0xC3 };

static
ULONGLONG
Ticks(void)
{
    LARGE_INTEGER Counter;

    NtQueryPerformanceCounter(&Counter, NULL);
    return Counter.QuadPart;
}

/* Best of three runs, so a preemption in one run does not decide the verdict */
static
ULONGLONG
TimeReads(
    _In_ volatile UCHAR *Base)
{
    ULONGLONG Best = (ULONGLONG)-1, Start, Elapsed;
    ULONG Run, Pass;
    SIZE_T Offset;
    static volatile ULONG Sink;
    ULONG Sum = 0;

    for (Run = 0; Run < 3; Run++)
    {
        Start = Ticks();
        for (Pass = 0; Pass < CACHE_READ_PASSES; Pass++)
            for (Offset = 0; Offset < CACHE_REGION_PAGES * PAGE_SIZE; Offset += 64)
                Sum += Base[Offset];
        Elapsed = Ticks() - Start;
        if (Elapsed < Best)
            Best = Elapsed;
    }
    Sink += Sum;
    return Best;
}

static
ULONGLONG
TimeCode(
    _In_ PVOID Code)
{
    ULONGLONG Best = (ULONGLONG)-1, Start, Elapsed;
    ULONG Run;

    for (Run = 0; Run < 3; Run++)
    {
        Start = Ticks();
        ((int (*)(void))Code)();
        Elapsed = Ticks() - Start;
        if (Elapsed < Best)
            Best = Elapsed;
    }
    return Best;
}

static
PVOID
AllocateCodeRegion(
    _In_ ULONG Protection)
{
    PVOID Base = NULL;
    SIZE_T Size = CACHE_REGION_PAGES * PAGE_SIZE;
    ULONG Count = CACHE_LOOP_COUNT, Page;
    NTSTATUS Status;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE | MEM_COMMIT, Protection);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return NULL;
    RtlCopyMemory(Base, LoopCode, sizeof(LoopCode));
    RtlCopyMemory((PUCHAR)Base + 1, &Count, sizeof(Count));
    for (Page = 0; Page < CACHE_REGION_PAGES; Page++)
        ((volatile UCHAR *)Base)[Page * PAGE_SIZE + 100] = 0;
    return Base;
}

static
void
FreeRegion(
    _In_ PVOID Base)
{
    SIZE_T Size = 0;
    NTSTATUS Status;

    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
    ok_ntstatus(Status, STATUS_SUCCESS);
}

/* Pages that were present when their protection changed must stay cached */
static
void
TestCachingAfterProtect(void)
{
    static const struct
    {
        ULONG First;
        ULONG Second;
        ULONG Third;
        PCSTR Name;
    } Cases[] =
    {
        { PAGE_READWRITE, PAGE_READONLY, 0, "RW to RO" },
        { PAGE_READWRITE, PAGE_EXECUTE_READ, 0, "RW to RX" },
        { PAGE_READWRITE, PAGE_EXECUTE_READ, PAGE_READWRITE, "RW to RX to RW" },
        { PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_READ, 0, "RWX to RX" },
    };
    PVOID Control, Base;
    SIZE_T Size;
    ULONG Old;
    ULONGLONG ControlReads, ControlCode, Reads, Code;
    NTSTATUS Status;
    ULONG i;
    BOOLEAN Executable;

    Control = AllocateCodeRegion(PAGE_EXECUTE_READWRITE);
    if (!Control)
        return;
    ControlReads = TimeReads(Control);
    ControlCode = TimeCode(Control);

    for (i = 0; i < RTL_NUMBER_OF(Cases); i++)
    {
        Base = AllocateCodeRegion(Cases[i].First);
        if (!Base)
            continue;
        Size = CACHE_REGION_PAGES * PAGE_SIZE;
        Status = NtProtectVirtualMemory(NtCurrentProcess(), &Base, &Size, Cases[i].Second, &Old);
        ok_ntstatus(Status, STATUS_SUCCESS);
        if (Cases[i].Third)
        {
            Size = CACHE_REGION_PAGES * PAGE_SIZE;
            Status = NtProtectVirtualMemory(NtCurrentProcess(), &Base, &Size, Cases[i].Third, &Old);
            ok_ntstatus(Status, STATUS_SUCCESS);
        }
        Executable = Cases[i].Third == 0 && Cases[i].Second == PAGE_EXECUTE_READ;

        Reads = TimeReads(Base);
        /* An uncached page takes about a hundred times longer, a cached one the same */
        ok(Reads < ControlReads * 10 + 1000, "%s: reading takes %I64u ticks, an untouched region takes %I64u\n",
           Cases[i].Name, Reads, ControlReads);
        if (Executable)
        {
            Code = TimeCode(Base);
            ok(Code < ControlCode * 10 + 1000, "%s: running code takes %I64u ticks, an untouched region takes %I64u\n",
               Cases[i].Name, Code, ControlCode);
        }
        FreeRegion(Base);
    }
    FreeRegion(Control);
}

START_TEST(NtProtectVirtualMemory)
{
    TestReadWrite();
    TestFreeNoAccess();
#ifdef _WIN64
    TestUntouchedTopLevelRegion();
#endif
    TestProtectionAfterInaccessible();
    TestProtectionAfterGuard();
    TestCachingAfterProtect();
}
