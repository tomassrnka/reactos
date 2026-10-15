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
#define PAT_TYPE_WT         4
#define PAT_TYPE_WB         6
#define PAT_TYPE_UCM        7

#define TEST_PAGES          8

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

/* The PAT layout is WB, WC, UC-, UC twice on every active processor if all of them have a PAT,
   else every processor keeps the power-on layout. Returns TRUE if write-combining is expected */
static
BOOLEAN
TestPatLayout(VOID)
{
    static const UCHAR Programmed[8] = { PAT_TYPE_WB, PAT_TYPE_WC, PAT_TYPE_UCM, PAT_TYPE_UC,
                                         PAT_TYPE_WB, PAT_TYPE_WC, PAT_TYPE_UCM, PAT_TYPE_UC };
    static const UCHAR PowerOn[8] = { PAT_TYPE_WB, PAT_TYPE_WT, PAT_TYPE_UCM, PAT_TYPE_UC,
                                      PAT_TYPE_WB, PAT_TYPE_WT, PAT_TYPE_UCM, PAT_TYPE_UC };
    KAFFINITY Active = KeQueryActiveProcessors(), HasPat = 0;
    ULONGLONG Pat[sizeof(KAFFINITY) * 8];
    const UCHAR *Expected;
    ULONG Cpu, Entry;

    for (Cpu = 0; Cpu < sizeof(KAFFINITY) * 8; Cpu++)
    {
        if (!(Active & ((KAFFINITY)1 << Cpu)))
            continue;
        KeSetSystemAffinityThread((KAFFINITY)1 << Cpu);
        if (CpuHasPat())
        {
            HasPat |= (KAFFINITY)1 << Cpu;
            Pat[Cpu] = __readmsr(IA32_PAT);
        }
        KeRevertToUserAffinityThread();
    }
    if (skip(HasPat != 0, "The processors have no PAT\n"))
        return FALSE;

    /* One processor without a PAT keeps the power-on layout on all of them */
    Expected = (HasPat == Active) ? Programmed : PowerOn;
    for (Cpu = 0; Cpu < sizeof(KAFFINITY) * 8; Cpu++)
    {
        if (!(HasPat & ((KAFFINITY)1 << Cpu)))
            continue;
        trace("CPU %lu: IA32_PAT = 0x%016I64x\n", Cpu, Pat[Cpu]);
        for (Entry = 0; Entry < 8; Entry++)
        {
            ok((UCHAR)((Pat[Cpu] >> (Entry * 8)) & 7) == Expected[Entry],
               "CPU %lu: PAT entry %lu is %u, expected %u\n",
               Cpu, Entry, (UCHAR)((Pat[Cpu] >> (Entry * 8)) & 7), Expected[Entry]);
        }
    }
    return (HasPat == Active);
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
MapIo(PHYSICAL_ADDRESS Device, ULONG Page, ULONG Pages, MEMORY_CACHING_TYPE CacheType)
{
    PHYSICAL_ADDRESS Physical;
    PVOID Mapping;

    Physical.QuadPart = Device.QuadPart + (LONGLONG)Page * PAGE_SIZE;
    Mapping = MmMapIoSpace(Physical, Pages * PAGE_SIZE, CacheType);
    ok(Mapping != NULL, "MmMapIoSpace(0x%I64x, %lu pages, %d) failed\n", Physical.QuadPart, Pages, CacheType);
    return Mapping;
}

static
ULONG
CacheBits(PVOID Address)
{
    return Address ? (ULONG)(GetPteValue(Address) & PTE_CACHE_BITS) : MAXULONG;
}

static
VOID
Unmap(PVOID Mapping, ULONG Pages)
{
    if (Mapping)
        MmUnmapIoSpace(Mapping, Pages * PAGE_SIZE);
}

static
ULONG
MappedCacheBits(PHYSICAL_ADDRESS Device, ULONG Page, MEMORY_CACHING_TYPE CacheType)
{
    PVOID Mapping = MapIo(Device, Page, 1, CacheType);
    ULONG Bits = CacheBits(Mapping);

    Unmap(Mapping, 1);
    return Bits;
}

#define WC_BITS     PTE_WRITE_THROUGH
#define UCM_BITS    PTE_CACHE_DISABLE
#define UC_BITS     PTE_CACHE_BITS

/* A live mapping never changes type, and no page is write-combined beside another type */
static
VOID
TestWriteCombiningRules(PHYSICAL_ADDRESS Device)
{
    PVOID A, B, C;

    /* Page 0: a later uncached request on a write-combined page gets write-combining too; once
       everything is unmapped the page is free again; WC beside a live UC mapping is UC- */
    A = MapIo(Device, 0, 1, MmWriteCombined);
    B = MapIo(Device, 0, 1, MmNonCached);
    ok_eq_hex(CacheBits(A), (ULONG)WC_BITS);
    ok_eq_hex(CacheBits(B), (ULONG)WC_BITS);
    Unmap(B, 1);
    ok_eq_hex(CacheBits(A), (ULONG)WC_BITS);
    Unmap(A, 1);
    A = MapIo(Device, 0, 1, MmNonCached);
    ok_eq_hex(CacheBits(A), (ULONG)UC_BITS);
    ok_eq_hex(MappedCacheBits(Device, 0, MmWriteCombined), (ULONG)UCM_BITS);
    ok_eq_hex(CacheBits(A), (ULONG)UC_BITS);
    Unmap(A, 1);
    ok_eq_hex(MappedCacheBits(Device, 0, MmWriteCombined), (ULONG)WC_BITS);

    /* Pages 1-2, partial overlap: UC on page 1, WC on page 2, then WC over both pages */
    A = MapIo(Device, 1, 1, MmNonCached);
    B = MapIo(Device, 2, 1, MmWriteCombined);
    C = MapIo(Device, 1, 2, MmWriteCombined);
    ok_eq_hex(CacheBits(A), (ULONG)UC_BITS);
    ok_eq_hex(CacheBits(B), (ULONG)WC_BITS);
    if (C)
    {
        ok_eq_hex(CacheBits(C), (ULONG)UCM_BITS);
        ok_eq_hex(CacheBits((PUCHAR)C + PAGE_SIZE), (ULONG)WC_BITS);
    }
    Unmap(C, 2);
    Unmap(B, 1);
    Unmap(A, 1);

    /* Pages 3-4, partial overlap: WC over both, WC on page 4, then UC on page 3, then UC over both
       once only page 4 is still mapped */
    A = MapIo(Device, 3, 2, MmWriteCombined);
    B = MapIo(Device, 4, 1, MmWriteCombined);
    C = MapIo(Device, 3, 1, MmNonCached);
    if (A)
    {
        ok_eq_hex(CacheBits(A), (ULONG)WC_BITS);
        ok_eq_hex(CacheBits((PUCHAR)A + PAGE_SIZE), (ULONG)WC_BITS);
    }
    ok_eq_hex(CacheBits(B), (ULONG)WC_BITS);
    ok_eq_hex(CacheBits(C), (ULONG)WC_BITS);
    Unmap(C, 1);
    Unmap(A, 2);
    C = MapIo(Device, 3, 2, MmNonCached);
    if (C)
    {
        ok_eq_hex(CacheBits(C), (ULONG)UC_BITS);
        ok_eq_hex(CacheBits((PUCHAR)C + PAGE_SIZE), (ULONG)WC_BITS);
    }
    ok_eq_hex(CacheBits(B), (ULONG)WC_BITS);
    Unmap(C, 2);
    Unmap(B, 1);

    /* Page 5, unmap order: each unmap gives the page back, so the next request gets its own type */
    A = MapIo(Device, 5, 1, MmNonCached);
    ok_eq_hex(CacheBits(A), (ULONG)UC_BITS);
    Unmap(A, 1);
    A = MapIo(Device, 5, 1, MmWriteCombined);
    ok_eq_hex(CacheBits(A), (ULONG)WC_BITS);
    Unmap(A, 1);
    A = MapIo(Device, 5, 1, MmCached);
    ok_eq_hex(CacheBits(A), 0UL);
    Unmap(A, 1);
    ok_eq_hex(MappedCacheBits(Device, 5, MmWriteCombined), (ULONG)WC_BITS);
}

/* A kernel MDL describing a write-combined device page, mapped with another cache type */
static
VOID
TestKernelMdl(PHYSICAL_ADDRESS Device)
{
    PVOID Kernel, Mapped;
    PMDL Mdl;

    Kernel = MapIo(Device, 6, 1, MmWriteCombined);
    if (!Kernel)
        return;
    Mdl = IoAllocateMdl(Kernel, PAGE_SIZE, FALSE, FALSE, NULL);
    ok(Mdl != NULL, "IoAllocateMdl failed\n");
    if (Mdl)
    {
        MmGetMdlPfnArray(Mdl)[0] = (PFN_NUMBER)((Device.QuadPart >> PAGE_SHIFT) + 6);
        Mdl->MdlFlags |= MDL_PAGES_LOCKED | MDL_IO_SPACE;
        Mapped = MmMapLockedPagesSpecifyCache(Mdl, KernelMode, MmNonCached, NULL, FALSE, NormalPagePriority);
        ok(Mapped != NULL, "MmMapLockedPagesSpecifyCache failed\n");
        if (Mapped)
        {
            ok_eq_hex(CacheBits(Mapped), (ULONG)WC_BITS);
            ok_eq_hex(CacheBits(Kernel), (ULONG)WC_BITS);
            MmUnmapLockedPages(Mapped, Mdl);
        }
        Mdl->MdlFlags &= ~(MDL_PAGES_LOCKED | MDL_IO_SPACE);
        IoFreeMdl(Mdl);
    }
    Unmap(Kernel, 1);
}

/* A user MDL mapping of a device page follows the page's type, and its unmap gives the page back */
static
VOID
TestUserMdl(PHYSICAL_ADDRESS Device, MEMORY_CACHING_TYPE KernelType, ULONG ExpectedUserBits)
{
    PVOID Kernel, Mapped = NULL;
    PMDL Mdl;

    Kernel = MapIo(Device, 7, 1, KernelType);
    if (!Kernel)
        return;
    Mdl = IoAllocateMdl(Kernel, PAGE_SIZE, FALSE, FALSE, NULL);
    ok(Mdl != NULL, "IoAllocateMdl failed\n");
    if (Mdl)
    {
        MmBuildMdlForNonPagedPool(Mdl);
        ok(Mdl->MdlFlags & MDL_IO_SPACE, "MDL flags 0x%x, expected MDL_IO_SPACE\n", Mdl->MdlFlags);
        KmtStartSeh()
            Mapped = MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmWriteCombined, NULL, FALSE, NormalPagePriority);
        KmtEndSeh(STATUS_SUCCESS);
        ok(Mapped != NULL, "MmMapLockedPagesSpecifyCache(UserMode) failed\n");
        if (Mapped)
        {
            ok_eq_hex(CacheBits(Mapped), ExpectedUserBits);
            MmUnmapLockedPages(Mapped, Mdl);
        }
        IoFreeMdl(Mdl);
    }
    Unmap(Kernel, 1);
    ok_eq_hex(MappedCacheBits(Device, 7, MmWriteCombined), (ULONG)WC_BITS);
}

static
VOID
TestIoSpaceCacheBits(BOOLEAN PatWc)
{
    const ULONG WcBits = PatWc ? WC_BITS : UCM_BITS;
    PHYSICAL_ADDRESS Device, Ram;
    PVOID Buffer;

    if (!FindDevicePages(&Device))
    {
        skip(FALSE, "No device pages below 4 GB\n");
    }
    else
    {
        trace("Device pages 0x%I64x\n", Device.QuadPart);
        ok_eq_hex(MappedCacheBits(Device, 0, MmCached), 0UL);
        ok_eq_hex(MappedCacheBits(Device, 0, MmNonCached), (ULONG)UC_BITS);
        /* PAT entry 1, write-combining; PCD alone is UC- and stays uncached under an UC MTRR */
        ok_eq_hex(MappedCacheBits(Device, 0, MmWriteCombined), WcBits);

        if (!skip(PatWc, "No PAT write-combining\n"))
        {
            TestWriteCombiningRules(Device);
            TestKernelMdl(Device);
            /* UC kernel mapping: the user WC request gets UC-; WC kernel mapping: the user one too */
            TestUserMdl(Device, MmNonCached, UCM_BITS);
            TestUserMdl(Device, MmWriteCombined, WC_BITS);
        }
    }

    /* RAM is never mapped write-combined: another mapping of the page caches it */
    Buffer = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, 'TapK');
    if (!skip(Buffer != NULL, "No memory\n"))
    {
        PVOID Mapping;

        Ram = MmGetPhysicalAddress(Buffer);
        Mapping = MmMapIoSpace(Ram, PAGE_SIZE, MmCached);
        ok_eq_hex(CacheBits(Mapping), 0UL);
        Unmap(Mapping, 1);
        Mapping = MmMapIoSpace(Ram, PAGE_SIZE, MmWriteCombined);
        ok_eq_hex(CacheBits(Mapping), (ULONG)UCM_BITS);
        Unmap(Mapping, 1);
        ok_eq_hex(CacheBits(Buffer), 0UL);
        ExFreePoolWithTag(Buffer, 'TapK');
    }
}

typedef struct _ALIAS_LARGE
{
    ULONGLONG BasePage;
    ULONGLONG PageCount;
    ULONG Type;
} ALIAS_LARGE;

typedef struct _ALIAS_SCAN
{
    PPHYSICAL_MEMORY_RANGE Ranges;
    PULONGLONG Entries;         /* page << 2 | PCD/PWT */
    ULONG Count;
    ULONG Max;
    ALIAS_LARGE Large[64];
    ULONG LargeCount;
    BOOLEAN Overflow;
} ALIAS_SCAN, *PALIAS_SCAN;

static
ULONGLONG
PteFrame(ULONGLONG Pte)
{
#ifdef _M_IX86
    if (ExIsProcessorFeaturePresent(PF_PAE_ENABLED))
        return (Pte >> PAGE_SHIFT) & 0xFFFFFFULL;
    return (Pte >> PAGE_SHIFT) & 0xFFFFFULL;
#else
    return (Pte >> PAGE_SHIFT) & 0xFFFFFFFFFULL;
#endif
}

static
VOID
AddPte(PALIAS_SCAN Scan, ULONGLONG Pte)
{
    ULONGLONG Page;

    if (!(Pte & PTE_VALID))
        return;
    Page = PteFrame(Pte);
    if (IsRamPage(Scan->Ranges, Page))
        return;
    if (Scan->Count < Scan->Max)
        Scan->Entries[Scan->Count++] = (Page << 2) | ((Pte & PTE_CACHE_BITS) >> 3);
    else
        Scan->Overflow = TRUE;
}

/* A large page: bit 12 is its PAT bit, so the frame starts above it */
static
VOID
AddLarge(PALIAS_SCAN Scan, ULONGLONG Entry, ULONGLONG PageCount)
{
    ULONGLONG Page = PteFrame(Entry) & ~(PageCount - 1);

    if (Scan->LargeCount < RTL_NUMBER_OF(Scan->Large))
    {
        Scan->Large[Scan->LargeCount].BasePage = Page;
        Scan->Large[Scan->LargeCount].PageCount = PageCount;
        Scan->Large[Scan->LargeCount].Type = (ULONG)((Entry & PTE_CACHE_BITS) >> 3);
        Scan->LargeCount++;
    }
    else
    {
        Scan->Overflow = TRUE;
    }
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

/* Every valid system-space PTE, large pages included */
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
            if (!IsRamPage(Scan->Ranges, PteFrame(Pde) & ~((PageTableSpan >> PAGE_SHIFT) - 1)))
                AddLarge(Scan, Pde, PageTableSpan >> PAGE_SHIFT);
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
            if (!(Entry & PTE_VALID))
                continue;
            if (Entry & PTE_LARGE_PAGE)
            {
                if (!IsRamPage(Scan->Ranges, PteFrame(Entry) & ~0x3FFFFULL))
                    AddLarge(Scan, Entry, 0x40000);
                continue;
            }
            for (Pdi = 0; Pdi < 512; Pdi++)
            {
                Va = (Pxi << 39) | (Ppi << 30) | (Pdi << 21);
                Entry = ReadEntry((PVOID)(PDE_BASE + ((Va >> 21) & 0x7FFFFFF) * 8), 8);
                if (!(Entry & PTE_VALID))
                    continue;
                if (Entry & PTE_LARGE_PAGE)
                {
                    if (!IsRamPage(Scan->Ranges, PteFrame(Entry) & ~0x1FFULL))
                        AddLarge(Scan, Entry, 0x200);
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

/* Types (bit per PCD/PWT value) every mapping of the page has */
static
ULONG
PageTypes(PALIAS_SCAN Scan, ULONGLONG Page)
{
    ULONG Types = 0, i, Low = 0, High = Scan->Count;

    while (Low < High)
    {
        i = (Low + High) / 2;
        if ((Scan->Entries[i] >> 2) < Page)
            Low = i + 1;
        else
            High = i;
    }
    for (i = Low; (i < Scan->Count) && ((Scan->Entries[i] >> 2) == Page); i++)
        Types |= 1 << (ULONG)(Scan->Entries[i] & 3);
    for (i = 0; i < Scan->LargeCount; i++)
    {
        if ((Page >= Scan->Large[i].BasePage) && (Page < Scan->Large[i].BasePage + Scan->Large[i].PageCount))
            Types |= 1 << Scan->Large[i].Type;
    }
    return Types;
}

/* No device page (the framebuffer above all) is mapped write-combined and with another type at once */
static
VOID
TestNoWriteCombinedAliases(VOID)
{
    ALIAS_SCAN Scan;
    ULONG i, j, Gap, WcPages = 0, Aliases = 0, Types;
    ULONGLONG Temp, Page;

    RtlZeroMemory(&Scan, sizeof(Scan));
    Scan.Max = 512 * 1024;
    Scan.Ranges = MmGetPhysicalMemoryRanges();
    Scan.Entries = ExAllocatePoolWithTag(NonPagedPool, Scan.Max * sizeof(ULONGLONG), 'TapK');
    if (skip(Scan.Ranges && Scan.Entries, "No memory\n"))
        goto Cleanup;

    CollectSystemMappings(&Scan);
    ok(!Scan.Overflow, "Too many device mappings: %lu, %lu large\n", Scan.Count, Scan.LargeCount);

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
        Page = Scan.Entries[i] >> 2;
        for (j = i; (j < Scan.Count) && ((Scan.Entries[j] >> 2) == Page); j++);
        Types = PageTypes(&Scan, Page);
        if (Types & (1 << 1))
        {
            WcPages++;
            if ((Types != (1 << 1)) && (Aliases++ < 10))
                trace("Page 0x%I64x is write-combined and also mapped with types 0x%lx\n", Page, Types);
        }
    }

    /* Large pages against each other */
    for (i = 0; i < Scan.LargeCount; i++)
    {
        for (j = i + 1; j < Scan.LargeCount; j++)
        {
            if ((Scan.Large[i].BasePage < Scan.Large[j].BasePage + Scan.Large[j].PageCount) &&
                (Scan.Large[j].BasePage < Scan.Large[i].BasePage + Scan.Large[i].PageCount) &&
                (Scan.Large[i].Type != Scan.Large[j].Type) &&
                ((Scan.Large[i].Type == 1) || (Scan.Large[j].Type == 1)))
            {
                Aliases++;
            }
        }
    }

    trace("%lu device PTEs, %lu large device pages, %lu write-combined device pages\n",
          Scan.Count, Scan.LargeCount, WcPages);
    ok_eq_ulong(Aliases, 0UL);

Cleanup:
    if (Scan.Entries)
        ExFreePoolWithTag(Scan.Entries, 'TapK');
    if (Scan.Ranges)
        ExFreePool(Scan.Ranges);
}

START_TEST(MmMapIoSpace)
{
    BOOLEAN PatWc;

    g_IsReactOS = *(PULONG)(KI_USER_SHARED_DATA + PAGE_SIZE - sizeof(ULONG)) == 0x8eac705;
    PatWc = TestPatLayout();

    /* Windows randomizes the PTE base */
    if (!skip(g_IsReactOS, "PTE checks run on ReactOS only\n"))
    {
        TestIoSpaceCacheBits(PatWc);
        TestNoWriteCombinedAliases();
    }
}

#else

START_TEST(MmMapIoSpace)
{
    skip(FALSE, "x86 and x64 only\n");
}

#endif
