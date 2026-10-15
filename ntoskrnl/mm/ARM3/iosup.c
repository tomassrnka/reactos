/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/mm/ARM3/iosup.c
 * PURPOSE:         ARM Memory Manager I/O Mapping Functionality
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

//
// Each architecture has its own caching attributes for both I/O and Physical
// memory mappings.
//
// This describes the attributes for the x86 architecture. It eventually needs
// to go in the appropriate i386 directory.
//
MI_PFN_CACHE_ATTRIBUTE MiPlatformCacheAttributes[2][MmMaximumCacheType] =
{
    //
    // RAM
    //
    {MiNonCached,MiCached,MiWriteCombined,MiCached,MiNonCached,MiWriteCombined},

    //
    // Device Memory
    //
    {MiNonCached,MiCached,MiWriteCombined,MiCached,MiNonCached,MiWriteCombined},
};

#ifdef MI_IO_SPACE_TRACKING

/*
 * Device memory has no PFN entries to hold a cache attribute, so the live
 * mappings of device pages are recorded here. Write-combining must never alias
 * an uncached or cached mapping of the same page (Intel SDM, "Programming the
 * PAT"): a write-combining request falls back to UC- while any other mapping of
 * its pages is not write-combining, and any other request turns the
 * write-combining kernel mappings of its pages into UC-. Kernel mappings are
 * recorded until they are unmapped. User mappings cannot be changed afterwards,
 * so they never use write-combining, and their record stays until reboot.
 */
typedef struct _MI_IO_RANGE
{
    LIST_ENTRY Links;
    PFN_NUMBER BasePage;
    PFN_NUMBER PageCount;
    PMMPTE FirstPte;            /* NULL for a user mapping */
    MI_IO_TYPE Type;
} MI_IO_RANGE, *PMI_IO_RANGE;

static LIST_ENTRY MiIoRangeList = { &MiIoRangeList, &MiIoRangeList };
static KSPIN_LOCK MiIoRangeLock;
/* A record could not be allocated: write-combining stays off from then on */
static BOOLEAN MiIoRangeTrackingLost;

static
BOOLEAN
MiIoRangeOverlaps(
    _In_ PMI_IO_RANGE Range,
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount)
{
    return (BasePage < Range->BasePage + Range->PageCount) &&
           (Range->BasePage < BasePage + PageCount);
}

/* Caller holds MiIoRangeLock and flushes the TB of all processors afterwards */
static
VOID
MiDemoteIoRange(
    _Inout_ PMI_IO_RANGE Range)
{
    PFN_NUMBER i;
    MMPTE TempPte;

    ASSERT((Range->Type == MiIoWriteCombined) && (Range->FirstPte != NULL));
    for (i = 0; i < Range->PageCount; i++)
    {
        TempPte = Range->FirstPte[i];
        MI_SET_IO_TYPE(&TempPte, MiIoUncachedMinus);
        MI_UPDATE_VALID_PTE(&Range->FirstPte[i], TempPte);
    }
    Range->Type = MiIoUncachedMinus;
}

/*
 * Picks the memory type for PageCount device pages from BasePage on and records
 * the mapping. A kernel mapping passes its system PTEs in FirstPte; they are
 * filled from TemplatePte under the lock, so a concurrent request sees either
 * none of them or all of them with their final type. A user mapping passes NULL
 * and applies the returned type itself. Returns TRUE if another mapping was
 * changed to UC-: the caller flushes the TB of all processors before the new
 * mapping can be used.
 */
BOOLEAN
NTAPI
MiMapIoSpacePages(
    _In_opt_ PMMPTE FirstPte,
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount,
    _In_ MI_PFN_CACHE_ATTRIBUTE Attribute,
    _In_ MMPTE TemplatePte,
    _Out_ PMI_IO_TYPE Type)
{
    PMI_IO_RANGE NewRange, Range;
    PLIST_ENTRY Entry;
    BOOLEAN Demoted = FALSE;
    MI_IO_TYPE NewType;
    MMPTE TempPte;
    PFN_NUMBER i;
    KIRQL OldIrql;

    ASSERT(PageCount != 0);
    if (Attribute == MiCached)
        NewType = MiIoWriteBack;
    else if (Attribute == MiNonCached)
        NewType = MiIoUncached;
    else if (FirstPte && MI_WRITE_COMBINED_PAT_AVAILABLE())
        NewType = MiIoWriteCombined;
    else
        NewType = MiIoUncachedMinus;

    NewRange = ExAllocatePoolWithTag(NonPagedPool, sizeof(*NewRange), 'RoiM');

    KeAcquireSpinLock(&MiIoRangeLock, &OldIrql);

    if (!NewRange && !MiIoRangeTrackingLost)
    {
        /* Later requests cannot see this mapping, so no page may stay write-combined */
        MiIoRangeTrackingLost = TRUE;
        for (Entry = MiIoRangeList.Flink; Entry != &MiIoRangeList; Entry = Entry->Flink)
        {
            Range = CONTAINING_RECORD(Entry, MI_IO_RANGE, Links);
            if (Range->Type == MiIoWriteCombined)
            {
                MiDemoteIoRange(Range);
                Demoted = TRUE;
            }
        }
    }

    if (MiIoRangeTrackingLost && (NewType == MiIoWriteCombined))
        NewType = MiIoUncachedMinus;

    if (NewType == MiIoWriteCombined)
    {
        for (Entry = MiIoRangeList.Flink; Entry != &MiIoRangeList; Entry = Entry->Flink)
        {
            Range = CONTAINING_RECORD(Entry, MI_IO_RANGE, Links);
            if (MiIoRangeOverlaps(Range, BasePage, PageCount) && (Range->Type != MiIoWriteCombined))
            {
                NewType = MiIoUncachedMinus;
                break;
            }
        }
    }
    else
    {
        for (Entry = MiIoRangeList.Flink; Entry != &MiIoRangeList; Entry = Entry->Flink)
        {
            Range = CONTAINING_RECORD(Entry, MI_IO_RANGE, Links);
            if (MiIoRangeOverlaps(Range, BasePage, PageCount) && (Range->Type == MiIoWriteCombined))
            {
                MiDemoteIoRange(Range);
                Demoted = TRUE;
            }
        }
    }

    if (FirstPte)
    {
        TempPte = TemplatePte;
        MI_SET_IO_TYPE(&TempPte, NewType);
        for (i = 0; i < PageCount; i++)
        {
            TempPte.u.Hard.PageFrameNumber = BasePage + i;
            MI_WRITE_VALID_PTE(&FirstPte[i], TempPte);
        }
    }

    if (NewRange && !MiIoRangeTrackingLost)
    {
        if (!FirstPte)
        {
            /* Merge a user mapping into a touching record of the same type */
            for (Entry = MiIoRangeList.Flink; Entry != &MiIoRangeList; Entry = Entry->Flink)
            {
                Range = CONTAINING_RECORD(Entry, MI_IO_RANGE, Links);
                if (!Range->FirstPte && (Range->Type == NewType) &&
                    (BasePage <= Range->BasePage + Range->PageCount) &&
                    (Range->BasePage <= BasePage + PageCount))
                {
                    PFN_NUMBER End = max(Range->BasePage + Range->PageCount, BasePage + PageCount);

                    Range->BasePage = min(Range->BasePage, BasePage);
                    Range->PageCount = End - Range->BasePage;
                    break;
                }
            }
            if (Entry != &MiIoRangeList)
                goto Done;
        }

        NewRange->BasePage = BasePage;
        NewRange->PageCount = PageCount;
        NewRange->FirstPte = FirstPte;
        NewRange->Type = NewType;
        InsertTailList(&MiIoRangeList, &NewRange->Links);
        NewRange = NULL;
    }

Done:
    KeReleaseSpinLock(&MiIoRangeLock, OldIrql);
    if (NewRange)
        ExFreePoolWithTag(NewRange, 'RoiM');

    *Type = NewType;
    return Demoted;
}

/* Drops the records of the kernel mappings whose PTEs lie in the range; called before the PTEs are cleared */
VOID
NTAPI
MiForgetIoSpaceMappings(
    _In_ PMMPTE FirstPte,
    _In_ PFN_NUMBER PteCount)
{
    LIST_ENTRY FreeList;
    PLIST_ENTRY Entry, Next;
    PMI_IO_RANGE Range;
    KIRQL OldIrql;

    /* Records of these PTEs were inserted by their owner, which is the caller */
    if (IsListEmpty(&MiIoRangeList))
        return;

    InitializeListHead(&FreeList);
    KeAcquireSpinLock(&MiIoRangeLock, &OldIrql);
    for (Entry = MiIoRangeList.Flink; Entry != &MiIoRangeList; Entry = Next)
    {
        Next = Entry->Flink;
        Range = CONTAINING_RECORD(Entry, MI_IO_RANGE, Links);
        if ((Range->FirstPte >= FirstPte) && (Range->FirstPte < FirstPte + PteCount))
        {
            RemoveEntryList(Entry);
            InsertTailList(&FreeList, Entry);
        }
    }
    KeReleaseSpinLock(&MiIoRangeLock, OldIrql);

    while (!IsListEmpty(&FreeList))
    {
        Entry = RemoveHeadList(&FreeList);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, MI_IO_RANGE, Links), 'RoiM');
    }
}

#endif /* MI_IO_SPACE_TRACKING */

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
PVOID
NTAPI
MmMapIoSpace(IN PHYSICAL_ADDRESS PhysicalAddress,
             IN SIZE_T NumberOfBytes,
             IN MEMORY_CACHING_TYPE CacheType)
{

    PFN_NUMBER Pfn;
    PFN_COUNT PageCount;
    PMMPTE PointerPte;
    PVOID BaseAddress;
    MMPTE TempPte;
    PMMPFN Pfn1 = NULL;
    MI_PFN_CACHE_ATTRIBUTE CacheAttribute;
    BOOLEAN IsIoMapping;
    PFN_COUNT i, Run;
    BOOLEAN Flush = FALSE;

    //
    // Must be called with a non-zero count
    //
    ASSERT(NumberOfBytes != 0);

    //
    // Make sure the upper bits are 0 if this system
    // can't describe more than 4 GB of physical memory.
    // FIXME: This doesn't respect PAE, but we currently don't
    // define a PAE build flag since there is no such build.
    //
#if !defined(_M_AMD64)
    ASSERT(PhysicalAddress.HighPart == 0);
#endif

    //
    // Normalize and validate the caching attributes
    //
    CacheType &= 0xFF;
    if (CacheType >= MmMaximumCacheType) return NULL;

    //
    // Calculate page count
    //
    PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(PhysicalAddress.LowPart,
                                               NumberOfBytes);

    //
    // Compute the PFN and check if it's a known I/O mapping
    // Also translate the cache attribute
    //
    Pfn = (PFN_NUMBER)(PhysicalAddress.QuadPart >> PAGE_SHIFT);
    Pfn1 = MiGetPfnEntry(Pfn);
    IsIoMapping = (Pfn1 == NULL) ? TRUE : FALSE;
    CacheAttribute = MiPlatformCacheAttributes[IsIoMapping][CacheType];

    //
    // Now allocate system PTEs for the mapping, and get the VA
    //
    PointerPte = MiReserveSystemPtes(PageCount, SystemPteSpace);
    if (!PointerPte) return NULL;
    BaseAddress = MiPteToAddress(PointerPte);

    //
    // Check if this is uncached
    //
    if (CacheAttribute != MiCached)
    {
        //
        // Flush all caches
        //
        KeFlushEntireTb(TRUE, TRUE);
        KeInvalidateAllCaches();
    }

    //
    // Now compute the VA offset
    //
    BaseAddress = (PVOID)((ULONG_PTR)BaseAddress +
                          BYTE_OFFSET(PhysicalAddress.LowPart));

    //
    // Get the template and configure caching
    //
    TempPte = ValidKernelPte;
    switch (CacheAttribute)
    {
        case MiNonCached:

            //
            // Disable the cache
            //
            MI_PAGE_DISABLE_CACHE(&TempPte);
            MI_PAGE_WRITE_THROUGH(&TempPte);
            break;

        case MiCached:

            //
            // Leave defaults
            //
            break;

        case MiWriteCombined:

            //
            // Disable the cache and allow combined writing
            // (device pages get their type from MiMapIoSpacePages)
            //
            MI_PAGE_DISABLE_CACHE(&TempPte);
            MI_PAGE_WRITE_COMBINED(&TempPte);
            break;

        default:

            //
            // Should never happen
            //
            ASSERT(FALSE);
            break;
    }

    //
    // Sanity check and re-flush
    //
    Pfn = (PFN_NUMBER)(PhysicalAddress.QuadPart >> PAGE_SHIFT);
    ASSERT((Pfn1 == MiGetPfnEntry(Pfn)) || (Pfn1 == NULL));
    KeFlushEntireTb(TRUE, TRUE);
    KeInvalidateAllCaches();

    //
    // Do the mapping
    //
    for (i = 0; i < PageCount; i += Run)
    {
#ifdef MI_IO_SPACE_TRACKING
        if (!MiGetPfnEntry(Pfn + i))
        {
            MI_IO_TYPE Type;

            for (Run = 1; (i + Run < PageCount) && !MiGetPfnEntry(Pfn + i + Run); Run++);
            Flush |= MiMapIoSpacePages(PointerPte + i,
                                       Pfn + i,
                                       Run,
                                       MiPlatformCacheAttributes[TRUE][CacheType],
                                       ValidKernelPte,
                                       &Type);
            continue;
        }
#endif
        Run = 1;
        TempPte.u.Hard.PageFrameNumber = Pfn + i;
        MI_WRITE_VALID_PTE(PointerPte + i, TempPte);
    }

    /* Another mapping of the pages changed from write-combining to UC- */
    if (Flush) KeFlushEntireTb(TRUE, TRUE);

    //
    // We're done!
    //
    return BaseAddress;
}

/*
 * @implemented
 */
VOID
NTAPI
MmUnmapIoSpace(IN PVOID BaseAddress,
               IN SIZE_T NumberOfBytes)
{
    PFN_NUMBER Pfn;
    PFN_COUNT PageCount;
    PMMPTE PointerPte;

    //
    // Sanity check
    //
    ASSERT(NumberOfBytes != 0);

    //
    // Get the page count
    //
    PageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(BaseAddress, NumberOfBytes);

    //
    // Get the PTE and PFN
    //
    PointerPte = MiAddressToPte(BaseAddress);
    Pfn = PFN_FROM_PTE(PointerPte);

#ifdef MI_IO_SPACE_TRACKING
    MiForgetIoSpaceMappings(PointerPte, PageCount);
#endif

    //
    // Is this an I/O mapping?
    //
    if (!MiGetPfnEntry(Pfn))
    {
        //
        // Destroy the PTE
        //
        RtlZeroMemory(PointerPte, PageCount * sizeof(MMPTE));

        //
        // Blow the TLB
        //
        KeFlushEntireTb(TRUE, TRUE);
    }

    //
    // Release the PTEs
    //
    MiReleaseSystemPtes(PointerPte, PageCount, 0);
}

/*
 * @implemented
 */
PVOID
NTAPI
MmMapVideoDisplay(IN PHYSICAL_ADDRESS PhysicalAddress,
                  IN SIZE_T NumberOfBytes,
                  IN MEMORY_CACHING_TYPE CacheType)
{
    PAGED_CODE();

    //
    // Call the real function
    //
    return MmMapIoSpace(PhysicalAddress, NumberOfBytes, CacheType);
}

/*
 * @implemented
 */
VOID
NTAPI
MmUnmapVideoDisplay(IN PVOID BaseAddress,
                    IN SIZE_T NumberOfBytes)
{
    //
    // Call the real function
    //
    MmUnmapIoSpace(BaseAddress, NumberOfBytes);
}

LOGICAL
NTAPI
MmIsIoSpaceActive(IN PHYSICAL_ADDRESS StartAddress,
                  IN SIZE_T NumberOfBytes)
{
    UNIMPLEMENTED;
    return FALSE;
}

/* EOF */
