/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        lib/drivers/hidparser/parser.c
 * PURPOSE:     HID Parser
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "parser.h"

#include <ntintsafe.h>

#define NDEBUG
#include <debug.h>

static UCHAR ItemSize[4] = { 0, 1, 2, 4 };

//
// Descriptor-level sanity limits. Report descriptors come from untrusted
// hardware (or anything a VM presents as a HID device), so every quantity
// built from descriptor fields is bounded before it drives an allocation,
// an index or a recursion. These caps are far above any real device and
// below the point where the derived arithmetic can wrap a ULONG.
//
// Fields wider than 32 bits are accepted (some vendor reports use them); only
// value extraction of such a field through the HidP value APIs is refused. The
// collection and push caps bound the parser's recursion and backing stacks so a
// malformed descriptor cannot exhaust the kernel stack or pool.
//
#define HID_MAX_REPORT_COUNT        0x00010000
#define HID_MAX_GLOBAL_STACK_DEPTH  256
#define HID_MAX_USAGE_STACK         0x00010000

//
// Cumulative report-item cap across the whole descriptor. Bounds the transient
// nonpaged-pool the growing report buffers hold while parsing (each item is
// sizeof(HID_REPORT_ITEM)), independent of the per-item HID_MAX_REPORT_COUNT.
// Capped at USHORT max so the public USHORT capability counters (HIDP_CAPS
// NumberXxxValueCaps/ButtonCaps/DataIndices and the ValueCaps match counter in
// GetSpecificValueCaps) cannot wrap.
//
#define HID_MAX_TOTAL_REPORT_ITEMS  0x0000FFFF

//
// The collection tree is walked recursively (HidParser_FindReportInCollection,
// HidParser_CalculateCollectionSize, HidParser_StoreCollection). Measured cost
// is ~48 bytes of stack per level on i386 at -Os/-O2, so the whole tree must
// stay well inside the ~12 KiB kernel stack together with the driver callers
// above us. 64 levels (~3 KiB) is far deeper than any real device and leaves
// ample headroom.
//
#define HID_MAX_COLLECTION_DEPTH    64

//
// Canonical long-item prefix byte (USB HID 1.11 6.2.2.3) and its fixed header
// length: prefix + bDataSize + bLongItemTag.
//
#define HID_LONG_ITEM_PREFIX        0xFE
#define HID_LONG_ITEM_HEADER        3

FORCEINLINE
NTSTATUS
HidParser_SizeAdd(
    IN ULONG Augend,
    IN ULONG Addend,
    OUT PULONG Result)
{
    return RtlULongAdd(Augend, Addend, Result);
}

FORCEINLINE
NTSTATUS
HidParser_SizeMult(
    IN ULONG Multiplicand,
    IN ULONG Multiplier,
    OUT PULONG Result)
{
    return RtlULongMult(Multiplicand, Multiplier, Result);
}

VOID
HidParser_DeleteReport(
    IN PHID_REPORT Report)
{
    //
    // not implemented
    //
}

VOID
HidParser_FreeCollection(
    IN PHID_COLLECTION Collection)
{
    //
    // not implemented
    //
}

NTSTATUS
HidParser_AllocateCollection(
    IN PHID_COLLECTION ParentCollection,
    IN UCHAR Type,
    IN PLOCAL_ITEM_STATE LocalItemState,
    OUT PHID_COLLECTION * OutCollection)
{
    PHID_COLLECTION Collection;
    USAGE_VALUE UsageValue;

    //
    // first allocate the collection
    //
    Collection = (PHID_COLLECTION)AllocFunction(sizeof(HID_COLLECTION));
    if (!Collection)
    {
        //
        // no memory
        //
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    //
    // init collection
    //
    Collection->Root = ParentCollection;
    Collection->Type = Type;
    Collection->StringID = LocalItemState->StringIndex;
    Collection->PhysicalID = LocalItemState->DesignatorIndex;

    //
    // set Usage
    //
    ASSERT(LocalItemState);
    ASSERT(LocalItemState->UsageStack);

    if (LocalItemState->UsageStackUsed > 0)
    {
        //
        // usage value from first local stack item
        //
        UsageValue.u.Extended = LocalItemState->UsageStack[0].u.Extended;
    }
    else if (LocalItemState->UsageMinimumSet)
    {
        //
        // use value from minimum
        //
        UsageValue.u.Extended = LocalItemState->UsageMinimum.u.Extended;
    }
    else if (LocalItemState->UsageMaximumSet)
    {
        //
        // use value from maximum
        //
        UsageValue.u.Extended = LocalItemState->UsageMaximum.u.Extended;
    }
    else if (Type == COLLECTION_LOGICAL)
    {
        //
        // root collection
        //
        UsageValue.u.Extended = 0;
    }
    else
    {
        //
        // no usage set
        //
        DebugFunction("HIDPARSE] No usage set\n");
        UsageValue.u.Extended = 0;
    }

    //
    // store usage
    //
    Collection->Usage = UsageValue.u.Extended;

    //
    // store result
    //
    *OutCollection = Collection;

    //
    // done
    //
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
HidParser_AddCollection(
    IN PHID_COLLECTION CurrentCollection,
    IN PHID_COLLECTION NewCollection)
{
    PHID_COLLECTION * NewAllocCollection;
    ULONG CollectionCount;
    ULONG AllocSize;

    //
    // increment collection array
    //
    if (!NT_SUCCESS(HidParser_SizeAdd(CurrentCollection->NodeCount, 1, &CollectionCount)))
        return HIDP_STATUS_INTERNAL_ERROR;

    //
    // allocate new collection (overflow-checked)
    //
    if (!NT_SUCCESS(HidParser_SizeMult(sizeof(PHID_COLLECTION), CollectionCount, &AllocSize)))
        return HIDP_STATUS_INTERNAL_ERROR;

    NewAllocCollection = (PHID_COLLECTION*)AllocFunction(AllocSize);
    if (!NewAllocCollection)
    {
        //
        // no memory
        //
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    if (CurrentCollection->NodeCount)
    {
        //
        // copy old array
        //
        CopyFunction(NewAllocCollection, CurrentCollection->Nodes, CurrentCollection->NodeCount * sizeof(PHID_COLLECTION));

        //
        // delete old array
        //
        FreeFunction(CurrentCollection->Nodes);
    }

    //
    // insert new item
    //
    NewAllocCollection[CurrentCollection->NodeCount] = (struct __HID_COLLECTION__*)NewCollection;


    //
    // store new array
    //
    CurrentCollection->Nodes = NewAllocCollection;
    CurrentCollection->NodeCount++;

    //
    // done
    //
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
HidParser_FindReportInCollection(
    IN PHID_COLLECTION Collection,
    IN UCHAR ReportType,
    IN UCHAR ReportID,
    OUT PHID_REPORT *OutReport)
{
    ULONG Index;
    NTSTATUS Status;

    //
    // search in local list
    //
    for(Index = 0; Index < Collection->ReportCount; Index++)
    {
        if (Collection->Reports[Index]->Type == ReportType && Collection->Reports[Index]->ReportID == ReportID)
        {
            //
            // found report
            //
            *OutReport = Collection->Reports[Index];
            return HIDP_STATUS_SUCCESS;
        }
    }

    //
    // search in sub collections
    //
    for(Index = 0; Index < Collection->NodeCount; Index++)
    {
        Status = HidParser_FindReportInCollection(Collection->Nodes[Index], ReportType, ReportID, OutReport);
        if (Status == HIDP_STATUS_SUCCESS)
            return Status;
    }

    //
    // no such report found
    //
    *OutReport = NULL;
    return HIDP_STATUS_REPORT_DOES_NOT_EXIST;
}


NTSTATUS
HidParser_FindReport(
    IN PHID_PARSER_CONTEXT ParserContext,
    IN UCHAR ReportType,
    IN UCHAR ReportID,
    OUT PHID_REPORT *OutReport)
{
    //
    // search in current top level collection
    //
    return HidParser_FindReportInCollection(ParserContext->RootCollection->Nodes[ParserContext->RootCollection->NodeCount-1], ReportType, ReportID, OutReport);
}

NTSTATUS
HidParser_AllocateReport(
    IN UCHAR ReportType,
    IN UCHAR ReportID,
    OUT PHID_REPORT *OutReport)
{
    PHID_REPORT Report;

    //
    // allocate report
    //
    Report = (PHID_REPORT)AllocFunction(sizeof(HID_REPORT));
    if (!Report)
    {
        //
        // no memory
        //
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    //
    // init report
    //
    Report->ReportID = ReportID;
    Report->Type = ReportType;

    //
    // done
    //
    *OutReport = Report;
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
HidParser_AddReportToCollection(
    IN PHID_PARSER_CONTEXT ParserContext,
    IN PHID_COLLECTION CurrentCollection,
    IN PHID_REPORT NewReport)
{
    PHID_REPORT * NewReportArray;
    ULONG ReportArrayCount;
    ULONG AllocSize;

    //
    // allocate new report array (overflow-checked)
    //
    if (!NT_SUCCESS(HidParser_SizeAdd(CurrentCollection->ReportCount, 1, &ReportArrayCount)))
        return HIDP_STATUS_INTERNAL_ERROR;

    if (!NT_SUCCESS(HidParser_SizeMult(sizeof(PHID_REPORT), ReportArrayCount, &AllocSize)))
        return HIDP_STATUS_INTERNAL_ERROR;

    NewReportArray = (PHID_REPORT*)AllocFunction(AllocSize);
    if (!NewReportArray)
    {
        //
        // no memory
        //
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    if (CurrentCollection->ReportCount)
    {
        //
        // copy old array contents
        //
        CopyFunction(NewReportArray, CurrentCollection->Reports, sizeof(PHID_REPORT) * CurrentCollection->ReportCount);

        //
        // free old array
        //
        FreeFunction(CurrentCollection->Reports);
    }

    //
    // store result
    //
    NewReportArray[CurrentCollection->ReportCount] = NewReport;
    CurrentCollection->Reports = NewReportArray;
    CurrentCollection->ReportCount++;

    //
    // completed successfully
    //
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
HidParser_GetReport(
    IN PHID_PARSER_CONTEXT ParserContext,
    IN PHID_COLLECTION Collection,
    IN UCHAR ReportType,
    IN UCHAR ReportID,
    IN UCHAR CreateIfNotExists,
    OUT PHID_REPORT *OutReport)
{
    NTSTATUS Status;

    //
    // try finding existing report
    //
    Status = HidParser_FindReport(ParserContext, ReportType, ReportID, OutReport);
    if (Status == HIDP_STATUS_SUCCESS || CreateIfNotExists == FALSE)
    {
        //
        // founed report
        //
        return Status;
    }

    //
    // allocate new report
    //
    Status = HidParser_AllocateReport(ReportType, ReportID, OutReport);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        //
        // failed to allocate report
        //
        return Status;
    }

    //
    // add report
    //
    Status = HidParser_AddReportToCollection(ParserContext, Collection, *OutReport);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        //
        // failed to allocate report
        //
        FreeFunction(*OutReport);
    }

    //
    // done
    //
    return Status;
}

NTSTATUS
HidParser_ReserveReportItems(
    IN PHID_REPORT Report,
    IN ULONG ReportCount,
    OUT PHID_REPORT *OutReport)
{
    PHID_REPORT NewReport;
    ULONG OldSize, Size, TotalSize, Needed;

    //
    // reject absurd report counts up front; everything below stays inside a
    // ULONG once this holds, but the arithmetic is still checked explicitly.
    //
    if (ReportCount > HID_MAX_REPORT_COUNT)
        return HIDP_STATUS_INTERNAL_ERROR;

    //
    // required item count after the grow (overflow-checked)
    //
    if (!NT_SUCCESS(HidParser_SizeAdd(Report->ItemCount, ReportCount, &Needed)))
        return HIDP_STATUS_INTERNAL_ERROR;

    if (Needed <= Report->ItemAllocated)
    {
        //
        // space is already allocated
        //
        *OutReport = Report;
        return HIDP_STATUS_SUCCESS;
    }

    //
    // calculate new size (every term overflow-checked)
    //
    if (!NT_SUCCESS(HidParser_SizeMult(Report->ItemCount, sizeof(HID_REPORT_ITEM), &OldSize)) ||
        !NT_SUCCESS(HidParser_SizeAdd(OldSize, sizeof(HID_REPORT), &OldSize)))
    {
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    if (!NT_SUCCESS(HidParser_SizeMult(ReportCount, sizeof(HID_REPORT_ITEM), &Size)))
        return HIDP_STATUS_INTERNAL_ERROR;

    if (!NT_SUCCESS(HidParser_SizeAdd(OldSize, Size, &TotalSize)))
        return HIDP_STATUS_INTERNAL_ERROR;

    //
    // allocate memory
    //
    NewReport = (PHID_REPORT)AllocFunction(TotalSize);
    if (!NewReport)
    {
        //
        // no memory
        //
        return HIDP_STATUS_INTERNAL_ERROR;
    }


    //
    // copy old report
    //
    CopyFunction(NewReport, Report, OldSize);

    //
    // increase array size
    //
    NewReport->ItemAllocated += ReportCount;

    //
    // store result
    //
    *OutReport = NewReport;

    //
    // completed sucessfully
    //
    return HIDP_STATUS_SUCCESS;
}

VOID
HidParser_SignRange(
    IN ULONG Minimum,
    IN ULONG Maximum,
    OUT PULONG NewMinimum,
    OUT PULONG NewMaximum)
{
    ULONG Mask = 0x80000000;
    ULONG Index;

    for (Index = 0; Index < 4; Index++)
    {
        if (Minimum & Mask)
        {
            Minimum |= Mask;
            if (Maximum & Mask)
                Maximum |= Mask;
            return;
        }

        Mask >>= 8;
        Mask |= 0xff000000;
    }

    *NewMinimum = Minimum;
    *NewMaximum = Maximum;
}

NTSTATUS
HidParser_InitReportItem(
    IN PHID_REPORT Report,
    IN PHID_REPORT_ITEM ReportItem,
    IN PGLOBAL_ITEM_STATE GlobalItemState,
    IN PLOCAL_ITEM_STATE LocalItemState,
    IN PMAIN_ITEM_DATA ItemData,
    IN ULONG ReportItemIndex)
{
    ULONG LogicalMinimum;
    ULONG LogicalMaximum;
    ULONG PhysicalMinimum;
    ULONG PhysicalMaximum;
    ULONG UsageMinimum;
    ULONG UsageMaximum;
    USAGE_VALUE UsageValue;

    //
    // get logical bounds
    //
    LogicalMinimum = GlobalItemState->LogicalMinimum;
    LogicalMaximum = GlobalItemState->LogicialMaximum;
    if (LogicalMinimum > LogicalMaximum)
    {
        //
        // make them signed
        //
        HidParser_SignRange(LogicalMinimum, LogicalMaximum, &LogicalMinimum, &LogicalMaximum);
    }
    //ASSERT(LogicalMinimum <= LogicalMaximum);

    //
    // get physical bounds
    //
    PhysicalMinimum = GlobalItemState->PhysicalMinimum;
    PhysicalMaximum = GlobalItemState->PhysicalMaximum;
    if (PhysicalMinimum > PhysicalMaximum)
    {
        //
        // make them signed
        //
        HidParser_SignRange(PhysicalMinimum, PhysicalMaximum, &PhysicalMinimum, &PhysicalMaximum);
    }
    //ASSERT(PhysicalMinimum <= PhysicalMaximum);

    //
    // get usage bounds
    //
    UsageMinimum = 0;
    UsageMaximum = 0;
    if (ItemData->ArrayVariable == FALSE)
    {
        //
        // get usage bounds
        //
        UsageMinimum = LocalItemState->UsageMinimum.u.Extended;
        UsageMaximum = LocalItemState->UsageMaximum.u.Extended;
    }
    else
    {
        //
        // get usage value from stack
        //
        if (ReportItemIndex < LocalItemState->UsageStackUsed)
        {
            //
            // use stack item
            //
            UsageValue = LocalItemState->UsageStack[ReportItemIndex];
        }
        else
        {
            //
            // get usage minimum from local state
            //
            UsageValue = LocalItemState->UsageMinimum;

            //
            // append item index
            //
            UsageValue.u.Extended += ReportItemIndex;

            if (LocalItemState->UsageMaximumSet)
            {
                if (UsageValue.u.Extended > LocalItemState->UsageMaximum.u.Extended)
                {
                    //
                    // maximum reached
                    //
                    UsageValue.u.Extended = LocalItemState->UsageMaximum.u.Extended;
                }
            }
        }

        //
        // usage usage bounds
        //
        UsageMinimum = UsageMaximum = UsageValue.u.Extended;
    }

    //
    // now store all values
    //
    ReportItem->ByteOffset = (Report->ReportSize / 8);
    ReportItem->Shift = (Report->ReportSize % 8);
    //
    // a field is at most 32 bits; shifting a 32-bit value by 32 or more is
    // undefined, so a full-width (or over-width) field masks every bit.
    //
    if (GlobalItemState->ReportSize >= 32)
        ReportItem->Mask = 0xFFFFFFFF;
    else
        ReportItem->Mask = ~(0xFFFFFFFF << GlobalItemState->ReportSize);
    ReportItem->BitCount = GlobalItemState->ReportSize;
    ReportItem->HasData = (ItemData->DataConstant == FALSE);
    ReportItem->Array = (ItemData->ArrayVariable == 0);
    ReportItem->Relative = (ItemData->Relative != FALSE);
    ReportItem->Minimum = LogicalMinimum;
    ReportItem->Maximum = LogicalMaximum;
    ReportItem->UsageMinimum = UsageMinimum;
    ReportItem->UsageMaximum = UsageMaximum;

    //
    // increment report size (overflow-checked: the running total feeds byte
    // offsets and the context size)
    //
    if (!NT_SUCCESS(HidParser_SizeAdd(Report->ReportSize, GlobalItemState->ReportSize, &Report->ReportSize)))
        return HIDP_STATUS_INTERNAL_ERROR;

    //
    // completed successfully
    //
    return HIDP_STATUS_SUCCESS;
}

BOOLEAN
HidParser_UpdateCurrentCollectionReport(
    IN PHID_COLLECTION Collection,
    IN PHID_REPORT Report,
    IN PHID_REPORT NewReport)
{
    ULONG Index;
    BOOLEAN Found = FALSE, TempFound;

    //
    // search in local list
    //
    for(Index = 0; Index < Collection->ReportCount; Index++)
    {
        if (Collection->Reports[Index] == Report)
        {
            //
            // update report
            //
            Collection->Reports[Index] = NewReport;
            Found = TRUE;
        }
    }

    //
    // search in sub collections
    //
    for(Index = 0; Index < Collection->NodeCount; Index++)
    {
        //
        // was it found
        //
        TempFound = HidParser_UpdateCurrentCollectionReport(Collection->Nodes[Index], Report, NewReport);
        if (TempFound)
        {
            //
            // the same report should not be found in different collections
            //
            ASSERT(Found == FALSE);
            Found = TRUE;
        }
    }

    //
    // done
    //
    return Found;
}

BOOLEAN
HidParser_UpdateCollectionReport(
    IN PHID_PARSER_CONTEXT ParserContext,
    IN PHID_REPORT Report,
    IN PHID_REPORT NewReport)
{
    //
    // update in current collection
    //
    return HidParser_UpdateCurrentCollectionReport(ParserContext->RootCollection->Nodes[ParserContext->RootCollection->NodeCount-1], Report, NewReport);
}


NTSTATUS
HidParser_AddMainItem(
    IN PHID_PARSER_CONTEXT ParserContext,
    IN PHID_REPORT Report,
    IN PGLOBAL_ITEM_STATE GlobalItemState,
    IN PLOCAL_ITEM_STATE LocalItemState,
    IN PMAIN_ITEM_DATA ItemData,
    IN PHID_COLLECTION Collection)
{
    NTSTATUS Status;
    ULONG Index;
    PHID_REPORT NewReport;
    BOOLEAN Found;

    //
    // a zero-width field contributes no bits and no readable data; skip it so
    // it creates no report item (a size-0 data item otherwise produces a
    // ByteOffset at the report end that the value APIs would read past)
    //
    if (GlobalItemState->ReportSize == 0)
        return HIDP_STATUS_SUCCESS;

    //
    // first grow report item array
    //
    Status = HidParser_ReserveReportItems(Report, GlobalItemState->ReportCount, &NewReport);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        //
        // failed to allocate memory
        //
        return Status;
    }

    if (NewReport != Report)
    {
        //
        // update current top level collection
        //
        Found = HidParser_UpdateCollectionReport(ParserContext, Report, NewReport);
        ASSERT(Found);

        //
        // ReserveReportItems allocated a larger buffer and copied the old one
        // across; the old report is now unreferenced and must be freed, or a
        // growing report orphans a buffer per growth (nonpaged-pool exhaustion
        // from a short descriptor)
        //
        if (Found)
            FreeFunction(Report);
    }

    //
    // sanity check
    //
    ASSERT(NewReport->ItemCount + GlobalItemState->ReportCount <= NewReport->ItemAllocated);

    for(Index = 0; Index < GlobalItemState->ReportCount; Index++)
    {
        Status = HidParser_InitReportItem(NewReport, &NewReport->Items[NewReport->ItemCount], GlobalItemState, LocalItemState, ItemData, Index);
        if (Status != HIDP_STATUS_SUCCESS)
        {
            //
            // failed to init report item
            //
            return Status;
        }

        //
        // increment report item count
        //
        NewReport->ItemCount++;
    }

    //
    // done
    //
    return HIDP_STATUS_SUCCESS;
}

//
// Free a collection and everything it owns (its reports, its report array,
// its child collections and their node array), then the collection itself.
// Recursion is bounded by HID_MAX_COLLECTION_DEPTH, enforced while parsing.
//
static
VOID
HidParser_FreeCollectionTree(
    IN PHID_COLLECTION Collection)
{
    ULONG Index;

    if (Collection == NULL)
        return;

    for (Index = 0; Index < Collection->NodeCount; Index++)
        HidParser_FreeCollectionTree(Collection->Nodes[Index]);

    if (Collection->Nodes)
        FreeFunction(Collection->Nodes);

    for (Index = 0; Index < Collection->ReportCount; Index++)
    {
        if (Collection->Reports[Index])
            FreeFunction(Collection->Reports[Index]);
    }

    if (Collection->Reports)
        FreeFunction(Collection->Reports);

    FreeFunction(Collection);
}

//
// Tear down a parser context and everything reachable from it. Used on every
// error path while parsing, and by HidParser_GetCollectionDescription once the
// per-collection preparsed contexts have been built (the tree is a scratch
// representation that was previously leaked).
//
VOID
HidParser_FreeContext(
    IN PHID_PARSER_CONTEXT ParserContext)
{
    PGLOBAL_ITEM_STATE LinkedGlobalItemState, NextLinkedGlobalItemState;

    if (ParserContext == NULL)
        return;

    HidParser_FreeCollectionTree(ParserContext->RootCollection);

    LinkedGlobalItemState = (PGLOBAL_ITEM_STATE)ParserContext->GlobalItemState.Next;
    while (LinkedGlobalItemState != NULL)
    {
        NextLinkedGlobalItemState = (PGLOBAL_ITEM_STATE)LinkedGlobalItemState->Next;
        FreeFunction(LinkedGlobalItemState);
        LinkedGlobalItemState = NextLinkedGlobalItemState;
    }

    if (ParserContext->LocalItemState.UsageStack)
        FreeFunction(ParserContext->LocalItemState.UsageStack);

    FreeFunction(ParserContext);
}

NTSTATUS
HidParser_ParseReportDescriptor(
    IN PUCHAR ReportDescriptor,
    IN ULONG ReportLength,
    OUT PVOID *OutParser)
{
    PGLOBAL_ITEM_STATE LinkedGlobalItemState, NextLinkedGlobalItemState;
    ULONG Index;
    PUSAGE_VALUE NewUsageStack, UsageValue;
    NTSTATUS Status;
    PHID_COLLECTION CurrentCollection, NewCollection;
    PUCHAR CurrentOffset, ReportEnd;
    PITEM_PREFIX CurrentItem;
    ULONG CurrentItemSize;
    PLONG_ITEM CurrentLongItem;
    PSHORT_ITEM CurrentShortItem;
    ULONG Data;
    UCHAR ReportType;
    PHID_REPORT Report;
    PMAIN_ITEM_DATA MainItemData;
    PHID_PARSER_CONTEXT ParserContext;
    ULONG CollectionDepth = 0;
    ULONG GlobalStackDepth = 0;
    ULONG TotalReportItems = 0;
    SIZE_T Remaining;

    CurrentOffset = ReportDescriptor;
    ReportEnd = ReportDescriptor + ReportLength;

    if (ReportDescriptor >= ReportEnd)
        return HIDP_STATUS_USAGE_NOT_FOUND;

    //
    // allocate parser
    //
    ParserContext = AllocFunction(sizeof(HID_PARSER_CONTEXT));
    if (!ParserContext)
        return HIDP_STATUS_INTERNAL_ERROR;


    //
    // allocate usage stack
    //
    ParserContext->LocalItemState.UsageStackAllocated = 10;
    ParserContext->LocalItemState.UsageStack = (PUSAGE_VALUE)AllocFunction(ParserContext->LocalItemState.UsageStackAllocated * sizeof(USAGE_VALUE));
    if (!ParserContext->LocalItemState.UsageStack)
    {
        //
        // no memory
        //
        FreeFunction(ParserContext);
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    //
    // now allocate root collection
    //
    Status = HidParser_AllocateCollection(NULL, COLLECTION_LOGICAL, &ParserContext->LocalItemState, &ParserContext->RootCollection);
    if (Status != HIDP_STATUS_SUCCESS)
    {
        //
        // no memory
        //
        FreeFunction(ParserContext->LocalItemState.UsageStack);
        ParserContext->LocalItemState.UsageStack = NULL;
        FreeFunction(ParserContext);
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    //
    // start parsing
    //
    CurrentCollection = ParserContext->RootCollection;

    do
    {
        //
        // get current item; the loop condition guarantees at least the
        // one-byte prefix is in range
        //
        CurrentItem = (PITEM_PREFIX)CurrentOffset;
        Remaining = (SIZE_T)(ReportEnd - CurrentOffset);

        //
        // get item size
        //
        CurrentItemSize = ItemSize[CurrentItem->Size];
        Data = 0;

        if (CurrentItem->Type == ITEM_TYPE_LONG)
        {
            //
            // only the canonical long-item prefix (0xFE) is defined; any other
            // reserved type-3 prefix (e.g. 0xFD) is malformed. A long item's
            // header is three bytes: prefix + bDataSize + bLongItemTag, all of
            // which must be present before any of them is read.
            //
            if (*(PUCHAR)CurrentItem != HID_LONG_ITEM_PREFIX ||
                Remaining < HID_LONG_ITEM_HEADER)
            {
                Status = HIDP_STATUS_INTERNAL_ERROR;
                goto ParseError;
            }

            //
            // item size (sans prefix) is bDataSize + bLongItemTag byte + the
            // payload, so the whole item spans HID_LONG_ITEM_HEADER + DataSize
            //
            CurrentLongItem = (PLONG_ITEM)CurrentItem;
            CurrentItemSize = (HID_LONG_ITEM_HEADER - sizeof(ITEM_PREFIX)) + CurrentLongItem->DataSize;
        }

        //
        // the whole item (prefix + value/payload bytes) must lie inside the
        // descriptor; a truncated final item is rejected rather than read OOB
        //
        if (Remaining < (SIZE_T)sizeof(ITEM_PREFIX) + CurrentItemSize)
        {
            Status = HIDP_STATUS_INTERNAL_ERROR;
            goto ParseError;
        }

        if (CurrentItem->Type != ITEM_TYPE_LONG)
        {
            //
            // get short item
            //
            CurrentShortItem = (PSHORT_ITEM)CurrentItem;

            //
            // get associated data
            //
            //ASSERT(CurrentItemSize == 1 || CurrentItemSize == 2 || CurrentItemSize == 4);
            if (CurrentItemSize == 1)
                Data = CurrentShortItem->Data.UData8[0];
            else if (CurrentItemSize == 2)
                Data = CurrentShortItem->Data.UData16[0];
            else if (CurrentItemSize == 4)
                Data = CurrentShortItem->Data.UData32;
            else
            {
                //
                // invalid item size
                //
                //DebugFunction("CurrentItem invalid item size %lu\n", CurrentItemSize);
            }

        }
        DebugFunction("Tag %x Type %x Size %x Offset %lu Length %lu\n", CurrentItem->Tag, CurrentItem->Type, CurrentItem->Size,  ((ULONG_PTR)CurrentItem - (ULONG_PTR)ReportDescriptor), ReportLength);
        //
        // handle items
        //
        ASSERT(CurrentItem->Type >= ITEM_TYPE_MAIN && CurrentItem->Type <= ITEM_TYPE_LONG);
        switch(CurrentItem->Type)
        {
            case ITEM_TYPE_MAIN:
            {
                // preprocess the local state if relevant (usages for
                // collections and report items)
                if (CurrentItem->Tag != ITEM_TAG_MAIN_END_COLLECTION)
                {
                    // make all usages extended for easier later processing
                    for (Index = 0; Index < ParserContext->LocalItemState.UsageStackUsed; Index++)
                    {
                        //
                        // is it already extended
                        //
                        if (ParserContext->LocalItemState.UsageStack[Index].IsExtended)
                            continue;

                        //
                        // extend usage item
                        //
                        ParserContext->LocalItemState.UsageStack[Index].u.s.UsagePage = ParserContext->GlobalItemState.UsagePage;
                        ParserContext->LocalItemState.UsageStack[Index].IsExtended = TRUE;
                    }

                    if (!ParserContext->LocalItemState.UsageMinimum.IsExtended) {
                        // the specs say if one of them is extended they must
                        // both be extended, so if the minimum isn't, the
                        // maximum mustn't either.
                        ParserContext->LocalItemState.UsageMinimum.u.s.UsagePage
                            = ParserContext->LocalItemState.UsageMaximum.u.s.UsagePage
                                = ParserContext->GlobalItemState.UsagePage;
                        ParserContext->LocalItemState.UsageMinimum.IsExtended
                            = ParserContext->LocalItemState.UsageMaximum.IsExtended = TRUE;
                    }

                    //LocalItemState.usage_stack = usageStack;
                    //ParserContext->LocalItemState.UsageStackUsed = UsageStackUsed;
                }

                if (CurrentItem->Tag == ITEM_TAG_MAIN_COLLECTION) {

                    //
                    // bound nesting depth: the collection tree drives
                    // recursion in the context builder, so a deeply nested
                    // descriptor must not exhaust the kernel stack
                    //
                    if (CollectionDepth >= HID_MAX_COLLECTION_DEPTH)
                    {
                        Status = HIDP_STATUS_INTERNAL_ERROR;
                        goto ParseError;
                    }

                    //
                    // allocate new collection
                    //
                    Status = HidParser_AllocateCollection(CurrentCollection, (UCHAR)Data, &ParserContext->LocalItemState, &NewCollection);
                    if (Status != HIDP_STATUS_SUCCESS)
                        goto ParseError;

                    //
                    // add new collection to current collection
                    //
                    Status = HidParser_AddCollection(CurrentCollection, NewCollection);
                    if (Status != HIDP_STATUS_SUCCESS)
                    {
                        //
                        // NewCollection is not yet linked into the tree, so
                        // free it here before unwinding
                        //
                        HidParser_FreeCollectionTree(NewCollection);
                        goto ParseError;
                    }

                    //
                    // make new collection current
                    //
                    CurrentCollection = NewCollection;
                    CollectionDepth++;
                }
                else if (CurrentItem->Tag == ITEM_TAG_MAIN_END_COLLECTION)
                {
                    //
                    // an End Collection without a matching Collection is
                    // malformed; do not walk above the root
                    //
                    if (CurrentCollection == ParserContext->RootCollection || CurrentCollection->Root == NULL)
                    {
                        Status = HIDP_STATUS_INTERNAL_ERROR;
                        goto ParseError;
                    }

                    //
                    // use parent of current collection
                    //
                    CurrentCollection = CurrentCollection->Root;
                    CollectionDepth--;
                }
                else
                {
                    ReportType = HID_REPORT_TYPE_ANY;

                    switch (CurrentItem->Tag) {
                        case ITEM_TAG_MAIN_INPUT:
                            ReportType = HID_REPORT_TYPE_INPUT;
                            break;

                        case ITEM_TAG_MAIN_OUTPUT:
                            ReportType = HID_REPORT_TYPE_OUTPUT;
                            break;

                        case ITEM_TAG_MAIN_FEATURE:
                            ReportType = HID_REPORT_TYPE_FEATURE;
                            break;

                        default:
                            DebugFunction("[HIDPARSE] Unknown ReportType Tag %x Type %x Size %x CurrentItemSize %x\n", CurrentItem->Tag, CurrentItem->Type, CurrentItem->Size, CurrentItemSize);
                            break;
                    }

                    if (ReportType == HID_REPORT_TYPE_ANY)
                        break;

                    //
                    // a main data item outside of any collection is malformed
                    //
                    if (CurrentCollection == ParserContext->RootCollection)
                    {
                        Status = HIDP_STATUS_INTERNAL_ERROR;
                        goto ParseError;
                    }

                    //
                    // bound cumulative report items across the descriptor
                    //
                    if (!NT_SUCCESS(HidParser_SizeAdd(TotalReportItems, ParserContext->GlobalItemState.ReportCount, &TotalReportItems)) ||
                        TotalReportItems > HID_MAX_TOTAL_REPORT_ITEMS)
                    {
                        Status = HIDP_STATUS_INTERNAL_ERROR;
                        goto ParseError;
                    }

                    //
                    // get report
                    //
                    Status = HidParser_GetReport(ParserContext, CurrentCollection, ReportType, ParserContext->GlobalItemState.ReportId, TRUE, &Report);
                    if (Status != HIDP_STATUS_SUCCESS)
                        goto ParseError;

                    // fill in a sensible default if the index isn't set
                    if (!ParserContext->LocalItemState.DesignatorIndexSet) {
                        ParserContext->LocalItemState.DesignatorIndex
                            = ParserContext->LocalItemState.DesignatorMinimum;
                    }

                    if (!ParserContext->LocalItemState.StringIndexSet)
                        ParserContext->LocalItemState.StringIndex = ParserContext->LocalItemState.StringMinimum;

                    //
                    // get main item data
                    //
                    MainItemData = (PMAIN_ITEM_DATA)&Data;

                    //
                    // add states & data to the report
                    //
                    Status = HidParser_AddMainItem(ParserContext, Report, &ParserContext->GlobalItemState, &ParserContext->LocalItemState, MainItemData, CurrentCollection);
                    if (Status != HIDP_STATUS_SUCCESS)
                        goto ParseError;
                }

                //
                // backup stack
                //
                Index = ParserContext->LocalItemState.UsageStackAllocated;
                NewUsageStack = ParserContext->LocalItemState.UsageStack;

                //
                // reset the local item state and clear the usage stack
                //
                ZeroFunction(&ParserContext->LocalItemState, sizeof(LOCAL_ITEM_STATE));

                //
                // restore stack
                //
                ParserContext->LocalItemState.UsageStack = NewUsageStack;
                ParserContext->LocalItemState.UsageStackAllocated = Index;
                break;
            }
            case ITEM_TYPE_GLOBAL:
            {
                switch (CurrentItem->Tag) {
                    case ITEM_TAG_GLOBAL_USAGE_PAGE:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_USAGE_PAGE %x\n", Data);
                        ParserContext->GlobalItemState.UsagePage = Data;
                        break;
                    case ITEM_TAG_GLOBAL_LOGICAL_MINIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_LOGICAL_MINIMUM %x\n", Data);
                        ParserContext->GlobalItemState.LogicalMinimum = Data;
                        break;

                    case ITEM_TAG_GLOBAL_LOGICAL_MAXIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_LOCAL_MAXIMUM %x\n", Data);
                        ParserContext->GlobalItemState.LogicialMaximum = Data;
                        break;

                    case ITEM_TAG_GLOBAL_PHYSICAL_MINIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_PHYSICAL_MINIMUM %x\n", Data);
                        ParserContext->GlobalItemState.PhysicalMinimum = Data;
                        break;

                    case ITEM_TAG_GLOBAL_PHYSICAL_MAXIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_PHYSICAL_MAXIMUM %x\n", Data);
                        ParserContext->GlobalItemState.PhysicalMaximum = Data;
                        break;

                    case ITEM_TAG_GLOBAL_UNIT_EXPONENT:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_UNIT_EXPONENT %x\n", Data);
                        ParserContext->GlobalItemState.UnitExponent = Data;
                        break;

                    case ITEM_TAG_GLOBAL_UNIT:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_UNIT %x\n", Data);
                        ParserContext->GlobalItemState.Unit = Data;
                        break;

                    case ITEM_TAG_GLOBAL_REPORT_SIZE:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_SIZE %x\n", Data);
                        //
                        // a field may legitimately be wider than 32 bits (some
                        // vendor reports use 64-bit fields), so the size is kept
                        // as-is and bounded only by the checked bit/byte
                        // arithmetic downstream. Extraction through the HidP
                        // value APIs refuses fields wider than 32 bits; the
                        // field mask is clamped for sizes >= 32.
                        //
                        ParserContext->GlobalItemState.ReportSize = Data;
                        break;

                    case ITEM_TAG_GLOBAL_REPORT_ID:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_ID %x\n", Data);
                        ParserContext->GlobalItemState.ReportId = Data;
                        ParserContext->UseReportIDs = TRUE;
                        break;

                    case ITEM_TAG_GLOBAL_REPORT_COUNT:
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_REPORT_COUNT %x\n", Data);
                        //
                        // bound the field count: this drives the per-report
                        // item array allocation and the add loop
                        //
                        if (Data > HID_MAX_REPORT_COUNT)
                        {
                            Status = HIDP_STATUS_INTERNAL_ERROR;
                            goto ParseError;
                        }
                        ParserContext->GlobalItemState.ReportCount = Data;
                        break;

                    case ITEM_TAG_GLOBAL_PUSH:
                    {
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_PUSH\n");

                        //
                        // bound the push stack: each Push allocates and links
                        // a saved global state, so an unbounded run would
                        // exhaust pool
                        //
                        if (GlobalStackDepth >= HID_MAX_GLOBAL_STACK_DEPTH)
                        {
                            Status = HIDP_STATUS_INTERNAL_ERROR;
                            goto ParseError;
                        }

                        //
                        // allocate global item state
                        //
                        LinkedGlobalItemState = (PGLOBAL_ITEM_STATE)AllocFunction(sizeof(GLOBAL_ITEM_STATE));
                        if (LinkedGlobalItemState == NULL)
                        {
                            Status = HIDP_STATUS_INTERNAL_ERROR;
                            goto ParseError;
                        }

                        //
                        // copy global item state
                        //
                        CopyFunction(LinkedGlobalItemState, &ParserContext->GlobalItemState, sizeof(GLOBAL_ITEM_STATE));

                        //
                        // store pushed item in link member
                        //
                        ParserContext->GlobalItemState.Next = (struct __GLOBAL_ITEM_STATE__*)LinkedGlobalItemState;
                        GlobalStackDepth++;
                        break;
                    }
                    case ITEM_TAG_GLOBAL_POP:
                    {
                        DebugFunction("[HIDPARSE] ITEM_TAG_GLOBAL_POP\n");
                        if (ParserContext->GlobalItemState.Next == NULL)
                        {
                            //
                            // pop without a matching push is malformed
                            //
                            Status = HIDP_STATUS_INTERNAL_ERROR;
                            goto ParseError;
                        }

                        //
                        // get link
                        //
                        LinkedGlobalItemState = (PGLOBAL_ITEM_STATE)ParserContext->GlobalItemState.Next;

                        //
                        // replace current item with linked one
                        //
                        CopyFunction(&ParserContext->GlobalItemState, LinkedGlobalItemState, sizeof(GLOBAL_ITEM_STATE));

                        //
                        // free item
                        //
                        FreeFunction(LinkedGlobalItemState);
                        GlobalStackDepth--;
                        break;
                    }

                    default:
                        //
                        // unknown / unsupported tag: ignore
                        //
                        break;
                }

                break;
            }
            case ITEM_TYPE_LOCAL:
            {
                switch (CurrentItem->Tag)
                {
                    case ITEM_TAG_LOCAL_USAGE:
                    {
                        if (ParserContext->LocalItemState.UsageStackUsed >= ParserContext->LocalItemState.UsageStackAllocated)
                        {
                            ULONG NewAllocated, NewStackBytes, OldStackBytes;

                            //
                            // bound and grow the usage stack with checked
                            // arithmetic; the old contents are preserved
                            //
                            if (ParserContext->LocalItemState.UsageStackAllocated >= HID_MAX_USAGE_STACK)
                            {
                                Status = HIDP_STATUS_INTERNAL_ERROR;
                                goto ParseError;
                            }

                            if (!NT_SUCCESS(HidParser_SizeAdd(ParserContext->LocalItemState.UsageStackAllocated, 10, &NewAllocated)) ||
                                !NT_SUCCESS(HidParser_SizeMult(NewAllocated, sizeof(USAGE_VALUE), &NewStackBytes)) ||
                                !NT_SUCCESS(HidParser_SizeMult(ParserContext->LocalItemState.UsageStackAllocated, sizeof(USAGE_VALUE), &OldStackBytes)))
                            {
                                Status = HIDP_STATUS_INTERNAL_ERROR;
                                goto ParseError;
                            }

                            //
                            // build new usage stack
                            //
                            NewUsageStack = (PUSAGE_VALUE)AllocFunction(NewStackBytes);
                            if (NewUsageStack == NULL)
                            {
                                Status = HIDP_STATUS_INTERNAL_ERROR;
                                goto ParseError;
                            }

                            //
                            // copy old usage stack
                            //
                            CopyFunction(NewUsageStack, ParserContext->LocalItemState.UsageStack, OldStackBytes);

                            //
                            // free old usage stack
                            //
                            FreeFunction(ParserContext->LocalItemState.UsageStack);

                            //
                            // replace with new usage stack
                            //
                            ParserContext->LocalItemState.UsageStack = NewUsageStack;
                            ParserContext->LocalItemState.UsageStackAllocated = NewAllocated;
                        }

                        //
                        // get fresh usage value
                        //
                        UsageValue = &ParserContext->LocalItemState.UsageStack[ParserContext->LocalItemState.UsageStackUsed];

                        //
                        // init usage stack
                        //
                        UsageValue->IsExtended = CurrentItemSize == sizeof(ULONG);
                        UsageValue->u.Extended = Data;

                        //
                        // increment usage stack usage count
                        //
                        ParserContext->LocalItemState.UsageStackUsed++;
                        break;
                    }

                    case ITEM_TAG_LOCAL_USAGE_MINIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_USAGE_MINIMUM Data %x\n", Data);
                        ParserContext->LocalItemState.UsageMinimum.u.Extended = Data;
                        ParserContext->LocalItemState.UsageMinimum.IsExtended
                            = CurrentItemSize == sizeof(ULONG);
                        ParserContext->LocalItemState.UsageMinimumSet = TRUE;
                        break;

                    case ITEM_TAG_LOCAL_USAGE_MAXIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_USAGE_MAXIMUM Data %x ItemSize %x %x\n", Data, CurrentItemSize, CurrentItem->Size);
                        ParserContext->LocalItemState.UsageMaximum.u.Extended = Data;
                        ParserContext->LocalItemState.UsageMaximum.IsExtended
                            = CurrentItemSize == sizeof(ULONG);
                        ParserContext->LocalItemState.UsageMaximumSet = TRUE;
                        break;

                    case ITEM_TAG_LOCAL_DESIGNATOR_INDEX:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_DESIGNATOR_INDEX Data %x\n", Data);
                        ParserContext->LocalItemState.DesignatorIndex = Data;
                        ParserContext->LocalItemState.DesignatorIndexSet = TRUE;
                        break;

                    case ITEM_TAG_LOCAL_DESIGNATOR_MINIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_DESIGNATOR_MINIMUM Data %x\n", Data);
                        ParserContext->LocalItemState.DesignatorMinimum = Data;
                        break;

                    case ITEM_TAG_LOCAL_DESIGNATOR_MAXIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_DESIGNATOR_MAXIMUM Data %x\n", Data);
                        ParserContext->LocalItemState.DesignatorMaximum = Data;
                        break;

                    case ITEM_TAG_LOCAL_STRING_INDEX:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_STRING_INDEX Data %x\n", Data);
                        ParserContext->LocalItemState.StringIndex = Data;
                        ParserContext->LocalItemState.StringIndexSet = TRUE;
                        break;

                    case ITEM_TAG_LOCAL_STRING_MINIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_STRING_MINIMUM Data %x\n", Data);
                        ParserContext->LocalItemState.StringMinimum = Data;
                        break;

                    case ITEM_TAG_LOCAL_STRING_MAXIMUM:
                        DebugFunction("[HIDPARSE] ITEM_TAG_LOCAL_STRING_MAXIMUM Data %x\n", Data);
                        ParserContext->LocalItemState.StringMaximum = Data;
                        break;

                    default:
                        DebugFunction("Unknown Local Item Tag %x\n", CurrentItem->Tag);
                        break;
                }
                break;
            }

            case ITEM_TYPE_LONG:
            {
                CurrentLongItem = (PLONG_ITEM)CurrentItem;
                DebugFunction("Unsupported ITEM_TYPE_LONG Tag %x\n", CurrentLongItem->LongItemTag);
                break;
            }
        }

        //
        // move to next item
        //
        CurrentOffset += CurrentItemSize + sizeof(ITEM_PREFIX);

    }while (CurrentOffset < ReportEnd);


    //
    // cleanup global stack
    //
    LinkedGlobalItemState = (PGLOBAL_ITEM_STATE)ParserContext->GlobalItemState.Next;
    while(LinkedGlobalItemState != NULL)
    {
        DebugFunction("[HIDPARSE] Freeing GlobalState %p\n", LinkedGlobalItemState);
        //
        // free global item state
        //
        NextLinkedGlobalItemState = (PGLOBAL_ITEM_STATE)LinkedGlobalItemState->Next;

        //
        // free state
        //
        FreeFunction(LinkedGlobalItemState);

        //
        // move to next global state
        //
        LinkedGlobalItemState = NextLinkedGlobalItemState;
    }

    //
    // the push chain is now empty; clear the head so a later teardown of the
    // context does not walk freed states
    //
    ParserContext->GlobalItemState.Next = NULL;

    //
    // free usage stack
    //
    FreeFunction(ParserContext->LocalItemState.UsageStack);
    ParserContext->LocalItemState.UsageStack = NULL;

    //
    // every opened collection must have been closed by an End Collection
    //
    if (CollectionDepth != 0 || CurrentCollection != ParserContext->RootCollection)
    {
        Status = HIDP_STATUS_INTERNAL_ERROR;
        goto ParseErrorNoUsageStack;
    }

    //
    // store result
    //
    *OutParser = ParserContext;

    //
    // done
    //
    return HIDP_STATUS_SUCCESS;

ParseError:
    //
    // free the usage stack here; the teardown frees everything else
    //
    FreeFunction(ParserContext->LocalItemState.UsageStack);
    ParserContext->LocalItemState.UsageStack = NULL;

ParseErrorNoUsageStack:
    //
    // tear down the whole scratch tree and the context, then report failure
    //
    HidParser_FreeContext(ParserContext);
    return Status;
}

PHID_COLLECTION
HidParser_GetCollection(
    PHID_PARSER_CONTEXT ParserContext,
    IN ULONG CollectionNumber)
{
    //
    // sanity checks
    //
    ASSERT(ParserContext);
    ASSERT(ParserContext->RootCollection);
    ASSERT(ParserContext->RootCollection->NodeCount);

    //
    // is collection index out of bounds
    //
    if (CollectionNumber < ParserContext->RootCollection->NodeCount)
    {
        //
        // valid collection
        //
        return ParserContext->RootCollection->Nodes[CollectionNumber];
    }

    //
    // no such collection
    //
    DebugFunction("HIDPARSE] No such collection %lu\n", CollectionNumber);
    return NULL;
}


ULONG
HidParser_NumberOfTopCollections(
    IN PVOID ParserCtx)
{
    PHID_PARSER_CONTEXT ParserContext;

    //
    // get parser context
    //
    ParserContext = (PHID_PARSER_CONTEXT)ParserCtx;

    //
    // sanity checks
    //
    ASSERT(ParserContext);
    ASSERT(ParserContext->RootCollection);
    ASSERT(ParserContext->RootCollection->NodeCount);

    //
    // number of top collections
    //
    return ParserContext->RootCollection->NodeCount;
}

NTSTATUS
HidParser_BuildContext(
    IN PVOID ParserContext,
    IN ULONG CollectionIndex,
    IN ULONG ContextSize,
    OUT PVOID *CollectionContext)
{
    PHID_COLLECTION Collection;
    PVOID Context;
    NTSTATUS Status;

    //
    // lets get the collection
    //
    Collection = HidParser_GetCollection((PHID_PARSER_CONTEXT)ParserContext, CollectionIndex);
    ASSERT(Collection);

    //
    // lets allocate the context
    //
    Context = AllocFunction(ContextSize);
    if (Context == NULL)
    {
        //
        // no memory
        //
        return HIDP_STATUS_INTERNAL_ERROR;
    }

    //
    // lets build the context
    //
    Status = HidParser_BuildCollectionContext(Collection, Context, ContextSize);
    if (Status == HIDP_STATUS_SUCCESS)
    {
        //
        // store context
        //
        *CollectionContext = Context;
    }

    //
    // done
    //
    return Status;
}


ULONG
HidParser_GetContextSize(
    IN PVOID ParserContext,
    IN ULONG CollectionIndex)
{
    PHID_COLLECTION Collection;
    ULONG Size;

    //
    // lets get the collection
    //
    Collection = HidParser_GetCollection((PHID_PARSER_CONTEXT)ParserContext, CollectionIndex);

    //
    // calculate size
    //
    Size = HidParser_CalculateContextSize(Collection);
    return Size;
}

