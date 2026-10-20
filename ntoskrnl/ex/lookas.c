/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            ntoskrnl/ex/lookas.c
* PURPOSE:         Lookaside Lists
* PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
*/

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

LIST_ENTRY ExpNonPagedLookasideListHead;
KSPIN_LOCK ExpNonPagedLookasideListLock;
LIST_ENTRY ExpPagedLookasideListHead;
KSPIN_LOCK ExpPagedLookasideListLock;
LIST_ENTRY ExSystemLookasideListHead;
LIST_ENTRY ExPoolLookasideListHead;
GENERAL_LOOKASIDE ExpSmallNPagedPoolLookasideLists[NUMBER_POOL_LOOKASIDE_LISTS];
GENERAL_LOOKASIDE ExpSmallPagedPoolLookasideLists[NUMBER_POOL_LOOKASIDE_LISTS];

/* PRIVATE FUNCTIONS *********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
ExInitializeSystemLookasideList(IN PGENERAL_LOOKASIDE List,
                                IN POOL_TYPE Type,
                                IN ULONG Size,
                                IN ULONG Tag,
                                IN USHORT MaximumDepth,
                                IN PLIST_ENTRY ListHead)
{
    /* Initialize the list */
    List->Tag = Tag;
    List->Type = Type;
    List->Size = Size;
    InsertHeadList(ListHead, &List->ListEntry);
    List->MaximumDepth = MaximumDepth;
    List->Depth = 2;

    /* Special pool has to see every free of a selected allocation (IRP, MDL and pool lists too) */
    if (MmSpecialPoolEnabled && MmSpecialPoolSelectsLookaside(Size, Tag))
    {
        List->MaximumDepth = 0;
        List->Depth = 0;
    }
    List->Allocate = ExAllocatePoolWithTag;
    List->Free = ExFreePool;
    InitializeSListHead(&List->ListHead);
    List->TotalAllocates = 0;
    List->AllocateHits = 0;
    List->TotalFrees = 0;
    List->FreeHits = 0;
    List->LastTotalAllocates = 0;
    List->LastAllocateHits = 0;
}

CODE_SEG("INIT")
VOID
NTAPI
ExInitPoolLookasidePointers(VOID)
{
    ULONG i;
    PKPRCB Prcb = KeGetCurrentPrcb();
    PGENERAL_LOOKASIDE Entry;

    /* Loop for all pool lists */
    for (i = 0; i < NUMBER_POOL_LOOKASIDE_LISTS; i++)
    {
        /* The lists are shared by all processors; only the boot processor initializes them */
        Entry = &ExpSmallNPagedPoolLookasideLists[i];
        if (Prcb->Number == 0) InitializeSListHead(&Entry->ListHead);

        /* Bind to PRCB */
        Prcb->PPNPagedLookasideList[i].P = Entry;
        Prcb->PPNPagedLookasideList[i].L = Entry;

        Entry = &ExpSmallPagedPoolLookasideLists[i];
        if (Prcb->Number == 0) InitializeSListHead(&Entry->ListHead);

        /* Bind to PRCB */
        Prcb->PPPagedLookasideList[i].P = Entry;
        Prcb->PPPagedLookasideList[i].L = Entry;
    }
}

CODE_SEG("INIT")
VOID
NTAPI
ExpInitLookasideLists(VOID)
{
    ULONG i;

    /* Initialize locks and lists */
    InitializeListHead(&ExpNonPagedLookasideListHead);
    InitializeListHead(&ExpPagedLookasideListHead);
    InitializeListHead(&ExSystemLookasideListHead);
    InitializeListHead(&ExPoolLookasideListHead);
    KeInitializeSpinLock(&ExpNonPagedLookasideListLock);
    KeInitializeSpinLock(&ExpPagedLookasideListLock);

    /* Initialize the system lookaside lists */
    for (i = 0; i < NUMBER_POOL_LOOKASIDE_LISTS; i++)
    {
        /* Initialize the non-paged list */
        ExInitializeSystemLookasideList(&ExpSmallNPagedPoolLookasideLists[i],
                                        NonPagedPool,
                                        (i + 1) * 8,
                                        'looP',
                                        256,
                                        &ExPoolLookasideListHead);

        /* Initialize the paged list */
        ExInitializeSystemLookasideList(&ExpSmallPagedPoolLookasideLists[i],
                                        PagedPool,
                                        (i + 1) * 8,
                                        'looP',
                                        256,
                                        &ExPoolLookasideListHead);
    }
}

/* Depth adjustment: called once a second by the balance set manager */
static
VOID
ExpAdjustLookasideList(IN PGENERAL_LOOKASIDE List,
                       IN BOOLEAN CountsHits,
                       IN USHORT MinimumDepth)
{
    ULONG Allocates, Misses, Hits;
    ULONG Depth, Delta;

    /* Special pool lists (and lists with a pinned depth) are left alone */
    if (List->MaximumDepth == 0) return;

    /* Allocates and hits/misses since the last pass (the counters are approximate, so clamp) */
    Allocates = List->TotalAllocates - List->LastTotalAllocates;
    if (CountsHits)
    {
        /* System pool lists count hits */
        Hits = List->AllocateHits - List->LastAllocateHits;
        Misses = (Allocates > Hits) ? (Allocates - Hits) : 0;
        List->LastAllocateHits = List->AllocateHits;
    }
    else
    {
        /* Driver lists count misses */
        Misses = List->AllocateMisses - List->LastAllocateMisses;
        List->LastAllocateMisses = List->AllocateMisses;
    }
    List->LastTotalAllocates = List->TotalAllocates;
    if (Misses > Allocates) Misses = Allocates;

    Depth = List->Depth;
    if (Misses * 100 > Allocates && Misses != 0)
    {
        /* More than 1% of the allocates missed: grow, faster the deeper we already are */
        Delta = Depth / 2;
        if (Delta < 2) Delta = 2;
        Depth += Delta;
        if (Depth > List->MaximumDepth) Depth = List->MaximumDepth;
    }
    else if (Misses == 0 && Allocates < Depth / 2 && Depth > MinimumDepth)
    {
        /* The list is bigger than the allocate rate needs: shrink slowly */
        Depth--;
    }
    if (Depth < MinimumDepth) Depth = min(MinimumDepth, List->MaximumDepth);
    List->Depth = (USHORT)Depth;
}

/* PUBLIC FUNCTIONS **********************************************************/

/* Not exported; declared without NTAPI in the SDK headers, so defined to match */
VOID
ExAdjustLookasideDepth(VOID)
{
    PLIST_ENTRY ListEntry;
    KIRQL OldIrql;

    /* The system pool lists are set up at boot and never removed */
    for (ListEntry = ExPoolLookasideListHead.Flink;
         ListEntry != &ExPoolLookasideListHead;
         ListEntry = ListEntry->Flink)
    {
        ExpAdjustLookasideList(CONTAINING_RECORD(ListEntry, GENERAL_LOOKASIDE, ListEntry),
                               TRUE,
                               2);
    }

    /* Driver lists come and go under their locks */
    KeAcquireSpinLock(&ExpNonPagedLookasideListLock, &OldIrql);
    for (ListEntry = ExpNonPagedLookasideListHead.Flink;
         ListEntry != &ExpNonPagedLookasideListHead;
         ListEntry = ListEntry->Flink)
    {
        ExpAdjustLookasideList(CONTAINING_RECORD(ListEntry, GENERAL_LOOKASIDE, ListEntry),
                               FALSE,
                               4);
    }
    KeReleaseSpinLock(&ExpNonPagedLookasideListLock, OldIrql);

    KeAcquireSpinLock(&ExpPagedLookasideListLock, &OldIrql);
    for (ListEntry = ExpPagedLookasideListHead.Flink;
         ListEntry != &ExpPagedLookasideListHead;
         ListEntry = ListEntry->Flink)
    {
        ExpAdjustLookasideList(CONTAINING_RECORD(ListEntry, GENERAL_LOOKASIDE, ListEntry),
                               FALSE,
                               4);
    }
    KeReleaseSpinLock(&ExpPagedLookasideListLock, OldIrql);
}

/*
 * @implemented
 */
PVOID
NTAPI
ExiAllocateFromPagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside)
{
    PVOID Entry;

    Lookaside->L.TotalAllocates++;
    Entry = InterlockedPopEntrySList(&Lookaside->L.ListHead);
    if (!Entry)
    {
        Lookaside->L.AllocateMisses++;
        Entry = (Lookaside->L.Allocate)(Lookaside->L.Type,
                                        Lookaside->L.Size,
                                        Lookaside->L.Tag);
    }
    return Entry;
}

/*
 * @implemented
 */
VOID
NTAPI
ExiFreeToPagedLookasideList(IN PPAGED_LOOKASIDE_LIST  Lookaside,
                            IN PVOID  Entry)
{
    Lookaside->L.TotalFrees++;
    if (ExQueryDepthSList(&Lookaside->L.ListHead) >= Lookaside->L.Depth)
    {
        Lookaside->L.FreeMisses++;
        (Lookaside->L.Free)(Entry);
    }
    else
    {
        InterlockedPushEntrySList(&Lookaside->L.ListHead, (PSLIST_ENTRY)Entry);
    }
}

/*
 * @implemented
 */
VOID
NTAPI
ExDeleteNPagedLookasideList(IN PNPAGED_LOOKASIDE_LIST Lookaside)
{
    KIRQL OldIrql;
    PVOID Entry;

    /* Pop all entries off the stack and release their resources */
    for (;;)
    {
        Entry = InterlockedPopEntrySList(&Lookaside->L.ListHead);
        if (!Entry) break;
        (*Lookaside->L.Free)(Entry);
    }

    /* Remove from list */
    KeAcquireSpinLock(&ExpNonPagedLookasideListLock, &OldIrql);
    RemoveEntryList(&Lookaside->L.ListEntry);
    KeReleaseSpinLock(&ExpNonPagedLookasideListLock, OldIrql);
}

/*
 * @implemented
 */
VOID
NTAPI
ExDeletePagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside)
{
    KIRQL OldIrql;
    PVOID Entry;

    /* Pop all entries off the stack and release their resources */
    for (;;)
    {
        Entry = InterlockedPopEntrySList(&Lookaside->L.ListHead);
        if (!Entry) break;
        (*Lookaside->L.Free)(Entry);
    }

    /* Remove from list */
    KeAcquireSpinLock(&ExpPagedLookasideListLock, &OldIrql);
    RemoveEntryList(&Lookaside->L.ListEntry);
    KeReleaseSpinLock(&ExpPagedLookasideListLock, OldIrql);
}

/*
 * @implemented
 */
VOID
NTAPI
ExInitializeNPagedLookasideList(IN PNPAGED_LOOKASIDE_LIST Lookaside,
                                IN PALLOCATE_FUNCTION Allocate OPTIONAL,
                                IN PFREE_FUNCTION Free OPTIONAL,
                                IN ULONG Flags,
                                IN SIZE_T Size,
                                IN ULONG Tag,
                                IN USHORT Depth)
{
    /* Initialize the Header */
    ExInitializeSListHead(&Lookaside->L.ListHead);
    Lookaside->L.TotalAllocates = 0;
    Lookaside->L.AllocateMisses = 0;
    Lookaside->L.TotalFrees = 0;
    Lookaside->L.FreeMisses = 0;
    Lookaside->L.Type = NonPagedPool | Flags;
    Lookaside->L.Tag = Tag;
    Lookaside->L.Size = (ULONG)Size;
    Lookaside->L.Depth = 4;
    Lookaside->L.MaximumDepth = 256;
    Lookaside->L.LastTotalAllocates = 0;
    Lookaside->L.LastAllocateMisses = 0;

    /* Special pool has to see every free of a selected allocation */
    if (MmSpecialPoolEnabled && MmSpecialPoolSelectsLookaside(Size, Tag))
    {
        Lookaside->L.Depth = 0;
        Lookaside->L.MaximumDepth = 0;
    }

    /* Set the Allocate/Free Routines */
    if (Allocate)
    {
        Lookaside->L.Allocate = Allocate;
    }
    else
    {
        Lookaside->L.Allocate = ExAllocatePoolWithTag;
    }

    if (Free)
    {
        Lookaside->L.Free = Free;
    }
    else
    {
        Lookaside->L.Free = ExFreePool;
    }

    /* Insert it into the list */
    ExInterlockedInsertTailList(&ExpNonPagedLookasideListHead,
                                &Lookaside->L.ListEntry,
                                &ExpNonPagedLookasideListLock);
}

/*
 * @implemented
 */
VOID
NTAPI
ExInitializePagedLookasideList(IN PPAGED_LOOKASIDE_LIST Lookaside,
                               IN PALLOCATE_FUNCTION Allocate OPTIONAL,
                               IN PFREE_FUNCTION Free OPTIONAL,
                               IN ULONG Flags,
                               IN SIZE_T Size,
                               IN ULONG Tag,
                               IN USHORT Depth)
{
    /* Initialize the Header */
    ExInitializeSListHead(&Lookaside->L.ListHead);
    Lookaside->L.TotalAllocates = 0;
    Lookaside->L.AllocateMisses = 0;
    Lookaside->L.TotalFrees = 0;
    Lookaside->L.FreeMisses = 0;
    Lookaside->L.Type = PagedPool | Flags;
    Lookaside->L.Tag = Tag;
    Lookaside->L.Size = (ULONG)Size;
    Lookaside->L.Depth = 4;
    Lookaside->L.MaximumDepth = 256;
    Lookaside->L.LastTotalAllocates = 0;
    Lookaside->L.LastAllocateMisses = 0;

    /* Special pool has to see every free of a selected allocation */
    if (MmSpecialPoolEnabled && MmSpecialPoolSelectsLookaside(Size, Tag))
    {
        Lookaside->L.Depth = 0;
        Lookaside->L.MaximumDepth = 0;
    }

    /* Set the Allocate/Free Routines */
    if (Allocate)
    {
        Lookaside->L.Allocate = Allocate;
    }
    else
    {
        Lookaside->L.Allocate = ExAllocatePoolWithTag;
    }

    if (Free)
    {
        Lookaside->L.Free = Free;
    }
    else
    {
        Lookaside->L.Free = ExFreePool;
    }

    /* Insert it into the list */
    ExInterlockedInsertTailList(&ExpPagedLookasideListHead,
                                &Lookaside->L.ListEntry,
                                &ExpPagedLookasideListLock);
}

/* EOF */
