/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite: PAT layout and MmMapIoSpace cache types
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define PTE_WRITE_THROUGH   0x08
#define PTE_CACHE_DISABLE   0x10
#define PTE_CACHE_BITS      (PTE_WRITE_THROUGH | PTE_CACHE_DISABLE)

#define IA32_PAT            0x277
#define PAT_TYPE_UC         0
#define PAT_TYPE_WC         1
#define PAT_TYPE_WB         6
#define PAT_TYPE_UCM        7

static BOOLEAN g_IsReactOS;

#ifdef _M_IX86

#define PTE_BASE    0xC0000000

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

/* A page below 4 GB that no physical memory range covers, so Mm treats it as device memory. It is never touched */
static
BOOLEAN
FindDevicePage(PPHYSICAL_ADDRESS Device)
{
    PPHYSICAL_MEMORY_RANGE Ranges, Range, Other;
    BOOLEAN Found = FALSE;

    Ranges = MmGetPhysicalMemoryRanges();
    if (!Ranges)
        return FALSE;

    for (Range = Ranges; Range->BaseAddress.QuadPart || Range->NumberOfBytes.QuadPart; Range++)
    {
        /* Past the firmware tables that often sit right above a range */
        LONGLONG Candidate = (Range->BaseAddress.QuadPart + Range->NumberOfBytes.QuadPart + 0xFFFFF) &
                             ~(LONGLONG)0xFFFFF;

        if (Candidate + PAGE_SIZE > 0x100000000LL)
            continue;
        for (Other = Ranges; Other->BaseAddress.QuadPart || Other->NumberOfBytes.QuadPart; Other++)
        {
            if (Candidate + PAGE_SIZE > Other->BaseAddress.QuadPart &&
                Candidate < Other->BaseAddress.QuadPart + Other->NumberOfBytes.QuadPart)
                break;
        }
        if (Other->BaseAddress.QuadPart || Other->NumberOfBytes.QuadPart)
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
ULONG
MappedCacheBits(PHYSICAL_ADDRESS Physical, MEMORY_CACHING_TYPE CacheType)
{
    PVOID Mapping;
    ULONG Bits;

    Mapping = MmMapIoSpace(Physical, PAGE_SIZE, CacheType);
    ok(Mapping != NULL, "MmMapIoSpace(0x%I64x, %d) failed\n", Physical.QuadPart, CacheType);
    if (!Mapping)
        return MAXULONG;

    Bits = (ULONG)(GetPteValue(Mapping) & PTE_CACHE_BITS);
    MmUnmapIoSpace(Mapping, PAGE_SIZE);
    return Bits;
}

static
VOID
TestIoSpaceCacheBits(BOOLEAN HasPat)
{
    PHYSICAL_ADDRESS Device, Ram;
    PVOID Buffer;

    if (!FindDevicePage(&Device))
    {
        skip(FALSE, "No device page below 4 GB\n");
    }
    else
    {
        trace("Device page 0x%I64x\n", Device.QuadPart);
        ok_eq_hex(MappedCacheBits(Device, MmCached), 0UL);
        ok_eq_hex(MappedCacheBits(Device, MmNonCached), (ULONG)PTE_CACHE_BITS);
        /* PAT entry 1, write-combining; PCD alone is UC- and stays uncached under an UC MTRR */
        ok_eq_hex(MappedCacheBits(Device, MmWriteCombined),
                  HasPat ? (ULONG)PTE_WRITE_THROUGH : (ULONG)PTE_CACHE_DISABLE);
    }

    /* RAM is never mapped write-combined: another mapping of the page caches it */
    Buffer = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, 'TapK');
    if (!skip(Buffer != NULL, "No memory\n"))
    {
        Ram = MmGetPhysicalAddress(Buffer);
        ok_eq_hex(MappedCacheBits(Ram, MmCached), 0UL);
        ok_eq_hex(MappedCacheBits(Ram, MmWriteCombined), (ULONG)PTE_CACHE_DISABLE);
        ok_eq_hex((ULONG)(GetPteValue(Buffer) & PTE_CACHE_BITS), 0UL);
        ExFreePoolWithTag(Buffer, 'TapK');
    }
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
        TestIoSpaceCacheBits(HasPat);
}
