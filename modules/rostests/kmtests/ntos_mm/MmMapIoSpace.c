/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite: PAT layout and memory types of device memory mappings
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#if defined(_M_IX86) || defined(_M_AMD64)

#define PTE_VALID           0x01
#define PTE_WRITE_THROUGH   0x08
#define PTE_CACHE_DISABLE   0x10
#define PTE_LARGE_PAGE      0x80
#define PTE_CACHE_BITS      (PTE_WRITE_THROUGH | PTE_CACHE_DISABLE)

#define IA32_PAT            0x277
#define PAT_TYPE_UC         0
#define PAT_TYPE_WC         1
#define PAT_TYPE_WB         6
#define PAT_TYPE_UCM        7

#define TEST_PAGES          3

static BOOLEAN g_IsReactOS;

#ifdef _M_IX86

#define PTE_BASE    0xC0000000
#define PDE_BASE    0xC0300000
#define PDE_BASE_PAE 0xC0600000

static
ULONGLONG
GetPteValue(PVOID Address)
{
    if (ExIsProcessorFeaturePresent(PF_PAE_ENABLED))
        return *(PULONGLONG)((((ULONG)Address >> 12) << 3) + PTE_BASE);
    return *(PULONG)((((ULONG)Address >> 12) << 2) + PTE_BASE);
}

#elif defined(_M_AMD64)

#define PTE_BASE    0xFFFFF68000000000ULL
#define PDE_BASE    0xFFFFF6FB40000000ULL
#define PPE_BASE    0xFFFFF6FB7DA00000ULL
#define PXE_BASE    0xFFFFF6FB7DBED000ULL
#define PXE_SELFMAP 0x1ED

static
ULONGLONG
GetPteValue(PVOID Address)
{
    ULONG64 Offset = ((ULONG64)Address >> 9) & (0xFFFFFFFFFULL << 3);
    return *(PULONGLONG)(PTE_BASE + Offset);
}

#endif

static
BOOLEAN
CpuHasPat(VOID)
{
    INT Regs[4];

    __cpuid(Regs, 1);
    return (Regs[3] & (1 << 16)) != 0;
}

/* Every active processor uses the documented layout: WB, WC, UC-, UC, twice */
static
VOID
TestPatLayout(VOID)
{
    static const UCHAR Expected[8] = { PAT_TYPE_WB, PAT_TYPE_WC, PAT_TYPE_UCM, PAT_TYPE_UC,
                                       PAT_TYPE_WB, PAT_TYPE_WC, PAT_TYPE_UCM, PAT_TYPE_UC };
    KAFFINITY Active = KeQueryActiveProcessors();
    ULONG Cpu, Entry;

    for (Cpu = 0; Cpu < sizeof(KAFFINITY) * 8; Cpu++)
    {
        ULONGLONG Pat;

        if (!(Active & ((KAFFINITY)1 << Cpu)))
            continue;

        KeSetSystemAffinityThread((KAFFINITY)1 << Cpu);
        Pat = __readmsr(IA32_PAT);
        KeRevertToUserAffinityThread();

        trace("CPU %lu: IA32_PAT = 0x%016I64x\n", Cpu, Pat);
        for (Entry = 0; Entry < 8; Entry++)
        {
            ok((UCHAR)((Pat >> (Entry * 8)) & 7) == Expected[Entry],
               "CPU %lu: PAT entry %lu is %u, expected %u\n",
               Cpu, Entry, (UCHAR)((Pat >> (Entry * 8)) & 7), Expected[Entry]);
        }
    }
}

static
BOOLEAN
IsRamPage(PPHYSICAL_MEMORY_RANGE Ranges, ULONGLONG Page)
{
    PPHYSICAL_MEMORY_RANGE Range;
    ULONGLONG Address = Page << PAGE_SHIFT;

    for (Range = Ranges; Range->BaseAddress.QuadPart || Range->NumberOfBytes.QuadPart; Range++)
    {
        if ((Address >= (ULONGLONG)Range->BaseAddress.QuadPart) &&
            (Address < (ULONGLONG)(Range->BaseAddress.QuadPart + Range->NumberOfBytes.QuadPart)))
            return TRUE;
    }
    return FALSE;
}

/* TEST_PAGES pages below 4 GB that no physical memory range covers, so Mm treats them as device memory.
   They are mapped but never touched */
static
BOOLEAN
FindDevicePages(PPHYSICAL_ADDRESS Device)
{
    PPHYSICAL_MEMORY_RANGE Ranges, Range;
    BOOLEAN Found = FALSE;
    ULONG i;

    Ranges = MmGetPhysicalMemoryRanges();
    if (!Ranges)
        return FALSE;

    for (Range = Ranges; Range->BaseAddress.QuadPart || Range->NumberOfBytes.QuadPart; Range++)
    {
        /* Past the firmware tables that often sit right above a range */
        LONGLONG Candidate = (Range->BaseAddress.QuadPart + Range->NumberOfBytes.QuadPart + 0xFFFFF) &
                             ~(LONGLONG)0xFFFFF;

        if (Candidate + TEST_PAGES * PAGE_SIZE > 0x100000000LL)
            continue;
        for (i = 0; i < TEST_PAGES; i++)
        {
            if (IsRamPage(Ranges, (Candidate >> PAGE_SHIFT) + i))
                break;
        }
        if (i < TEST_PAGES)
            continue;
        if (!Found || Candidate > Device->QuadPart)
        {
            Device->QuadPart = Candidate;
            Found = TRUE;
        }
    }

    ExFreePool(Ranges);
    return Found;
}

static
PVOID
MapIoPage(PHYSICAL_ADDRESS Physical, MEMORY_CACHING_TYPE CacheType)
{
    PVOID Mapping = MmMapIoSpace(Physical, PAGE_SIZE, CacheType);

    ok(Mapping != NULL, "MmMapIoSpace(0x%I64x, %d) failed\n", Physical.QuadPart, CacheType);
    return Mapping;
}

static
ULONG
CacheBits(PVOID Address)
{
    return (ULONG)(GetPteValue(Address) & PTE_CACHE_BITS);
}

static
ULONG
MappedCacheBits(PHYSICAL_ADDRESS Physical, MEMORY_CACHING_TYPE CacheType)
{
    PVOID Mapping;
    ULONG Bits;

    Mapping = MapIoPage(Physical, CacheType);
    if (!Mapping)
        return MAXULONG;

    Bits = CacheBits(Mapping);
    MmUnmapIoSpace(Mapping, PAGE_SIZE);
    return Bits;
}

/* An MDL over an existing mapping of device memory, mapped again with the same cache type */
static
VOID
TestMdlMapping(PHYSICAL_ADDRESS Physical, KPROCESSOR_MODE Mode, ULONG ExpectedBits)
{
    PVOID Kernel, Mapped = NULL;
    PMDL Mdl;

    Kernel = MapIoPage(Physical, MmWriteCombined);
    if (!Kernel)
        return;
    Mdl = IoAllocateMdl(Kernel, PAGE_SIZE, FALSE, FALSE, NULL);
    ok(Mdl != NULL, "IoAllocateMdl failed\n");
    if (Mdl)
    {
        MmBuildMdlForNonPagedPool(Mdl);
        ok(Mdl->MdlFlags & MDL_IO_SPACE, "MDL flags 0x%x, expected MDL_IO_SPACE\n", Mdl->MdlFlags);

        KmtStartSeh()
            Mapped = MmMapLockedPagesSpecifyCache(Mdl, Mode, MmWriteCombined, NULL, FALSE, NormalPagePriority);
        KmtEndSeh(STATUS_SUCCESS);
        ok(Mapped != NULL, "MmMapLockedPagesSpecifyCache(%d) failed\n", Mode);
        if (Mapped)
        {
            /* Both mappings have one type, whichever path asked first */
            ok_eq_hex(CacheBits(Mapped), ExpectedBits);
            ok_eq_hex(CacheBits(Kernel), ExpectedBits);
            MmUnmapLockedPages(Mapped, Mdl);
        }
        IoFreeMdl(Mdl);
    }
    MmUnmapIoSpace(Kernel, PAGE_SIZE);
}

static
VOID
TestIoSpaceCacheBits(BOOLEAN HasPat)
{
    const ULONG WcBits = HasPat ? PTE_WRITE_THROUGH : PTE_CACHE_DISABLE;
    PHYSICAL_ADDRESS Device, Page;
    PHYSICAL_ADDRESS Ram;
    PVOID Buffer, First, Second;

    if (!FindDevicePages(&Device))
    {
        skip(FALSE, "No device pages below 4 GB\n");
    }
    else
    {
        trace("Device pages 0x%I64x\n", Device.QuadPart);

        /* Page 0: each caching type alone */
        ok_eq_hex(MappedCacheBits(Device, MmCached), 0UL);
        ok_eq_hex(MappedCacheBits(Device, MmNonCached), (ULONG)PTE_CACHE_BITS);
        /* PAT entry 1, write-combining; PCD alone is UC- and stays uncached under an UC MTRR */
        ok_eq_hex(MappedCacheBits(Device, MmWriteCombined), WcBits);

        /* Page 0: an uncached mapping turns the write-combined one into UC-, and a write-combined request
           made while the uncached one lives gets UC-: never a WC alias of an UC page */
        First = MapIoPage(Device, MmWriteCombined);
        if (First)
        {
            ok_eq_hex(CacheBits(First), WcBits);
            Second = MapIoPage(Device, MmNonCached);
            if (Second)
            {
                ok_eq_hex(CacheBits(Second), (ULONG)PTE_CACHE_BITS);
                ok_eq_hex(CacheBits(First), (ULONG)PTE_CACHE_DISABLE);
                ok_eq_hex(MappedCacheBits(Device, MmWriteCombined), (ULONG)PTE_CACHE_DISABLE);
                MmUnmapIoSpace(Second, PAGE_SIZE);
            }
            MmUnmapIoSpace(First, PAGE_SIZE);
        }
        /* Nothing maps the page any more */
        ok_eq_hex(MappedCacheBits(Device, MmWriteCombined), WcBits);

        /* Page 1: a kernel MDL mapping with the same cache type gets the same type */
        Page.QuadPart = Device.QuadPart + PAGE_SIZE;
        TestMdlMapping(Page, KernelMode, WcBits);

        /* Page 2: a user mapping is never write-combined, so the kernel mapping follows it to UC-.
           User mappings keep their claim until reboot, so the page stays UC- afterwards */
        Page.QuadPart = Device.QuadPart + 2 * PAGE_SIZE;
        TestMdlMapping(Page, UserMode, PTE_CACHE_DISABLE);
        ok_eq_hex(MappedCacheBits(Page, MmWriteCombined), (ULONG)PTE_CACHE_DISABLE);
    }

    /* RAM is never mapped write-combined: another mapping of the page caches it */
    Buffer = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, 'TapK');
    if (!skip(Buffer != NULL, "No memory\n"))
    {
        Ram = MmGetPhysicalAddress(Buffer);
        ok_eq_hex(MappedCacheBits(Ram, MmCached), 0UL);
        ok_eq_hex(MappedCacheBits(Ram, MmWriteCombined), (ULONG)PTE_CACHE_DISABLE);
        ok_eq_hex(CacheBits(Buffer), 0UL);
        ExFreePoolWithTag(Buffer, 'TapK');
    }
}

typedef struct _ALIAS_SCAN
{
    PPHYSICAL_MEMORY_RANGE Ranges;
    PULONGLONG Entries;         /* page << 2 | PCD/PWT */
    ULONG Count;
    ULONG Max;
} ALIAS_SCAN, *PALIAS_SCAN;

static
VOID
AddPte(PALIAS_SCAN Scan, ULONGLONG Pte)
{
    ULONGLONG Page;

    if (!(Pte & PTE_VALID))
        return;
#ifdef _M_IX86
    if (ExIsProcessorFeaturePresent(PF_PAE_ENABLED))
        Page = (Pte >> PAGE_SHIFT) & 0xFFFFFFULL;
    else
        Page = (Pte >> PAGE_SHIFT) & 0xFFFFFULL;
#else
    Page = (Pte >> PAGE_SHIFT) & 0xFFFFFFFFFULL;
#endif
    if (IsRamPage(Scan->Ranges, Page))
        return;
    if (Scan->Count < Scan->Max)
        Scan->Entries[Scan->Count++] = (Page << 2) | ((Pte & PTE_CACHE_BITS) >> 3);
}

static
ULONGLONG
ReadEntry(PVOID Address, ULONG Size)
{
    ULONGLONG Value = 0;

    if (!MmIsAddressValid(Address))
        return 0;
    _SEH2_TRY
    {
        Value = (Size == 8) ? *(volatile ULONGLONG *)Address : *(volatile ULONG *)Address;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Value = 0;
    }
    _SEH2_END;
    return Value;
}

/* Every valid system-space PTE that maps device memory */
static
VOID
CollectSystemMappings(PALIAS_SCAN Scan)
{
#ifdef _M_IX86
    BOOLEAN Pae = ExIsProcessorFeaturePresent(PF_PAE_ENABLED);
    ULONG PageTableSpan = Pae ? (1UL << 21) : (1UL << 22);
    ULONG EntrySize = Pae ? 8 : 4;
    ULONG_PTR Va, Pte;
    ULONGLONG Pde;
    ULONG i;

    for (Va = 0x80000000; Va != 0; Va += PageTableSpan)
    {
        if (Pae)
            Pde = ReadEntry((PVOID)(PDE_BASE_PAE + (Va >> 21) * 8), 8);
        else
            Pde = ReadEntry((PVOID)(PDE_BASE + (Va >> 22) * 4), 4);
        if (!(Pde & PTE_VALID))
            continue;
        if (Pde & PTE_LARGE_PAGE)
        {
            AddPte(Scan, Pde);
            continue;
        }
        Pte = PTE_BASE + (Va >> PAGE_SHIFT) * EntrySize;
        for (i = 0; i < PageTableSpan / PAGE_SIZE; i++)
            AddPte(Scan, ReadEntry((PVOID)(Pte + i * EntrySize), EntrySize));
    }
#else
    ULONG64 Pxi, Ppi, Pdi, Pti, Va;
    ULONGLONG Entry;

    for (Pxi = 256; Pxi < 512; Pxi++)
    {
        if (Pxi == PXE_SELFMAP)
            continue;
        if (!(ReadEntry((PVOID)(PXE_BASE + Pxi * 8), 8) & PTE_VALID))
            continue;
        for (Ppi = 0; Ppi < 512; Ppi++)
        {
            Va = (Pxi << 39) | (Ppi << 30);
            Entry = ReadEntry((PVOID)(PPE_BASE + ((Va >> 30) & 0x3FFFF) * 8), 8);
            if (!(Entry & PTE_VALID) || (Entry & PTE_LARGE_PAGE))
                continue;
            for (Pdi = 0; Pdi < 512; Pdi++)
            {
                Va = (Pxi << 39) | (Ppi << 30) | (Pdi << 21);
                Entry = ReadEntry((PVOID)(PDE_BASE + ((Va >> 21) & 0x7FFFFFF) * 8), 8);
                if (!(Entry & PTE_VALID))
                    continue;
                if (Entry & PTE_LARGE_PAGE)
                {
                    AddPte(Scan, Entry);
                    continue;
                }
                for (Pti = 0; Pti < 512; Pti++)
                {
                    Va = (Pxi << 39) | (Ppi << 30) | (Pdi << 21) | (Pti << 12);
                    AddPte(Scan, ReadEntry((PVOID)(PTE_BASE + ((Va >> 12) & 0xFFFFFFFFFULL) * 8), 8));
                }
            }
        }
    }
#endif
}

/* With the PAT in use, no device page (the framebuffer above all) is mapped write-combined and with another type at once */
static
VOID
TestNoWriteCombinedAliases(VOID)
{
    ALIAS_SCAN Scan;
    ULONG i, j, Gap, WcPages = 0, Aliases = 0;
    ULONGLONG Temp;

    Scan.Max = 512 * 1024;
    Scan.Count = 0;
    Scan.Ranges = MmGetPhysicalMemoryRanges();
    Scan.Entries = ExAllocatePoolWithTag(NonPagedPool, Scan.Max * sizeof(ULONGLONG), 'TapK');
    if (skip(Scan.Ranges && Scan.Entries, "No memory\n"))
        goto Cleanup;

    CollectSystemMappings(&Scan);
    ok(Scan.Count < Scan.Max, "Too many device mappings: %lu\n", Scan.Count);

    /* Shell sort by page, then type */
    for (Gap = Scan.Count / 2; Gap > 0; Gap /= 2)
    {
        for (i = Gap; i < Scan.Count; i++)
        {
            Temp = Scan.Entries[i];
            for (j = i; (j >= Gap) && (Scan.Entries[j - Gap] > Temp); j -= Gap)
                Scan.Entries[j] = Scan.Entries[j - Gap];
            Scan.Entries[j] = Temp;
        }
    }

    for (i = 0; i < Scan.Count; i = j)
    {
        ULONG Types = 0;

        for (j = i; (j < Scan.Count) && ((Scan.Entries[j] >> 2) == (Scan.Entries[i] >> 2)); j++)
            Types |= 1 << (ULONG)(Scan.Entries[j] & 3);
        if (Types & (1 << 1))
        {
            WcPages++;
            if (Types != (1 << 1))
            {
                if (Aliases++ < 10)
                    trace("Page 0x%I64x is write-combined and also mapped with types 0x%lx\n",
                          Scan.Entries[i] >> 2, Types);
            }
        }
    }

    trace("%lu device PTEs, %lu write-combined device pages\n", Scan.Count, WcPages);
    ok_eq_ulong(Aliases, 0UL);

Cleanup:
    if (Scan.Entries)
        ExFreePoolWithTag(Scan.Entries, 'TapK');
    if (Scan.Ranges)
        ExFreePool(Scan.Ranges);
}

START_TEST(MmMapIoSpace)
{
    BOOLEAN HasPat;

    g_IsReactOS = *(PULONG)(KI_USER_SHARED_DATA + PAGE_SIZE - sizeof(ULONG)) == 0x8eac705;
    HasPat = CpuHasPat();

    if (!skip(HasPat, "The processor has no PAT\n"))
        TestPatLayout();

    /* Windows randomizes the PTE base */
    if (!skip(g_IsReactOS, "PTE checks run on ReactOS only\n"))
    {
        TestIoSpaceCacheBits(HasPat);
        TestNoWriteCombinedAliases();
    }
}

#else

START_TEST(MmMapIoSpace)
{
    skip(FALSE, "x86 and x64 only\n");
}

#endif
