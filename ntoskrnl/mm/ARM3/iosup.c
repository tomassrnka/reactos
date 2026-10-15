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
 * mappings of device pages are counted here, per physical page, in disjoint
 * page ranges kept sorted by page number and split where a mapping starts or
 * ends inside one. Write-combining must never alias an uncached or cached
 * mapping of the same page (Intel SDM, "Programming the PAT"), and a live
 * mapping never changes type:
 * - a page that has write-combined mappings is mapped write-combined again,
 *   whatever the new request asks for;
 * - a write-combining request gets write-combining for its free pages only if
 *   no page of the request has a mapping of another type, else UC-;
 * - any other request gets its own type for pages that are free or have only
 *   mappings of other types.
 * A page's count drops only after its PTE is cleared and the TB flushed, so a
 * new type is never granted while an old translation can still be used.
 */
typedef struct _MI_IO_CLAIM
{
    LIST_ENTRY Links;
    PFN_NUMBER BasePage;
    PFN_NUMBER PageCount;
    ULONG WcCount;              /* write-combined mappings of each page */
    ULONG OtherCount;           /* mappings of each page with another type */
} MI_IO_CLAIM, *PMI_IO_CLAIM;

static LIST_ENTRY MiIoClaimList = { &MiIoClaimList, &MiIoClaimList };
static KSPIN_LOCK MiIoClaimLock;
/* A record could not be allocated: counts are kept as they are for good and write-combining is refused for free pages */
static BOOLEAN MiIoClaimsFrozen;
/* Write-back device mappings released since the caches were last written back on every processor */
static volatile LONG MiIoWriteBackGeneration;
static volatile LONG MiIoFlushedGeneration;

#define MI_IO_CLAIM_TAG 'RoiM'

static
ULONG_PTR
NTAPI
MiWriteBackCachesWorker(
    _In_ ULONG_PTR Context)
{
    UNREFERENCED_PARAMETER(Context);
    __wbinvd();
    return 0;
}

static
VOID
MiFreeIoClaimList(
    _Inout_ PLIST_ENTRY List)
{
    while (!IsListEmpty(List))
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(List), MI_IO_CLAIM, Links), MI_IO_CLAIM_TAG);
}

/* Adds spare records until the list holds Needed of them; FALSE if the pool is exhausted */
static
BOOLEAN
MiReserveIoClaims(
    _Inout_ PLIST_ENTRY Spares,
    _Inout_ PULONG SpareCount,
    _In_ ULONG Needed)
{
    PMI_IO_CLAIM Claim;

    while (*SpareCount < Needed)
    {
        Claim = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Claim), MI_IO_CLAIM_TAG);
        if (!Claim)
            return FALSE;
        InsertTailList(Spares, &Claim->Links);
        (*SpareCount)++;
    }
    return TRUE;
}

static
PMI_IO_CLAIM
MiTakeIoClaim(
    _Inout_ PLIST_ENTRY Spares)
{
    ASSERT(!IsListEmpty(Spares));
    return CONTAINING_RECORD(RemoveHeadList(Spares), MI_IO_CLAIM, Links);
}

/* The first claim that ends after Page, or NULL; caller holds MiIoClaimLock */
static
PMI_IO_CLAIM
MiFindIoClaim(
    _In_ PFN_NUMBER Page)
{
    PLIST_ENTRY Entry;
    PMI_IO_CLAIM Claim;

    for (Entry = MiIoClaimList.Flink; Entry != &MiIoClaimList; Entry = Entry->Flink)
    {
        Claim = CONTAINING_RECORD(Entry, MI_IO_CLAIM, Links);
        if (Claim->BasePage + Claim->PageCount > Page)
            return Claim;
    }
    return NULL;
}

static
ULONG
MiCountIoClaims(
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount)
{
    PMI_IO_CLAIM Claim;
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    for (Entry = MiIoClaimList.Flink; Entry != &MiIoClaimList; Entry = Entry->Flink)
    {
        Claim = CONTAINING_RECORD(Entry, MI_IO_CLAIM, Links);
        if ((Claim->BasePage < BasePage + PageCount) && (BasePage < Claim->BasePage + Claim->PageCount))
            Count++;
    }
    return Count;
}

/*
 * Adds the deltas to the counts of every page of the range, splitting claims at
 * its ends and filling gaps, then drops empty claims and merges equal
 * neighbours. Uses at most three spare records; freed records go to Spares.
 */
static
VOID
MiAdjustIoClaims(
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount,
    _In_ LONG WcDelta,
    _In_ LONG OtherDelta,
    _Inout_ PLIST_ENTRY Spares)
{
    PFN_NUMBER Page = BasePage, End = BasePage + PageCount, GapEnd;
    PMI_IO_CLAIM Claim, New, Next;
    PLIST_ENTRY Entry;

    while (Page < End)
    {
        Claim = MiFindIoClaim(Page);
        if (!Claim || (Claim->BasePage > Page))
        {
            GapEnd = (Claim && (Claim->BasePage < End)) ? Claim->BasePage : End;
            if ((WcDelta > 0) || (OtherDelta > 0))
            {
                New = MiTakeIoClaim(Spares);
                New->BasePage = Page;
                New->PageCount = GapEnd - Page;
                New->WcCount = (WcDelta > 0) ? WcDelta : 0;
                New->OtherCount = (OtherDelta > 0) ? OtherDelta : 0;
                /* Before Claim, or at the end of the list */
                InsertTailList(Claim ? &Claim->Links : &MiIoClaimList, &New->Links);
            }
            Page = GapEnd;
            continue;
        }

        if (Claim->BasePage < Page)
        {
            New = MiTakeIoClaim(Spares);
            *New = *Claim;
            New->BasePage = Page;
            New->PageCount = Claim->BasePage + Claim->PageCount - Page;
            Claim->PageCount = Page - Claim->BasePage;
            InsertHeadList(&Claim->Links, &New->Links);
            Claim = New;
        }
        if (Claim->BasePage + Claim->PageCount > End)
        {
            New = MiTakeIoClaim(Spares);
            *New = *Claim;
            New->BasePage = End;
            New->PageCount = Claim->BasePage + Claim->PageCount - End;
            Claim->PageCount = End - Claim->BasePage;
            InsertHeadList(&Claim->Links, &New->Links);
        }

        ASSERT((LONG)Claim->WcCount + WcDelta >= 0);
        ASSERT((LONG)Claim->OtherCount + OtherDelta >= 0);
        Claim->WcCount = max((LONG)Claim->WcCount + WcDelta, 0);
        Claim->OtherCount = max((LONG)Claim->OtherCount + OtherDelta, 0);
        Page = Claim->BasePage + Claim->PageCount;
    }

    Entry = MiIoClaimList.Flink;
    while (Entry != &MiIoClaimList)
    {
        Claim = CONTAINING_RECORD(Entry, MI_IO_CLAIM, Links);
        Entry = Entry->Flink;
        if (!Claim->WcCount && !Claim->OtherCount)
        {
            RemoveEntryList(&Claim->Links);
            InsertTailList(Spares, &Claim->Links);
            continue;
        }
        while (Entry != &MiIoClaimList)
        {
            Next = CONTAINING_RECORD(Entry, MI_IO_CLAIM, Links);
            if ((Next->BasePage != Claim->BasePage + Claim->PageCount) ||
                (Next->WcCount != Claim->WcCount) || (Next->OtherCount != Claim->OtherCount))
                break;
            Entry = Entry->Flink;
            Claim->PageCount += Next->PageCount;
            RemoveEntryList(&Next->Links);
            InsertTailList(Spares, &Next->Links);
        }
    }
}

/*
 * Picks the memory type of each of PageCount device pages from BasePage on,
 * counts the new mapping and, for a kernel mapping, fills its system PTEs from
 * TemplatePte under the same lock. FirstPte is NULL for a user mapping of one
 * page; its type is returned in *UserType.
 */
static
VOID
MiClaimIoSpace(
    _In_opt_ PMMPTE FirstPte,
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount,
    _In_ MI_PFN_CACHE_ATTRIBUTE Attribute,
    _In_ MMPTE TemplatePte,
    _Out_opt_ PMI_IO_TYPE UserType)
{
    PFN_NUMBER Page, PieceEnd, i, End = BasePage + PageCount;
    LIST_ENTRY Spares;
    ULONG SpareCount = 0, Claims;
    MI_IO_TYPE Requested, Type;
    PMI_IO_CLAIM Claim;
    BOOLEAN Record, AnyOther;
    LONG Generation;
    MMPTE TempPte;
    KIRQL OldIrql;

    ASSERT(PageCount != 0);
    ASSERT(FirstPte || (PageCount == 1));
    InitializeListHead(&Spares);

    if (Attribute == MiCached)
        Requested = MiIoWriteBack;
    else if (Attribute == MiNonCached)
        Requested = MiIoUncached;
    else if (MI_WRITE_COMBINED_PAT_AVAILABLE())
        Requested = MiIoWriteCombined;
    else
        Requested = MiIoUncachedMinus;

Retry:
    /* Lines a released write-back mapping left in some cache must not be written over device data later */
    Generation = MiIoWriteBackGeneration;
    if ((Requested != MiIoWriteBack) && (Generation != MiIoFlushedGeneration))
    {
        KeIpiGenericCall(MiWriteBackCachesWorker, 0);
        MiIoFlushedGeneration = Generation;
    }

    KeAcquireSpinLock(&MiIoClaimLock, &OldIrql);

    if ((Requested != MiIoWriteBack) && (Generation != MiIoWriteBackGeneration))
    {
        KeReleaseSpinLock(&MiIoClaimLock, OldIrql);
        goto Retry;
    }

    /* Each piece needs at most three records: two splits and a gap */
    Record = !MiIoClaimsFrozen;
    if (Record)
    {
        Claims = MiCountIoClaims(BasePage, PageCount);
        if (SpareCount < 6 * Claims + 3)
        {
            KeReleaseSpinLock(&MiIoClaimLock, OldIrql);
            if (MiReserveIoClaims(&Spares, &SpareCount, 6 * Claims + 3))
                goto Retry;
            KeAcquireSpinLock(&MiIoClaimLock, &OldIrql);
            MiIoClaimsFrozen = TRUE;
            Record = FALSE;
        }
    }

    AnyOther = FALSE;
    for (Page = BasePage; Page < End; Page = Claim->BasePage + Claim->PageCount)
    {
        Claim = MiFindIoClaim(Page);
        if (!Claim || (Claim->BasePage >= End))
            break;
        if (Claim->OtherCount && !Claim->WcCount)
            AnyOther = TRUE;
    }

    for (Page = BasePage; Page < End; Page = PieceEnd)
    {
        Claim = MiFindIoClaim(Page);
        if (Claim && (Claim->BasePage <= Page))
        {
            PieceEnd = min(Claim->BasePage + Claim->PageCount, End);
            if (Claim->WcCount)
                Type = MiIoWriteCombined;
            else if (Requested == MiIoWriteCombined)
                Type = MiIoUncachedMinus;
            else
                Type = Requested;
        }
        else
        {
            PieceEnd = (Claim && (Claim->BasePage < End)) ? Claim->BasePage : End;
            if ((Requested == MiIoWriteCombined) && (AnyOther || MiIoClaimsFrozen))
                Type = MiIoUncachedMinus;
            else
                Type = Requested;
        }

        if (FirstPte)
        {
            TempPte = TemplatePte;
            MI_SET_IO_TYPE(&TempPte, Type);
            for (i = Page; i < PieceEnd; i++)
            {
                TempPte.u.Hard.PageFrameNumber = i;
                MI_WRITE_VALID_PTE(&FirstPte[i - BasePage], TempPte);
            }
        }
        else
        {
            *UserType = Type;
        }

        if (Record)
        {
            MiAdjustIoClaims(Page,
                             PieceEnd - Page,
                             (Type == MiIoWriteCombined) ? 1 : 0,
                             (Type == MiIoWriteCombined) ? 0 : 1,
                             &Spares);
        }
    }

    KeReleaseSpinLock(&MiIoClaimLock, OldIrql);
    MiFreeIoClaimList(&Spares);
}

VOID
NTAPI
MiMapIoSpacePages(
    _In_ PMMPTE FirstPte,
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount,
    _In_ MI_PFN_CACHE_ATTRIBUTE Attribute,
    _In_ MMPTE TemplatePte)
{
    MiClaimIoSpace(FirstPte, BasePage, PageCount, Attribute, TemplatePte, NULL);
}

MI_IO_TYPE
NTAPI
MiClaimIoSpacePage(
    _In_ PFN_NUMBER Page,
    _In_ MI_PFN_CACHE_ATTRIBUTE Attribute)
{
    MI_IO_TYPE Type;
    MMPTE Unused = {{0}};

    MiClaimIoSpace(NULL, Page, 1, Attribute, Unused, &Type);
    return Type;
}

/* Called once the PTEs of these pages are cleared and no TB holds them any more */
VOID
NTAPI
MiReleaseIoSpacePages(
    _In_ PFN_NUMBER BasePage,
    _In_ PFN_NUMBER PageCount,
    _In_ MI_IO_TYPE Type)
{
    LIST_ENTRY Spares;
    ULONG SpareCount = 0;
    KIRQL OldIrql;
    BOOLEAN Reserved;

    if (Type == MiIoWriteBack)
        InterlockedIncrement(&MiIoWriteBackGeneration);
    if (MiIoClaimsFrozen)
        return;

    InitializeListHead(&Spares);
    Reserved = MiReserveIoClaims(&Spares, &SpareCount, 3);

    KeAcquireSpinLock(&MiIoClaimLock, &OldIrql);
    if (!Reserved)
        MiIoClaimsFrozen = TRUE;
    if (!MiIoClaimsFrozen)
    {
        MiAdjustIoClaims(BasePage,
                         PageCount,
                         (Type == MiIoWriteCombined) ? -1 : 0,
                         (Type == MiIoWriteCombined) ? 0 : -1,
                         &Spares);
    }
    KeReleaseSpinLock(&MiIoClaimLock, OldIrql);
    MiFreeIoClaimList(&Spares);
}

/* FALSE if the batch is full: flush, release it, then add the page again */
BOOLEAN
NTAPI
MiRetireIoSpacePage(
    _Inout_ PMI_IO_RETIRE Retire,
    _In_ PFN_NUMBER Page,
    _In_ MI_IO_TYPE Type)
{
    if (Retire->Count &&
        (Retire->Run[Retire->Count - 1].Type == Type) &&
        (Retire->Run[Retire->Count - 1].BasePage + Retire->Run[Retire->Count - 1].PageCount == Page))
    {
        Retire->Run[Retire->Count - 1].PageCount++;
        return TRUE;
    }
    if (Retire->Count == RTL_NUMBER_OF(Retire->Run))
        return FALSE;
    Retire->Run[Retire->Count].BasePage = Page;
    Retire->Run[Retire->Count].PageCount = 1;
    Retire->Run[Retire->Count].Type = Type;
    Retire->Count++;
    return TRUE;
}

/* After the TB flush that retired the batch's PTEs */
VOID
NTAPI
MiReleaseRetiredIoSpace(
    _Inout_ PMI_IO_RETIRE Retire)
{
    ULONG i;

    for (i = 0; i < Retire->Count; i++)
        MiReleaseIoSpacePages(Retire->Run[i].BasePage, Retire->Run[i].PageCount, Retire->Run[i].Type);
    Retire->Count = 0;
}

/* Clears the device-page PTEs of a system PTE range, flushes every TB, then drops their counts */
VOID
NTAPI
MiRetireIoSpacePtes(
    _In_ PMMPTE FirstPte,
    _In_ PFN_NUMBER PteCount)
{
    MI_IO_RETIRE Retire;
    PFN_NUMBER i, Page;
    MI_IO_TYPE Type;

    Retire.Count = 0;
    for (i = 0; i < PteCount; i++)
    {
        if (!FirstPte[i].u.Hard.Valid)
            continue;
        Page = FirstPte[i].u.Hard.PageFrameNumber;
        if (MiGetPfnEntry(Page))
            continue;

        Type = MI_GET_IO_TYPE(&FirstPte[i]);
        if (!MiRetireIoSpacePage(&Retire, Page, Type))
        {
            KeFlushEntireTb(TRUE, TRUE);
            MiReleaseRetiredIoSpace(&Retire);
            MiRetireIoSpacePage(&Retire, Page, Type);
        }
        FirstPte[i].u.Long = 0;
    }

    if (Retire.Count)
    {
        KeFlushEntireTb(TRUE, TRUE);
        MiReleaseRetiredIoSpace(&Retire);
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
            for (Run = 1; (i + Run < PageCount) && !MiGetPfnEntry(Pfn + i + Run); Run++);
            MiMapIoSpacePages(PointerPte + i,
                              Pfn + i,
                              Run,
                              MiPlatformCacheAttributes[TRUE][CacheType],
                              ValidKernelPte);
            continue;
        }
#endif
        Run = 1;
        TempPte.u.Hard.PageFrameNumber = Pfn + i;
        MI_WRITE_VALID_PTE(PointerPte + i, TempPte);
    }

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
    /* Clear the device PTEs and flush before their pages may take another type */
    MiRetireIoSpacePtes(PointerPte, PageCount);
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
