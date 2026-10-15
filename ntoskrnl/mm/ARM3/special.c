/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/mm/ARM3/special.c
 * PURPOSE:         ARM Memory Manager Special Pool implementation
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
    References:
    https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/special-pool
*/

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

extern ULONG ExpPoolFlags;
extern PMMPTE MmSystemPteBase;

PMMPTE
NTAPI
MiReserveAlignedSystemPtes(IN ULONG NumberOfPtes,
                           IN MMSYSTEM_PTE_POOL_TYPE SystemPtePoolType,
                           IN ULONG Alignment);

/* GLOBALS ********************************************************************/

#define SPECIAL_POOL_PAGED_PTE    0x2000
#define SPECIAL_POOL_NONPAGED_PTE 0x4000
#define SPECIAL_POOL_PAGED        0x8000

PVOID MmSpecialPoolStart;
PVOID MmSpecialPoolEnd;
PVOID MiSpecialPoolExtra;
ULONG MiSpecialPoolExtraCount;

PMMPTE MiSpecialPoolFirstPte;
PMMPTE MiSpecialPoolLastPte;

PFN_COUNT MmSpecialPagesInUse;
PFN_COUNT MmSpecialPagesInUsePeak;
PFN_COUNT MiSpecialPagesPagable;
PFN_COUNT MiSpecialPagesPagablePeak;
PFN_COUNT MiSpecialPagesNonPaged;
PFN_COUNT MiSpecialPagesNonPagedPeak;
PFN_COUNT MiSpecialPagesNonPagedMaximum;

/* Set once special pool is ready; every hook outside the allocator tests it first */
BOOLEAN MmSpecialPoolEnabled;

/* The control vector fills these from Session Manager\Memory Management; types and lengths are checked before use */
ULONG MmSpecialPoolTagType = REG_NONE;
ULONG MmSpecialPoolTagLength = sizeof(ULONG);

/* Registry PoolTagOverruns: nonzero puts the data at the end of the page (guard page after it), zero at the start */
ULONG MmSpecialPoolCatchOverruns = TRUE;
ULONG MiSpecialPoolOverrunsType = REG_NONE;
ULONG MiSpecialPoolOverrunsLength = sizeof(ULONG);

/* Registry SpecialPoolTags (REG_SZ or REG_MULTI_SZ): more tag patterns, see MiSpecialPoolParseTags.
   One spare character: a value that fills the whole buffer may have been cut, and is rejected. */
WCHAR MiSpecialPoolTagList[MI_SPECIAL_POOL_TAG_LIST_CHARS + 1];
ULONG MiSpecialPoolTagListLength = sizeof(MiSpecialPoolTagList);
ULONG MiSpecialPoolTagListType = REG_NONE;

#define MI_SPECIAL_POOL_MAX_PATTERNS 16
static ULONG MiSpecialPoolPatterns[MI_SPECIAL_POOL_MAX_PATTERNS];
static ULONG MiSpecialPoolPatternCount;
static BOOLEAN MiSpecialPoolPatternsParsed;

typedef struct _MI_FREED_SPECIAL_POOL
{
    POOL_HEADER OverlaidPoolHeader;
    /* TODO: Add overlaid verifier pool header */
    ULONG Signature;
    ULONG TickCount;
    ULONG NumberOfBytesRequested;
    BOOLEAN Pagable;
    PVOID VirtualAddress;
    PVOID StackPointer;
    ULONG StackBytes;
    PETHREAD Thread;
    UCHAR StackData[0x400];
} MI_FREED_SPECIAL_POOL, *PMI_FREED_SPECIAL_POOL;

/* PRIVATE FUNCTIONS **********************************************************/

VOID NTAPI MiTestSpecialPool(VOID);

/* A pattern holds the tag bytes in memory order; '?' matches any byte, '*' matches the rest of the tag */
static
BOOLEAN
MiSpecialPoolTagMatches(ULONG Pattern, ULONG Tag)
{
    ULONG i;
    UCHAR PatternChar;

    for (i = 0; i < sizeof(ULONG); i++)
    {
        PatternChar = (UCHAR)(Pattern >> (i * 8));
        if (PatternChar == '*') return TRUE;
        if ((PatternChar != '?') && (PatternChar != (UCHAR)(Tag >> (i * 8)))) return FALSE;
    }

    return TRUE;
}

static
VOID
MiSpecialPoolAddPattern(ULONG Pattern)
{
    if (MiSpecialPoolPatternCount < MI_SPECIAL_POOL_MAX_PATTERNS)
    {
        MiSpecialPoolPatterns[MiSpecialPoolPatternCount++] = Pattern;
    }
}

/*
 * PoolTag (REG_DWORD) is the documented single pattern; 0x2A ('*') selects every tag.
 * SpecialPoolTags is text: patterns of at most four characters separated by spaces,
 * commas, semicolons or NULs, e.g. "NTN* LTAB Mm??". A pattern shorter than four
 * characters without '*' is padded with spaces, like the tags themselves.
 */
static
BOOLEAN
MiSpecialPoolIsSeparator(WCHAR c)
{
    return (c == UNICODE_NULL) || (c == L' ') || (c == L',') || (c == L';');
}

/* The control vector stores -1 as the length when a value is absent */
static
BOOLEAN
MiSpecialPoolDwordValid(PCWSTR Name, ULONG Type, ULONG Length)
{
    if (Length == (ULONG)-1) return FALSE;
    if ((Type == REG_DWORD) && (Length == sizeof(ULONG))) return TRUE;
    DbgPrint("Special pool: ignoring %ls (type %lu, %lu bytes; a REG_DWORD is expected)\n", Name, Type, Length);
    return FALSE;
}

static
VOID
MiSpecialPoolParseTags(VOID)
{
    ULONG Chars, i, Length, Pattern;
    WCHAR c;
    BOOLEAN Valid;

    if (MiSpecialPoolPatternsParsed) return;
    MiSpecialPoolPatternsParsed = TRUE;

    if (!MiSpecialPoolDwordValid(L"PoolTagOverruns", MiSpecialPoolOverrunsType, MiSpecialPoolOverrunsLength))
    {
        MmSpecialPoolCatchOverruns = TRUE;
    }

    if (!MiSpecialPoolDwordValid(L"PoolTag", MmSpecialPoolTagType, MmSpecialPoolTagLength))
    {
        MmSpecialPoolTag = 0;
    }
    else if ((MmSpecialPoolTag != 0) && (MmSpecialPoolTag != -1))
    {
        MiSpecialPoolAddPattern(MmSpecialPoolTag);
    }

    if (MiSpecialPoolTagListLength == (ULONG)-1) return;
    if (((MiSpecialPoolTagListType != REG_SZ) && (MiSpecialPoolTagListType != REG_MULTI_SZ)) ||
        (MiSpecialPoolTagListLength % sizeof(WCHAR)) ||
        (MiSpecialPoolTagListLength > MI_SPECIAL_POOL_TAG_LIST_CHARS * sizeof(WCHAR)))
    {
        DbgPrint("Special pool: ignoring SpecialPoolTags (type %lu, %lu bytes; REG_SZ or REG_MULTI_SZ of at most %lu characters is expected)\n",
                 MiSpecialPoolTagListType, MiSpecialPoolTagListLength, (ULONG)MI_SPECIAL_POOL_TAG_LIST_CHARS);
        return;
    }
    Chars = MiSpecialPoolTagListLength / sizeof(WCHAR);

    i = 0;
    while (i < Chars)
    {
        if (MiSpecialPoolIsSeparator(MiSpecialPoolTagList[i]))
        {
            i++;
            continue;
        }

        Pattern = 0;
        Length = 0;
        Valid = TRUE;
        while ((i < Chars) && !MiSpecialPoolIsSeparator(MiSpecialPoolTagList[i]))
        {
            c = MiSpecialPoolTagList[i];
            if ((Length >= sizeof(ULONG)) || (c < 0x21) || (c > 0x7E)) Valid = FALSE;
            if (Valid) Pattern |= (ULONG)c << (Length * 8);
            Length++;
            i++;
        }

        if (!Valid)
        {
            DbgPrint("Special pool: ignoring a tag pattern of %lu characters (at most four printable ASCII characters)\n", Length);
            continue;
        }

        if ((Pattern >> ((Length - 1) * 8)) != '*')
        {
            while (Length < sizeof(ULONG)) Pattern |= (ULONG)' ' << (Length++ * 8);
        }

        MiSpecialPoolAddPattern(Pattern);
    }
}

BOOLEAN
NTAPI
MiSpecialPoolConfigured(VOID)
{
    MiSpecialPoolParseTags();
    return (MiSpecialPoolPatternCount != 0);
}

BOOLEAN
NTAPI
MmUseSpecialPool(SIZE_T NumberOfBytes, ULONG Tag)
{
    ULONG i;

    /* Special pool is not suitable for allocations bigger than 1 page */
    if (NumberOfBytes > (PAGE_SIZE - sizeof(POOL_HEADER)))
    {
        return FALSE;
    }

    Tag &= ~PROTECTED_POOL;
    for (i = 0; i < MiSpecialPoolPatternCount; i++)
    {
        if (MiSpecialPoolTagMatches(MiSpecialPoolPatterns[i], Tag)) return TRUE;
    }

    return FALSE;
}

/* Lookaside lists of selected allocations must not cache freed entries, or a use after free would go unseen.
   Callers test MmSpecialPoolEnabled first. */
BOOLEAN
NTAPI
MmSpecialPoolSelectsLookaside(SIZE_T NumberOfBytes, ULONG Tag)
{
    return MmSpecialPoolEnabled && MmUseSpecialPool(NumberOfBytes, Tag);
}

BOOLEAN
NTAPI
MmIsSpecialPoolAddress(PVOID P)
{
    /* MmSpecialPoolEnd is the start of the last guard page; the whole page belongs to the range */
    return MmSpecialPoolEnabled &&
           ((ULONG_PTR)P >= (ULONG_PTR)MmSpecialPoolStart) &&
           ((ULONG_PTR)PAGE_ALIGN(P) <= (ULONG_PTR)MmSpecialPoolEnd);
}

BOOLEAN
NTAPI
MmIsSpecialPoolAddressFree(PVOID P)
{
    PMMPTE PointerPte;

    ASSERT(MmIsSpecialPoolAddress(P));
    PointerPte = MiAddressToPte(P);

    if (PointerPte->u.Soft.PageFileHigh == SPECIAL_POOL_PAGED_PTE ||
        PointerPte->u.Soft.PageFileHigh == SPECIAL_POOL_NONPAGED_PTE)
    {
        /* Guard page PTE */
        return FALSE;
    }

    /* Free PTE */
    return TRUE;
}

/* Pages pair up from MmSpecialPoolStart: a data page, then its guard page */
static
BOOLEAN
MiIsSpecialPoolGuardPage(PVOID P)
{
    return ((((ULONG_PTR)P - (ULONG_PTR)MmSpecialPoolStart) >> PAGE_SHIFT) & 1) != 0;
}

/*
 * Called first thing on a kernel-address fault, before any lock, at any IRQL and with interrupts
 * on or off. Special pool pages are never paged out, so an invalid one is a guard page or a freed
 * allocation. Takes no lock; reads only the page directory entry and the PTE.
 */
VOID
NTAPI
MmCheckSpecialPoolFault(
    _In_ ULONG FaultCode,
    _In_ PVOID Address,
    _In_opt_ PVOID TrapInformation)
{
    PMMPDE PointerPde;

    if (!MmIsSpecialPoolAddress(Address)) return;

    /* A page directory entry not yet copied into this process is fixed by the ordinary path, which retries */
    PointerPde = MiAddressToPde(Address);
#if (_MI_PAGING_LEVELS == 2)
    if (!MiSynchronizeSystemPde(PointerPde)) return;
#else
    if (!PointerPde->u.Hard.Valid) return;
#endif
    if (MiAddressToPte(Address)->u.Hard.Valid) return;

    KeBugCheckEx(MiIsSpecialPoolGuardPage(Address) ?
                     PAGE_FAULT_BEYOND_END_OF_ALLOCATION :
                     PAGE_FAULT_IN_FREED_SPECIAL_POOL,
                 (ULONG_PTR)Address,
                 MI_IS_WRITE_ACCESS(FaultCode),
                 TrapInformation ? KeGetTrapFramePc((PKTRAP_FRAME)TrapInformation) : 0,
                 (ULONG_PTR)TrapInformation);
}

VOID
NTAPI
MiInitializeSpecialPool(VOID)
{
    ULONG SpecialPoolPtes, i;
    PMMPTE PointerPte;

    /* Check if there is a special pool tag */
    if (!MiSpecialPoolConfigured()) return;

    /* Calculate number of system PTEs for the special pool */
    if (MmNumberOfSystemPtes >= 0x3000)
        SpecialPoolPtes = MmNumberOfSystemPtes / 3;
    else
        SpecialPoolPtes = MmNumberOfSystemPtes / 6;

    /* Don't let the number go too high */
    if (SpecialPoolPtes > 0x6000) SpecialPoolPtes = 0x6000;

    /* Round up to the page size */
    SpecialPoolPtes = PAGE_ROUND_UP(SpecialPoolPtes);

    ASSERT((SpecialPoolPtes & (PTE_PER_PAGE - 1)) == 0);

    /* Reserve those PTEs */
    do
    {
        PointerPte = MiReserveAlignedSystemPtes(SpecialPoolPtes,
                                                SystemPteSpace,
                                                /*0x400000*/0); // FIXME:
        if (PointerPte) break;

        /* Reserving didn't work, so try to reduce the requested size */
        ASSERT(SpecialPoolPtes >= PTE_PER_PAGE);
        SpecialPoolPtes -= PTE_PER_PAGE;
    } while (SpecialPoolPtes);

    /* Fail if we couldn't reserve them at all */
    if (!SpecialPoolPtes) return;

    /* Make sure we got enough */
    ASSERT(SpecialPoolPtes >= PTE_PER_PAGE);

    /* Save first PTE and its address */
    MmSpecialPoolStart = MiPteToAddress(PointerPte);

    /* The first pair stays unmapped, so that an underrun of the first page faults too */
    PointerPte += 2;
    MiSpecialPoolFirstPte = PointerPte;

    for (i = 1; i < PTE_PER_PAGE / 2; i++)
    {
        /* Point it to the next entry */
        PointerPte->u.List.NextEntry = &PointerPte[2] - MmSystemPteBase;

        /* Move to the next pair */
        PointerPte += 2;
    }

    /* Save extra values */
    MiSpecialPoolExtra = PointerPte;
    MiSpecialPoolExtraCount = SpecialPoolPtes - PTE_PER_PAGE;

    /* Mark the previous PTE as the last one */
    MiSpecialPoolLastPte = PointerPte - 2;
    MiSpecialPoolLastPte->u.List.NextEntry = MM_EMPTY_PTE_LIST;

    /* Save end address of the special pool */
    MmSpecialPoolEnd = MiPteToAddress(MiSpecialPoolLastPte + 1);

    /* Calculate maximum non-paged part of the special pool */
    MiSpecialPagesNonPagedMaximum = MmResidentAvailablePages >> 4;

    /* And limit it if it turned out to be too big */
    if (MmNumberOfPhysicalPages > 0x3FFF)
        MiSpecialPagesNonPagedMaximum = MmResidentAvailablePages >> 3;

    DbgPrint("Special pool: %lu tag pattern(s), %s, %lu PTEs at %p\n",
             MiSpecialPoolPatternCount,
             MmSpecialPoolCatchOverruns ? "overruns" : "underruns",
             SpecialPoolPtes,
             MmSpecialPoolStart);
    ExpPoolFlags |= POOL_FLAG_SPECIAL_POOL;
    MmSpecialPoolEnabled = TRUE;

    //MiTestSpecialPool();
}

NTSTATUS
NTAPI
MmExpandSpecialPool(VOID)
{
    ULONG i;
    PMMPTE PointerPte;

    MI_ASSERT_PFN_LOCK_HELD();

    if (MiSpecialPoolExtraCount == 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    PointerPte = MiSpecialPoolExtra;
    ASSERT(MiSpecialPoolFirstPte == MiSpecialPoolLastPte);
    ASSERT(MiSpecialPoolFirstPte->u.List.NextEntry == MM_EMPTY_PTE_LIST);
    MiSpecialPoolFirstPte->u.List.NextEntry = PointerPte - MmSystemPteBase;

    ASSERT(MiSpecialPoolExtraCount >= PTE_PER_PAGE);
    for (i = 0; i < PTE_PER_PAGE / 2; i++)
    {
        /* Point it to the next entry */
        PointerPte->u.List.NextEntry = &PointerPte[2] - MmSystemPteBase;

        /* Move to the next pair */
        PointerPte += 2;
    }

    /* Save remaining extra values */
    MiSpecialPoolExtra = PointerPte;
    MiSpecialPoolExtraCount -= PTE_PER_PAGE;

    /* Mark the previous PTE as the last one */
    MiSpecialPoolLastPte = PointerPte - 2;
    MiSpecialPoolLastPte->u.List.NextEntry = MM_EMPTY_PTE_LIST;

    /* Save new end address of the special pool */
    MmSpecialPoolEnd = MiPteToAddress(MiSpecialPoolLastPte + 1);

    return STATUS_SUCCESS;
}

PVOID
NTAPI
MmAllocateSpecialPool(SIZE_T NumberOfBytes, ULONG Tag, POOL_TYPE PoolType, ULONG SpecialType)
{
    KIRQL Irql;
    MMPTE TempPte = ValidKernelPte;
    PMMPTE PointerPte;
    PFN_NUMBER PageFrameNumber;
    LARGE_INTEGER TickCount;
    PVOID Entry;
    PPOOL_HEADER Header;
    PFN_COUNT PagesInUse;

    DPRINT("MmAllocateSpecialPool(%x %x %x %x)\n", NumberOfBytes, Tag, PoolType, SpecialType);

    /* Check if the pool is initialized and quit if it's not */
    if (!MiSpecialPoolFirstPte) return NULL;

    /* The data lands at a 16-byte boundary next to the guard page, never on a cache line */
    if (PoolType & CACHE_ALIGNED_POOL_MASK) return NULL;

    /* Get the pool type */
    PoolType &= BASE_POOL_TYPE_MASK;

    /* Check whether current IRQL matches the pool type */
    Irql = KeGetCurrentIrql();

    if (((PoolType == PagedPool) && (Irql > APC_LEVEL)) ||
        ((PoolType != PagedPool) && (Irql > DISPATCH_LEVEL)))
    {
        /* Bad caller */
        KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                     Irql,
                     PoolType,
                     NumberOfBytes,
                     0x30);
    }

    /* Some allocations from Mm must never use special pool */
    if (Tag == 'tSmM')
    {
        /* Reject and let normal pool handle it */
        return NULL;
    }

    /* TODO: Take into account various limitations */

    /* Heed the maximum limit of nonpaged pages */
    if ((PoolType == NonPagedPool) &&
        (MiSpecialPagesNonPaged > MiSpecialPagesNonPagedMaximum))
    {
        return NULL;
    }

    /* Lock PFN database */
    Irql = MiAcquirePfnLock();

    /* Reject allocation in case amount of available pages is too small */
    if (MmAvailablePages < 0x100)
    {
        /* Release the PFN database lock */
        MiReleasePfnLock(Irql);
        DPRINT1("Special pool: MmAvailablePages 0x%x is too small\n", MmAvailablePages);
        return NULL;
    }

    /* Check if special pool PTE list is exhausted */
    if (MiSpecialPoolFirstPte->u.List.NextEntry == MM_EMPTY_PTE_LIST)
    {
        /* Try to expand it */
        if (!NT_SUCCESS(MmExpandSpecialPool()))
        {
            /* No reserves left, reject this allocation */
            static int once;
            MiReleasePfnLock(Irql);
            if (!once++) DPRINT1("Special pool: No PTEs left!\n");
            return NULL;
        }
        ASSERT(MiSpecialPoolFirstPte->u.List.NextEntry != MM_EMPTY_PTE_LIST);
    }

    /* Save allocation time */
    KeQueryTickCount(&TickCount);

    /* Get a pointer to the first PTE */
    PointerPte = MiSpecialPoolFirstPte;

    /* Set the first PTE pointer to the next one in the list */
    MiSpecialPoolFirstPte = MmSystemPteBase + PointerPte->u.List.NextEntry;

    /* Allocate a physical page */
    if (PoolType == PagedPool)
    {
        MI_SET_USAGE(MI_USAGE_PAGED_POOL);
    }
    else
    {
        MI_SET_USAGE(MI_USAGE_NONPAGED_POOL);
    }
    MI_SET_PROCESS2("Kernel-Special");
    PageFrameNumber = MiRemoveAnyPage(MI_GET_NEXT_COLOR());

    /* Initialize PFN and make it valid */
    TempPte.u.Hard.PageFrameNumber = PageFrameNumber;
    MiInitializePfnAndMakePteValid(PageFrameNumber, PointerPte, TempPte);

    /* Release the PFN database lock */
    MiReleasePfnLock(Irql);

    /* Increase page counter */
    PagesInUse = InterlockedIncrementUL(&MmSpecialPagesInUse);
    if (PagesInUse > MmSpecialPagesInUsePeak)
        MmSpecialPagesInUsePeak = PagesInUse;

    /* Put some content into the page. Low value of tick count would do */
    Entry = MiPteToAddress(PointerPte);
    RtlFillMemory(Entry, PAGE_SIZE, TickCount.LowPart);

    /* Calculate header and entry addresses */
    if ((SpecialType != 0) &&
        ((SpecialType == 1) || (!MmSpecialPoolCatchOverruns)))
    {
        /* We catch underruns. Data is at the beginning of the page */
        Header = (PPOOL_HEADER)((PUCHAR)Entry + PAGE_SIZE - sizeof(POOL_HEADER));
    }
    else
    {
        /* We catch overruns. Data is at the end of the page */
        Header = (PPOOL_HEADER)Entry;
        Entry = (PVOID)((ULONG_PTR)((PUCHAR)Entry - NumberOfBytes + PAGE_SIZE) & ~((LONG_PTR)sizeof(POOL_HEADER) - 1));
    }

    /* Initialize the header */
    RtlZeroMemory(Header, sizeof(POOL_HEADER));

    /* Save allocation size there */
    Header->Ulong1 = (ULONG)NumberOfBytes;

    /* Make sure it's all good */
    ASSERT((NumberOfBytes <= PAGE_SIZE - sizeof(POOL_HEADER)) &&
           (PAGE_SIZE <= 32 * 1024));

    /* Mark it as paged or nonpaged */
    if (PoolType == PagedPool)
    {
        /* Add pagedpool flag into the pool header too */
        Header->Ulong1 |= SPECIAL_POOL_PAGED;

        /* Also mark the next PTE as special-pool-paged */
        PointerPte[1].u.Soft.PageFileHigh |= SPECIAL_POOL_PAGED_PTE;

        /* Increase pagable counter */
        PagesInUse = InterlockedIncrementUL(&MiSpecialPagesPagable);
        if (PagesInUse > MiSpecialPagesPagablePeak)
            MiSpecialPagesPagablePeak = PagesInUse;
    }
    else
    {
        /* Mark the next PTE as special-pool-nonpaged */
        PointerPte[1].u.Soft.PageFileHigh |= SPECIAL_POOL_NONPAGED_PTE;

        /* Increase nonpaged counter */
        PagesInUse = InterlockedIncrementUL(&MiSpecialPagesNonPaged);
        if (PagesInUse > MiSpecialPagesNonPagedPeak)
            MiSpecialPagesNonPagedPeak = PagesInUse;
    }

    /* Finally save tag and put allocation time into the header's blocksize.
       That time will be used to check memory consistency within the allocated
       page. */
    Header->PoolTag = Tag;
    Header->BlockSize = (UCHAR)TickCount.LowPart;
    DPRINT("%p\n", Entry);
    return Entry;
}

VOID
NTAPI
MiSpecialPoolCheckPattern(PUCHAR P, PPOOL_HEADER Header)
{
    ULONG BytesToCheck, BytesRequested, Index;
    PUCHAR Ptr;

    /* Get amount of bytes user requested to be allocated by clearing out the paged mask */
    BytesRequested = (Header->Ulong1 & ~SPECIAL_POOL_PAGED) & 0xFFFF;
    ASSERT(BytesRequested <= PAGE_SIZE - sizeof(POOL_HEADER));

    /* Get a pointer to the end of user's area */
    Ptr = P + BytesRequested;

    /* Calculate how many bytes to check */
    BytesToCheck = (ULONG)((PUCHAR)PAGE_ALIGN(P) + PAGE_SIZE - Ptr);

    /* Remove pool header size if we're catching underruns */
    if (((ULONG_PTR)P & (PAGE_SIZE - 1)) == 0)
    {
        /* User buffer is located in the beginning of the page */
        BytesToCheck -= sizeof(POOL_HEADER);
    }

    /* Check the pattern after user buffer */
    for (Index = 0; Index < BytesToCheck; Index++)
    {
        /* Bugcheck if bytes don't match */
        if (Ptr[Index] != Header->BlockSize)
        {
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                         (ULONG_PTR)P,
                         (ULONG_PTR)&Ptr[Index],
                         Header->BlockSize,
                         0x24);
        }
    }
}

VOID
NTAPI
MmFreeSpecialPool(PVOID P)
{
    PMMPTE PointerPte;
    PPOOL_HEADER Header;
    BOOLEAN Overruns = FALSE;
    KIRQL Irql = KeGetCurrentIrql();
    POOL_TYPE PoolType;
    ULONG BytesRequested, BytesReal = 0;
    ULONG PtrOffset;
    PUCHAR b;
    PMI_FREED_SPECIAL_POOL FreedHeader;
    LARGE_INTEGER TickCount;
    PMMPFN Pfn;

    DPRINT("MmFreeSpecialPool(%p)\n", P);

    /* Get the PTE */
    PointerPte = MiAddressToPte(P);

    /* Check if it's valid */
    if (PointerPte->u.Hard.Valid == 0)
    {
        /* Bugcheck if it has NOACCESS or 0 set as protection */
        if (PointerPte->u.Soft.Protection == MM_NOACCESS ||
            !PointerPte->u.Soft.Protection)
        {
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                         (ULONG_PTR)P,
                         (ULONG_PTR)PointerPte,
                         0,
                         0x20);
        }
    }

    /* Determine if it's a underruns or overruns pool pointer */
    PtrOffset = (ULONG)((ULONG_PTR)P & (PAGE_SIZE - 1));
    if (PtrOffset)
    {
        /* Pool catches overruns */
        Header = PAGE_ALIGN(P);
        Overruns = TRUE;
    }
    else
    {
        /* Pool catches underruns */
        Header = (PPOOL_HEADER)((PUCHAR)PAGE_ALIGN(P) + PAGE_SIZE - sizeof(POOL_HEADER));
    }

    /* Check if it's non paged pool */
    if ((Header->Ulong1 & SPECIAL_POOL_PAGED) == 0)
    {
        /* Non-paged allocation, ensure that IRQ is not higher that DISPATCH */
        PoolType = NonPagedPool;
        ASSERT(PointerPte[1].u.Soft.PageFileHigh == SPECIAL_POOL_NONPAGED_PTE);
        if (Irql > DISPATCH_LEVEL)
        {
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                         Irql,
                         PoolType,
                         (ULONG_PTR)P,
                         0x31);
        }
    }
    else
    {
        /* Paged allocation, ensure */
        PoolType = PagedPool;
        ASSERT(PointerPte[1].u.Soft.PageFileHigh == SPECIAL_POOL_PAGED_PTE);
        if (Irql > APC_LEVEL)
        {
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                         Irql,
                         PoolType,
                         (ULONG_PTR)P,
                         0x31);
        }
    }

    /* Get amount of bytes user requested to be allocated by clearing out the paged mask */
    BytesRequested = (Header->Ulong1 & ~SPECIAL_POOL_PAGED) & 0xFFFF;
    ASSERT(BytesRequested <= PAGE_SIZE - sizeof(POOL_HEADER));

    /* Check memory before the allocated user buffer in case of overruns detection */
    if (Overruns)
    {
        /* Calculate the real placement of the buffer */
        BytesReal = PAGE_SIZE - PtrOffset;

        /* If they mismatch, it's unrecoverable */
        if (BytesRequested > BytesReal)
        {
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                         (ULONG_PTR)P,
                         BytesRequested,
                         BytesReal,
                         0x21);
        }

        if (BytesRequested + sizeof(POOL_HEADER) < BytesReal)
        {
            KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                         (ULONG_PTR)P,
                         BytesRequested,
                         BytesReal,
                         0x22);
        }

        /* Actually check the memory pattern */
        for (b = (PUCHAR)(Header + 1); b < (PUCHAR)P; b++)
        {
            if (*b != Header->BlockSize)
            {
                /* Bytes mismatch */
                KeBugCheckEx(SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION,
                             (ULONG_PTR)P,
                             (ULONG_PTR)b,
                             Header->BlockSize,
                             0x23);
            }
        }
    }

    /* Check the memory pattern after the user buffer */
    MiSpecialPoolCheckPattern(P, Header);

    /* Fill the freed header */
    KeQueryTickCount(&TickCount);
    FreedHeader = (PMI_FREED_SPECIAL_POOL)PAGE_ALIGN(P);
    FreedHeader->Signature = 0x98764321;
    FreedHeader->TickCount = TickCount.LowPart;
    FreedHeader->NumberOfBytesRequested = BytesRequested;
    FreedHeader->Pagable = PoolType;
    FreedHeader->VirtualAddress = P;
    FreedHeader->Thread = PsGetCurrentThread();
    /* TODO: Fill StackPointer and StackBytes */
    FreedHeader->StackPointer = NULL;
    FreedHeader->StackBytes = 0;

    if (PoolType == NonPagedPool)
    {
        /* Non pagable. Get PFN element corresponding to the PTE */
        Pfn = MI_PFN_ELEMENT(PointerPte->u.Hard.PageFrameNumber);

        /* Count the page as free */
        InterlockedDecrementUL(&MiSpecialPagesNonPaged);

        /* Lock PFN database */
        Irql = MiAcquirePfnLock();

        /* Delete this PFN */
        MI_SET_PFN_DELETED(Pfn);

        /* Decrement share count of this PFN */
        MiDecrementShareCount(Pfn, PointerPte->u.Hard.PageFrameNumber);

        MI_ERASE_PTE(PointerPte);

        /* Flush the TLB on every processor, so that a use after free faults */
        KeFlushSingleTb(PAGE_ALIGN(P), TRUE);
    }
    else
    {
        /* Pagable. Delete that virtual address */
        MiDeleteSystemPageableVm(PointerPte, 1, 0, NULL);

        /* Count the page as free */
        InterlockedDecrementUL(&MiSpecialPagesPagable);

        /* Lock PFN database */
        Irql = MiAcquirePfnLock();
    }

    /* Mark next PTE as invalid */
    MI_ERASE_PTE(PointerPte + 1);

    /* Make sure that the last entry is really the last one */
    ASSERT(MiSpecialPoolLastPte->u.List.NextEntry == MM_EMPTY_PTE_LIST);

    /* Update the current last PTE next pointer */
    MiSpecialPoolLastPte->u.List.NextEntry = PointerPte - MmSystemPteBase;

    /* PointerPte becomes the new last PTE */
    PointerPte->u.List.NextEntry = MM_EMPTY_PTE_LIST;
    MiSpecialPoolLastPte = PointerPte;

    /* Release the PFN database lock */
    MiReleasePfnLock(Irql);

    /* Update page counter */
    InterlockedDecrementUL(&MmSpecialPagesInUse);
}

VOID
NTAPI
MiTestSpecialPool(VOID)
{
    ULONG i;
    PVOID p1, p2[100];
    //PUCHAR p3;
    ULONG ByteSize;
    POOL_TYPE PoolType = PagedPool;

    // First allocate/free
    for (i=0; i<100; i++)
    {
        ByteSize = (100 * (i+1)) % (PAGE_SIZE - sizeof(POOL_HEADER));
        p1 = MmAllocateSpecialPool(ByteSize, 'TEST', PoolType, 0);
        DPRINT1("p1 %p size %lu\n", p1, ByteSize);
        MmFreeSpecialPool(p1);
    }

    // Now allocate all at once, then free at once
    for (i=0; i<100; i++)
    {
        ByteSize = (100 * (i+1)) % (PAGE_SIZE - sizeof(POOL_HEADER));
        p2[i] = MmAllocateSpecialPool(ByteSize, 'TEST', PoolType, 0);
        DPRINT1("p2[%lu] %p size %lu\n", i, p1, ByteSize);
    }
    for (i=0; i<100; i++)
    {
        DPRINT1("Freeing %p\n", p2[i]);
        MmFreeSpecialPool(p2[i]);
    }

    // Overrun the buffer to test
    //ByteSize = 16;
    //p3 = MmAllocateSpecialPool(ByteSize, 'TEST', NonPagedPool, 0);
    //p3[ByteSize] = 0xF1; // This should cause an exception

    // Underrun the buffer to test
    //p3 = MmAllocateSpecialPool(ByteSize, 'TEST', NonPagedPool, 1);
    //p3--;
    //*p3 = 0xF1; // This should cause an exception

}

/* EOF */
