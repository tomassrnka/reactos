/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Editing a partition layout in memory (no system calls, shared with the apitest)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The rules (one mebibyte of alignment, the unused first and last mebibyte of an MBR disk, the boot record
 * sector of every logical drive taking the mebibyte in front of it, the fourth volume becoming an extended
 * partition, the reserved partition of a GPT disk, the type bytes after a format) are the ones measured on
 * Windows Server 2008 R2 with diskpart, which drives the same volume manager as Disk Management.
 */

#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <winioctl.h>
#include <strsafe.h>

#include "disklayout.h"

const GUID DlGuidBasicData = { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } };
const GUID DlGuidReserved  = { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };
const GUID DlGuidEfi       = { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };

static BOOL
DlIsContainerType(UCHAR Type)
{
    /* the partition manager takes only these two as extended partitions; a 0x85 entry is an ordinary partition to it */
    return Type == 0x05 || Type == 0x0F;
}

static BOOL
DlIsZeroGuid(const GUID *Guid)
{
    static const GUID Zero;

    return memcmp(Guid, &Zero, sizeof(GUID)) == 0;
}

ULONGLONG
DlAlignUp(ULONGLONG Value)
{
    return (Value + DL_ALIGN - 1) / DL_ALIGN * DL_ALIGN;
}

ULONGLONG
DlAlignDown(ULONGLONG Value)
{
    return Value / DL_ALIGN * DL_ALIGN;
}

ULONGLONG
DlReservedSize(ULONGLONG DiskLength)
{
    return DiskLength >= DL_MSR_LARGE_DISK ? DL_MSR_LARGE : DL_MSR_SMALL;
}

/* ------------------------------------------------------------------ conversion */

VOID
DlInit(PDL_LAYOUT Layout, DM_STYLE Style, ULONGLONG DiskLength, ULONG SectorSize, DWORD Signature, const GUID *DiskId)
{
    ZeroMemory(Layout, sizeof(*Layout));
    Layout->Style = Style;
    Layout->DiskLength = DiskLength;
    Layout->SectorSize = SectorSize ? SectorSize : 512;
    Layout->Signature = Signature;
    if (DiskId)
        Layout->DiskId = *DiskId;
    if (Style == DM_STYLE_GPT)
    {
        ULONGLONG Table = 128 * 128;    /* 128 entries of 128 bytes */
        ULONGLONG Sector = Layout->SectorSize;

        Layout->MaxGptEntries = 128;
        /* protective MBR, header, then the entry table; the backup mirrors the table and the header at the end */
        Layout->UsableStart = (2 + (Table + Sector - 1) / Sector) * Sector;
        Layout->UsableEnd = DiskLength - (1 + (Table + Sector - 1) / Sector) * Sector;
    }
}

BOOL
DlFromDriver(const DRIVE_LAYOUT_INFORMATION_EX *Driver, ULONGLONG DiskLength, ULONG SectorSize, PDL_LAYOUT Layout)
{
    ULONG i;

    if (Driver->PartitionStyle != PARTITION_STYLE_MBR && Driver->PartitionStyle != PARTITION_STYLE_GPT)
        return FALSE;

    DlInit(Layout, Driver->PartitionStyle == PARTITION_STYLE_MBR ? DM_STYLE_MBR : DM_STYLE_GPT, DiskLength, SectorSize,
           Driver->PartitionStyle == PARTITION_STYLE_MBR ? Driver->Mbr.Signature : 0,
           Driver->PartitionStyle == PARTITION_STYLE_GPT ? &Driver->Gpt.DiskId : NULL);
    if (Layout->Style == DM_STYLE_GPT)
    {
        Layout->UsableStart = (ULONGLONG)Driver->Gpt.StartingUsableOffset.QuadPart;
        Layout->UsableEnd = Layout->UsableStart + (ULONGLONG)Driver->Gpt.UsableLength.QuadPart;
        Layout->MaxGptEntries = Driver->Gpt.MaxPartitionCount ? Driver->Gpt.MaxPartitionCount : 128;
    }

    for (i = 0; i < Driver->PartitionCount; i++)
    {
        const PARTITION_INFORMATION_EX *In = &Driver->PartitionEntry[i];
        PDL_ENTRY Out;

        if (In->PartitionLength.QuadPart == 0)
            continue;
        if (Layout->Style == DM_STYLE_MBR)
        {
            BOOL Container = DlIsContainerType(In->Mbr.PartitionType);

            if (In->Mbr.PartitionType == 0)
                continue;
            if (i >= DL_MBR_PRIMARY_SLOTS && Container)
                continue;   /* the link to the next boot record of the chain */
            if (Layout->Count >= DL_MAX_ENTRIES)
                return FALSE;
            Out = &Layout->Entry[Layout->Count++];
            ZeroMemory(Out, sizeof(*Out));
            Out->Container = Container;
            Out->Logical = (i >= DL_MBR_PRIMARY_SLOTS);
            Out->Slot = (i < DL_MBR_PRIMARY_SLOTS) ? i : 0;
            Out->MbrType = In->Mbr.PartitionType;
            Out->Active = In->Mbr.BootIndicator ? TRUE : FALSE;
        }
        else
        {
            if (DlIsZeroGuid(&In->Gpt.PartitionType))
                continue;
            if (Layout->Count >= DL_MAX_ENTRIES)
                return FALSE;
            Out = &Layout->Entry[Layout->Count++];
            ZeroMemory(Out, sizeof(*Out));
            Out->GptType = In->Gpt.PartitionType;
            Out->GptId = In->Gpt.PartitionId;
            Out->GptAttributes = In->Gpt.Attributes;
            memcpy(Out->GptName, In->Gpt.Name, sizeof(Out->GptName));
        }
        Out->Offset = (ULONGLONG)In->StartingOffset.QuadPart;
        Out->Length = (ULONGLONG)In->PartitionLength.QuadPart;
        Out->Number = In->PartitionNumber;
    }
    return TRUE;
}

static int
DlCompareEntries(const void *a, const void *b)
{
    const DL_ENTRY *x = *(const DL_ENTRY * const *)a, *y = *(const DL_ENTRY * const *)b;

    return (x->Offset > y->Offset) - (x->Offset < y->Offset);
}

static VOID
DlFillMbr(PPARTITION_INFORMATION_EX Out, const DL_ENTRY *In, ULONG SectorSize, ULONGLONG HiddenFrom)
{
    Out->PartitionStyle = PARTITION_STYLE_MBR;
    Out->StartingOffset.QuadPart = (LONGLONG)In->Offset;
    Out->PartitionLength.QuadPart = (LONGLONG)In->Length;
    Out->PartitionNumber = In->Number;
    Out->RewritePartition = TRUE;
    Out->Mbr.PartitionType = In->MbrType;
    Out->Mbr.BootIndicator = In->Active ? TRUE : FALSE;
    Out->Mbr.RecognizedPartition = In->Container ? FALSE : TRUE;
    Out->Mbr.HiddenSectors = (DWORD)((In->Offset - HiddenFrom) / SectorSize);
}

/* An MBR entry holds its start and its length as 32-bit counts of sectors. */
BOOL
DlIsEncodable(const DL_LAYOUT *Layout)
{
    ULONG i, SectorSize = Layout->SectorSize ? Layout->SectorSize : 512;

    if (Layout->Style != DM_STYLE_MBR)
        return TRUE;
    for (i = 0; i < Layout->Count; i++)
    {
        const DL_ENTRY *Entry = &Layout->Entry[i];

        /* the last sector it uses must have a 32-bit number */
        if (Entry->Offset / SectorSize > 0xFFFFFFFFULL || Entry->Length / SectorSize > 0xFFFFFFFFULL ||
            (Entry->Length > 0 && (Entry->Offset + Entry->Length - 1) / SectorSize > 0xFFFFFFFFULL))
            return FALSE;
    }
    return TRUE;
}

PDRIVE_LAYOUT_INFORMATION_EX
DlToDriver(const DL_LAYOUT *Layout, PULONG Size)
{
    PDRIVE_LAYOUT_INFORMATION_EX Out;
    const DL_ENTRY *Sorted[DL_MAX_ENTRIES];
    const DL_ENTRY *Logical[DL_MAX_ENTRIES];
    const DL_ENTRY *Ext = NULL;
    ULONG i, Total, LogicalCount = 0, SortedCount = 0;
    SIZE_T Bytes;

    if (Layout->Style == DM_STYLE_MBR)
    {
        for (i = 0; i < Layout->Count; i++)
        {
            if (Layout->Entry[i].Container)
                Ext = &Layout->Entry[i];
            if (Layout->Entry[i].Logical)
                Logical[LogicalCount++] = &Layout->Entry[i];
        }
        qsort(Logical, LogicalCount, sizeof(Logical[0]), DlCompareEntries);
        /* an extended partition without drives still gets one empty boot record, so that the drive it held is gone */
        Total = DL_MBR_PRIMARY_SLOTS * (1 + (LogicalCount ? LogicalCount : (Ext ? 1 : 0)));
    }
    else
    {
        for (i = 0; i < Layout->Count; i++)
            Sorted[SortedCount++] = &Layout->Entry[i];
        qsort(Sorted, SortedCount, sizeof(Sorted[0]), DlCompareEntries);
        /* a spare, empty entry so that deleting the last partition still rewrites the table; a full table has no room for one */
        Total = SortedCount + (SortedCount < (Layout->MaxGptEntries ? Layout->MaxGptEntries : 128) ? 1 : 0);
    }

    Bytes = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + (SIZE_T)Total * sizeof(PARTITION_INFORMATION_EX);
    Out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Bytes);
    if (Out == NULL)
        return NULL;
    Out->PartitionCount = Total;
    if (Size)
        *Size = (ULONG)Bytes;

    if (Layout->Style == DM_STYLE_MBR)
    {
        ULONGLONG PrevEnd;

        Out->PartitionStyle = PARTITION_STYLE_MBR;
        Out->Mbr.Signature = Layout->Signature;
        for (i = 0; i < DL_MBR_PRIMARY_SLOTS; i++)
        {
            Out->PartitionEntry[i].PartitionStyle = PARTITION_STYLE_MBR;
            Out->PartitionEntry[i].RewritePartition = TRUE;
        }
        for (i = 0; i < Layout->Count; i++)
        {
            const DL_ENTRY *In = &Layout->Entry[i];

            if (!In->Logical)
                DlFillMbr(&Out->PartitionEntry[In->Slot & 3], In, Layout->SectorSize, 0);
        }
        if (Ext && LogicalCount == 0)
        {
            PPARTITION_INFORMATION_EX Empty = &Out->PartitionEntry[DL_MBR_PRIMARY_SLOTS];
            ULONG s;

            for (s = 0; s < DL_MBR_PRIMARY_SLOTS; s++)
            {
                Empty[s].PartitionStyle = PARTITION_STYLE_MBR;
                Empty[s].RewritePartition = TRUE;
            }
        }
        PrevEnd = Ext ? Ext->Offset : 0;
        for (i = 0; i < LogicalCount; i++)
        {
            const DL_ENTRY *In = Logical[i];
            PPARTITION_INFORMATION_EX Data = &Out->PartitionEntry[DL_MBR_PRIMARY_SLOTS * (i + 1)];
            ULONGLONG Gap = In->Offset > PrevEnd ? In->Offset - PrevEnd : 0;
            ULONGLONG Ebr = (i == 0 && Ext) ? Ext->Offset : In->Offset - min(Gap, DL_ALIGN);
            ULONG s;

            DlFillMbr(Data, In, Layout->SectorSize, Ebr);
            for (s = 1; s < DL_MBR_PRIMARY_SLOTS; s++)
            {
                Data[s].PartitionStyle = PARTITION_STYLE_MBR;
                Data[s].RewritePartition = TRUE;
            }
            if (i + 1 < LogicalCount)
            {
                const DL_ENTRY *Next = Logical[i + 1];
                ULONGLONG NextGap = Next->Offset - (In->Offset + In->Length);
                ULONGLONG NextEbr = Next->Offset - min(NextGap, DL_ALIGN);
                PPARTITION_INFORMATION_EX Link = &Data[1];

                Link->StartingOffset.QuadPart = (LONGLONG)NextEbr;
                Link->PartitionLength.QuadPart = (LONGLONG)(Next->Offset + Next->Length - NextEbr);
                Link->Mbr.PartitionType = Ext ? Ext->MbrType : DL_MBR_TYPE_EXTENDED;
                Link->Mbr.HiddenSectors = Ext ? (DWORD)((NextEbr - Ext->Offset) / Layout->SectorSize) : 0;
            }
            PrevEnd = In->Offset + In->Length;
        }
    }
    else
    {
        Out->PartitionStyle = PARTITION_STYLE_GPT;
        Out->Gpt.DiskId = Layout->DiskId;
        Out->Gpt.StartingUsableOffset.QuadPart = (LONGLONG)Layout->UsableStart;
        Out->Gpt.UsableLength.QuadPart = (LONGLONG)(Layout->UsableEnd - Layout->UsableStart);
        Out->Gpt.MaxPartitionCount = Layout->MaxGptEntries;
        for (i = 0; i < SortedCount; i++)
        {
            const DL_ENTRY *In = Sorted[i];
            PPARTITION_INFORMATION_EX Dest = &Out->PartitionEntry[i];

            Dest->PartitionStyle = PARTITION_STYLE_GPT;
            Dest->StartingOffset.QuadPart = (LONGLONG)In->Offset;
            Dest->PartitionLength.QuadPart = (LONGLONG)In->Length;
            Dest->PartitionNumber = In->Number;
            Dest->RewritePartition = TRUE;
            Dest->Gpt.PartitionType = In->GptType;
            Dest->Gpt.PartitionId = In->GptId;
            Dest->Gpt.Attributes = In->GptAttributes;
            memcpy(Dest->Gpt.Name, In->GptName, sizeof(Dest->Gpt.Name));
        }
        if (Total > SortedCount)
        {
            Out->PartitionEntry[SortedCount].PartitionStyle = PARTITION_STYLE_GPT;
            Out->PartitionEntry[SortedCount].RewritePartition = TRUE;
        }
    }
    return Out;
}

/* --------------------------------------------------------------------- queries */

PDL_ENTRY
DlFindEntry(PDL_LAYOUT Layout, ULONGLONG Offset)
{
    ULONG i;

    for (i = 0; i < Layout->Count; i++)
        if (Layout->Entry[i].Offset == Offset)
            return &Layout->Entry[i];
    return NULL;
}

PDL_ENTRY
DlFindExtended(PDL_LAYOUT Layout)
{
    ULONG i;

    for (i = 0; i < Layout->Count; i++)
        if (Layout->Entry[i].Container)
            return &Layout->Entry[i];
    return NULL;
}

ULONG
DlCountPrimaries(const DL_LAYOUT *Layout)
{
    ULONG i, Count = 0;

    for (i = 0; i < Layout->Count; i++)
        if (!Layout->Entry[i].Logical)
            Count++;
    return Count;
}

VOID
DlUsableRange(const DL_LAYOUT *Layout, PULONGLONG Start, PULONGLONG End)
{
    ULONGLONG From, To;

    if (Layout->Style == DM_STYLE_GPT)
    {
        From = DlAlignUp(Layout->UsableStart);
        To = DlAlignDown(Layout->UsableEnd);
    }
    else
    {
        /* the first and the last mebibyte of an MBR disk are never used */
        ULONGLONG Limit = DlAlignDown(0xFFFFFFFFULL * (Layout->SectorSize ? Layout->SectorSize : 512));

        From = DL_ALIGN;
        To = DlAlignDown(Layout->DiskLength);
        To = To > DL_ALIGN ? To - DL_ALIGN : 0;
        /* the start and the length of an MBR partition are 32-bit counts of sectors */
        if (To > Limit)
            To = Limit;
    }
    if (To < From)
        To = From;
    *Start = From;
    *End = To;
}

static VOID
DlAddFree(PDL_FREE Out, ULONG *Count, ULONG Max, BOOL InExtended, ULONGLONG From, ULONGLONG To,
          ULONGLONG DataFrom, ULONGLONG DataTo, ULONGLONG ExactTo)
{
    DL_FREE Item;
    ULONGLONG Whole;

    if (To <= From)
        return;
    Item.InExtended = InExtended;
    Item.Offset = From;
    Item.Length = To - From;
    Item.DataOffset = DlAlignUp(DataFrom);
    DataTo = DlAlignDown(DataTo);
    Item.MaxLength = (DataTo > Item.DataOffset) ? DataTo - Item.DataOffset : 0;
    /* a gap of the top level offers its whole mebibytes, as measured from the exact start, less one; a gap
     * inside the extended partition has lost that mebibyte to the boot record already */
    if (InExtended)
    {
        Item.WizardMax = Item.MaxLength;
    }
    else
    {
        Whole = (ExactTo > From) ? (ExactTo - From) / DL_ALIGN : 0;
        Item.WizardMax = Whole > 0 ? (Whole - 1) * DL_ALIGN : 0;
        if (Item.WizardMax > Item.MaxLength)
            Item.WizardMax = Item.MaxLength;
    }
    /* a gap the disk map does not draw is no gap */
    if (Item.Length <= DM_MIB)
        return;
    if (*Count < Max)
        Out[*Count] = Item;
    (*Count)++;
}

ULONG
DlFindFree(const DL_LAYOUT *Layout, PDL_FREE Out, ULONG Max)
{
    const DL_ENTRY *Top[DL_MAX_ENTRIES], *Inner[DL_MAX_ENTRIES];
    const DL_ENTRY *Ext = NULL;
    ULONG i, TopCount = 0, InnerCount = 0, Count = 0;
    ULONGLONG Start, End, Cursor;

    DlUsableRange(Layout, &Start, &End);
    for (i = 0; i < Layout->Count; i++)
    {
        if (Layout->Entry[i].Container)
            Ext = &Layout->Entry[i];
        if (Layout->Entry[i].Logical)
            Inner[InnerCount++] = &Layout->Entry[i];
        else
            Top[TopCount++] = &Layout->Entry[i];
    }
    qsort(Top, TopCount, sizeof(Top[0]), DlCompareEntries);
    qsort(Inner, InnerCount, sizeof(Inner[0]), DlCompareEntries);

    Cursor = Start;
    for (i = 0; i < TopCount; i++)
    {
        const DL_ENTRY *Entry = Top[i];

        if (Entry->Offset > Cursor)
            DlAddFree(Out, &Count, Max, FALSE, Cursor, Entry->Offset, Cursor, Entry->Offset, Entry->Offset);
        if (Entry->Offset + Entry->Length > Cursor)
            Cursor = Entry->Offset + Entry->Length;
    }
    if (End > Cursor)
        DlAddFree(Out, &Count, Max, FALSE, Cursor, End, Cursor, End,
                  Layout->Style == DM_STYLE_GPT ? Layout->UsableEnd : End);

    if (Ext)
    {
        ULONGLONG ExtEnd = Ext->Offset + Ext->Length;

        Cursor = Ext->Offset;
        for (i = 0; i <= InnerCount; i++)
        {
            ULONGLONG Next = (i < InnerCount) ? Inner[i]->Offset : ExtEnd;

            if (Next > Cursor)
            {
                /* the new drive needs a boot record sector in front, and so does the drive after it */
                ULONGLONG DataFrom = Cursor + DL_ALIGN;
                ULONGLONG DataTo = (i < InnerCount) ? Next - DL_ALIGN : Next;

                DlAddFree(Out, &Count, Max, TRUE, Cursor, Next, DataFrom, DataTo, Next);
            }
            if (i < InnerCount && Inner[i]->Offset + Inner[i]->Length > Cursor)
                Cursor = Inner[i]->Offset + Inner[i]->Length;
        }
    }
    return Count;
}

/* The gap that holds Offset, as it is once the reserved partition of an empty GPT disk is in. */
BOOL
DlFindFreeFor(const DL_LAYOUT *Layout, ULONGLONG Offset, BOOL InExtended, PDL_FREE Result)
{
    DL_LAYOUT Copy;
    DL_FREE Free[DL_MAX_ENTRIES];
    ULONG i, Count;

    memcpy(&Copy, Layout, sizeof(Copy));
    if (DlGptNeedsReserved(&Copy))
        DlCreateReserved(&Copy, NULL);
    Count = DlFindFree(&Copy, Free, DL_MAX_ENTRIES);
    if (Count > DL_MAX_ENTRIES)
        Count = DL_MAX_ENTRIES;
    for (i = 0; i < Count; i++)
    {
        if (Free[i].InExtended != InExtended)
            continue;
        if (Offset >= Free[i].Offset && Offset < Free[i].Offset + Free[i].Length)
        {
            *Result = Free[i];
            return TRUE;
        }
        /* an empty GPT disk: the gap now starts behind the reserved partition */
        if (Copy.Count != Layout->Count && Offset >= Layout->UsableStart && Offset < Free[i].Offset + Free[i].Length)
        {
            *Result = Free[i];
            return TRUE;
        }
    }
    return FALSE;
}

BOOL
DlGptNeedsReserved(const DL_LAYOUT *Layout)
{
    return Layout->Style == DM_STYLE_GPT && Layout->Count == 0;
}

BOOL
DlWantsExtendedForNext(const DL_LAYOUT *Layout, const DL_FREE *Free)
{
    ULONG i, Plain = 0;
    BOOL HaveExt = FALSE;

    if (Layout->Style != DM_STYLE_MBR || Free->InExtended)
        return FALSE;
    for (i = 0; i < Layout->Count; i++)
    {
        if (Layout->Entry[i].Logical)
            continue;
        if (Layout->Entry[i].Container)
            HaveExt = TRUE;
        else
            Plain++;
    }
    return !HaveExt && Plain >= DL_MBR_PRIMARY_SLOTS - 1;
}

/* -------------------------------------------------------------------- editing */

static BOOL
DlRangeIsFree(const DL_LAYOUT *Layout, DL_KIND Kind, ULONGLONG Offset, ULONGLONG Length)
{
    DL_FREE Free[DL_MAX_ENTRIES];
    ULONG i, Count = DlFindFree(Layout, Free, DL_MAX_ENTRIES);

    if (Count > DL_MAX_ENTRIES)
        Count = DL_MAX_ENTRIES;
    for (i = 0; i < Count; i++)
    {
        if (Free[i].InExtended != (Kind == DL_NEW_LOGICAL))
            continue;
        if (Kind == DL_NEW_EXTENDED)
        {
            ULONGLONG End = Free[i].Offset + Free[i].Length;

            /* the extended partition covers its boot record and the drives, so it may use the whole gap */
            if (Offset >= DlAlignUp(Free[i].Offset) && Offset <= DlAlignDown(End) && Length <= DlAlignDown(End) - Offset)
                return TRUE;
            continue;
        }
        if (Offset >= Free[i].DataOffset && Length <= Free[i].MaxLength && Offset - Free[i].DataOffset <= Free[i].MaxLength - Length)
            return TRUE;
    }
    return FALSE;
}

static ULONG
DlFreeSlot(const DL_LAYOUT *Layout)
{
    BOOL Used[DL_MBR_PRIMARY_SLOTS] = { FALSE };
    ULONG i;

    for (i = 0; i < Layout->Count; i++)
        if (!Layout->Entry[i].Logical && Layout->Entry[i].Slot < DL_MBR_PRIMARY_SLOTS)
            Used[Layout->Entry[i].Slot] = TRUE;
    for (i = 0; i < DL_MBR_PRIMARY_SLOTS; i++)
        if (!Used[i])
            return i;
    return DL_MBR_PRIMARY_SLOTS;
}

DL_STATUS
DlCreate(PDL_LAYOUT Layout, DL_KIND Kind, ULONGLONG Offset, ULONGLONG Length, UCHAR MbrType,
         const GUID *GptType, const GUID *GptId, PDL_ENTRY *Created)
{
    PDL_ENTRY Entry;
    ULONG Slot = 0;

    if (Created)
        *Created = NULL;
    if (Length == 0 || (Offset % DL_ALIGN) != 0 || (Length % DL_ALIGN) != 0 || Offset > ~0ULL - Length)
        return DL_E_BAD_ARGUMENT;
    if (Layout->Style == DM_STYLE_GPT && Kind != DL_NEW_PRIMARY)
        return DL_E_WRONG_STYLE;
    if (Layout->Count >= DL_MAX_ENTRIES || (Layout->Style == DM_STYLE_GPT && Layout->Count >= Layout->MaxGptEntries))
        return DL_E_TOO_MANY;

    if (Layout->Style == DM_STYLE_MBR)
    {
        if (Kind == DL_NEW_LOGICAL)
        {
            if (DlFindExtended(Layout) == NULL)
                return DL_E_NO_EXTENDED;
        }
        else
        {
            if (Kind == DL_NEW_EXTENDED && DlFindExtended(Layout) != NULL)
                return DL_E_HAVE_EXTENDED;
            Slot = DlFreeSlot(Layout);
            if (Slot >= DL_MBR_PRIMARY_SLOTS)
                return DL_E_NO_PRIMARY_SLOT;
        }
    }
    if (!DlRangeIsFree(Layout, Kind, Offset, Length))
        return DL_E_NO_SPACE;

    Entry = &Layout->Entry[Layout->Count++];
    ZeroMemory(Entry, sizeof(*Entry));
    Entry->Offset = Offset;
    Entry->Length = Length;
    if (Layout->Style == DM_STYLE_MBR)
    {
        Entry->Logical = (Kind == DL_NEW_LOGICAL);
        Entry->Container = (Kind == DL_NEW_EXTENDED);
        Entry->Slot = Slot;
        Entry->MbrType = Entry->Container ? DL_MBR_TYPE_EXTENDED : (MbrType ? MbrType : DL_MBR_TYPE_NEW);
    }
    else
    {
        Entry->GptType = GptType ? *GptType : DlGuidBasicData;
        if (GptId)
            Entry->GptId = *GptId;
    }
    if (Created)
        *Created = Entry;
    return DL_OK;
}

DL_STATUS
DlCreateReserved(PDL_LAYOUT Layout, const GUID *Id)
{
    PDL_ENTRY Entry;

    if (Layout->Style != DM_STYLE_GPT)
        return DL_E_WRONG_STYLE;
    if (Layout->Count != 0)
        return DL_E_NO_SPACE;
    Entry = &Layout->Entry[Layout->Count++];
    ZeroMemory(Entry, sizeof(*Entry));
    Entry->Offset = Layout->UsableStart;
    Entry->Length = DlReservedSize(Layout->DiskLength);
    Entry->GptType = DlGuidReserved;
    if (Id)
        Entry->GptId = *Id;
    StringCchCopyW(Entry->GptName, ARRAYSIZE(Entry->GptName), L"Microsoft reserved partition");
    return DL_OK;
}

DL_STATUS
DlDelete(PDL_LAYOUT Layout, ULONGLONG Offset)
{
    PDL_ENTRY Entry = DlFindEntry(Layout, Offset);
    ULONG i;

    if (Entry == NULL)
        return DL_E_NOT_FOUND;
    if (Entry->Container)
    {
        for (i = 0; i < Layout->Count; i++)
            if (Layout->Entry[i].Logical)
                return DL_E_EXTENDED_IN_USE;
    }
    i = (ULONG)(Entry - Layout->Entry);
    memmove(&Layout->Entry[i], &Layout->Entry[i + 1], (Layout->Count - i - 1) * sizeof(DL_ENTRY));
    Layout->Count--;
    return DL_OK;
}

DL_STATUS
DlSetActive(PDL_LAYOUT Layout, ULONGLONG Offset, BOOL Active)
{
    PDL_ENTRY Entry = DlFindEntry(Layout, Offset);
    ULONG i;

    if (Layout->Style != DM_STYLE_MBR)
        return DL_E_WRONG_STYLE;
    if (Entry == NULL)
        return DL_E_NOT_FOUND;
    if (Entry->Logical || Entry->Container)
        return DL_E_BAD_ARGUMENT;
    if (Active)
    {
        for (i = 0; i < Layout->Count; i++)
            Layout->Entry[i].Active = FALSE;
    }
    Entry->Active = Active ? TRUE : FALSE;
    return DL_OK;
}

DL_STATUS
DlSetMbrType(PDL_LAYOUT Layout, ULONGLONG Offset, UCHAR Type)
{
    PDL_ENTRY Entry = DlFindEntry(Layout, Offset);

    if (Layout->Style != DM_STYLE_MBR)
        return DL_E_WRONG_STYLE;
    if (Entry == NULL)
        return DL_E_NOT_FOUND;
    if (Entry->Container)
        return DL_E_BAD_ARGUMENT;
    Entry->MbrType = Type;
    return DL_OK;
}

/* The CHS limit of the old BIOS: 1024 cylinders of 255 heads and 63 sectors of 512 bytes. */
#define DL_CHS_LIMIT ((ULONGLONG)1024 * 255 * 63 * 512)

UCHAR
DlMbrTypeForFileSystem(PCWSTR FileSystem, BOOL Fat12, BOOL Fat16, ULONGLONG Offset, ULONGLONG Length, UCHAR Current)
{
    BOOL Beyond = (Offset + Length) > DL_CHS_LIMIT;

    if (FileSystem == NULL)
        return Current;
    if (_wcsicmp(FileSystem, L"NTFS") == 0 || _wcsicmp(FileSystem, L"exFAT") == 0)
        return 0x07;
    if (_wcsicmp(FileSystem, L"FAT32") == 0)
        return Beyond ? 0x0C : 0x0B;
    if (_wcsicmp(FileSystem, L"FAT") == 0)
    {
        if (Fat12)
            return 0x01;
        if (Fat16)
        {
            if (Length < 32 * DM_MIB)
                return 0x04;
            return Beyond ? 0x0E : 0x06;
        }
    }
    /* a file system the table has no byte for: do not leave a FAT type on it */
    switch (Current)
    {
        case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E:
            return 0x07;
    }
    return Current;
}
