/*
 *  FreeLoader NTFS support
 *  Copyright (C) 2004  Filip Navara  <xnavara@volny.cz>
 *  Copyright (C) 2009-2010  Hervé Poussineau
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Limitations:
 * - No support for compressed files.
 * - May crash on corrupted filesystem.
 */

#ifndef _M_ARM
#include <freeldr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(FILESYSTEM);

#define TAG_NTFS_CONTEXT 'CftN'
#define TAG_NTFS_LIST 'LftN'
#define TAG_NTFS_MFT 'MftN'
#define TAG_NTFS_INDEX_REC 'IftN'
#define TAG_NTFS_BITMAP 'BftN'
#define TAG_NTFS_FILE 'FftN'
#define TAG_NTFS_VOLUME 'VftN'
#define TAG_NTFS_DATA 'DftN'

#define NTFS_MAX_ATTRIBUTE_LIST_RECURSION 8

typedef struct _NTFS_VOLUME_INFO
{
    NTFS_BOOTSECTOR BootSector;
    ULONG ClusterSize;
    ULONG MftRecordSize;
    ULONG IndexRecordSize;
    PNTFS_MFT_RECORD MasterFileTable;
    /* FIXME: MFTContext is never freed. */
    PNTFS_ATTR_CONTEXT MFTContext;
    ULONG DeviceId;
    PUCHAR TemporarySector;
    /* Committed transaction of the ntfsng metadata journal, shown over the disk (see NtfsJournalLoad) */
    ULONG JnlCount;
    PULONGLONG JnlBlock;        /* volume offset / 4096 of each page, ascending */
    PULONGLONG JnlSlot;         /* volume offset of its copy in $LogFile */
    PUCHAR JnlMask;             /* valid 512-byte sectors of the copy */
    PUCHAR JnlPage;
    ULONG JnlLoaded;            /* index of the copy in JnlPage, or JnlCount */
} NTFS_VOLUME_INFO;

PNTFS_VOLUME_INFO NtfsVolumes[MAX_FDS];

static ULONGLONG NtfsGetAttributeSize(PNTFS_ATTR_RECORD AttrRecord)
{
    if (AttrRecord->IsNonResident)
        return AttrRecord->NonResident.DataSize;
    else
        return AttrRecord->Resident.ValueLength;
}

static PUCHAR NtfsDecodeRun(PUCHAR DataRun, LONGLONG *DataRunOffset, ULONGLONG *DataRunLength)
{
    UCHAR DataRunOffsetSize;
    UCHAR DataRunLengthSize;
    CHAR i;

    DataRunOffsetSize = (*DataRun >> 4) & 0xF;
    DataRunLengthSize = *DataRun & 0xF;
    *DataRunOffset = 0;
    *DataRunLength = 0;
    DataRun++;
    for (i = 0; i < DataRunLengthSize; i++)
    {
        *DataRunLength += ((ULONG64)*DataRun) << (i * 8);
        DataRun++;
    }

    /* NTFS 3+ sparse files */
    if (DataRunOffsetSize == 0)
    {
        *DataRunOffset = -1;
    }
    else
    {
        for (i = 0; i < DataRunOffsetSize - 1; i++)
        {
            *DataRunOffset += ((ULONG64)*DataRun) << (i * 8);
            DataRun++;
        }
        /* The last byte contains sign so we must process it different way. */
        *DataRunOffset = ((LONG64)(CHAR)(*(DataRun++)) << (i * 8)) + *DataRunOffset;
    }

    TRACE("DataRunOffsetSize: %x\n", DataRunOffsetSize);
    TRACE("DataRunLengthSize: %x\n", DataRunLengthSize);
    TRACE("DataRunOffset: %x\n", *DataRunOffset);
    TRACE("DataRunLength: %x\n", *DataRunLength);

    return DataRun;
}

static PNTFS_ATTR_CONTEXT NtfsPrepareAttributeContext(PNTFS_ATTR_RECORD AttrRecord)
{
    PNTFS_ATTR_CONTEXT Context;

    Context = FrLdrTempAlloc(FIELD_OFFSET(NTFS_ATTR_CONTEXT, Record) + AttrRecord->Length,
                             TAG_NTFS_CONTEXT);
    RtlCopyMemory(&Context->Record, AttrRecord, AttrRecord->Length);
    if (AttrRecord->IsNonResident)
    {
        LONGLONG DataRunOffset;
        ULONGLONG DataRunLength;

        Context->CacheRun = (PUCHAR)&Context->Record + Context->Record.NonResident.MappingPairsOffset;
        Context->CacheRunOffset = 0;
        Context->CacheRun = NtfsDecodeRun(Context->CacheRun, &DataRunOffset, &DataRunLength);
        Context->CacheRunLength = DataRunLength;
        if (DataRunOffset != -1)
        {
            /* Normal run. */
            Context->CacheRunStartLCN =
            Context->CacheRunLastLCN = DataRunOffset;
        }
        else
        {
            /* Sparse run. */
            Context->CacheRunStartLCN = -1;
            Context->CacheRunLastLCN = 0;
        }
        Context->CacheRunCurrentOffset = 0;
    }

    return Context;
}

static VOID NtfsReleaseAttributeContext(PNTFS_ATTR_CONTEXT Context)
{
    FrLdrTempFree(Context, TAG_NTFS_CONTEXT);
}

static BOOLEAN NtfsJournalPatch(PNTFS_VOLUME_INFO Volume, ULONGLONG Offset, ULONGLONG Length, PCHAR Buffer);

static BOOLEAN NtfsDiskRead(PNTFS_VOLUME_INFO Volume, ULONGLONG Offset, ULONGLONG Length, PCHAR Buffer)
{
    LARGE_INTEGER Position;
    ULONG Count;
    ULONG ReadLength;
    ARC_STATUS Status;
    ULONGLONG OrigOffset = Offset, OrigLength = Length;
    PCHAR OrigBuffer = Buffer;

    TRACE("NtfsDiskRead - Offset: %I64u Length: %I64u\n", Offset, Length);

    //
    // I. Read partial first sector if needed
    //
    if (Offset % Volume->BootSector.BytesPerSector)
    {
        Position.QuadPart = Offset & ~(Volume->BootSector.BytesPerSector - 1);
        Status = ArcSeek(Volume->DeviceId, &Position, SeekAbsolute);
        if (Status != ESUCCESS)
            return FALSE;
        Status = ArcRead(Volume->DeviceId, Volume->TemporarySector, Volume->BootSector.BytesPerSector, &Count);
        if (Status != ESUCCESS || Count != Volume->BootSector.BytesPerSector)
            return FALSE;
        ReadLength = (USHORT)min(Length, Volume->BootSector.BytesPerSector - (Offset % Volume->BootSector.BytesPerSector));

        //
        // Copy interesting data
        //
        RtlCopyMemory(Buffer,
                      &Volume->TemporarySector[Offset % Volume->BootSector.BytesPerSector],
                      ReadLength);

        //
        // Move to unfilled buffer part
        //
        Buffer += ReadLength;
        Length -= ReadLength;
        Offset += ReadLength;
    }

    //
    // II. Read all complete blocks
    //
    if (Length >= Volume->BootSector.BytesPerSector)
    {
        Position.QuadPart = Offset;
        Status = ArcSeek(Volume->DeviceId, &Position, SeekAbsolute);
        if (Status != ESUCCESS)
            return FALSE;
        ReadLength = Length & ~(Volume->BootSector.BytesPerSector - 1);
        Status = ArcRead(Volume->DeviceId, Buffer, ReadLength, &Count);
        if (Status != ESUCCESS || Count != ReadLength)
            return FALSE;

        //
        // Move to unfilled buffer part
        //
        Buffer += ReadLength;
        Length -= ReadLength;
        Offset += ReadLength;
    }

    //
    // III. Read the rest of data
    //
    if (Length)
    {
        Position.QuadPart = Offset;
        Status = ArcSeek(Volume->DeviceId, &Position, SeekAbsolute);
        if (Status != ESUCCESS)
            return FALSE;
        Status = ArcRead(Volume->DeviceId, Buffer, (ULONG)Length, &Count);
        if (Status != ESUCCESS || Count != Length)
            return FALSE;
    }

    return NtfsJournalPatch(Volume, OrigOffset, OrigLength, OrigBuffer);
}

static ULONG NtfsReadAttribute(PNTFS_VOLUME_INFO Volume, PNTFS_ATTR_CONTEXT Context, ULONGLONG Offset, PCHAR Buffer, ULONG Length)
{
    ULONGLONG LastLCN;
    PUCHAR DataRun;
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG DataRunStartLCN;
    ULONGLONG CurrentOffset;
    ULONG ReadLength;
    ULONG AlreadyRead;

    if (!Context->Record.IsNonResident)
    {
        if (Offset > Context->Record.Resident.ValueLength)
            return 0;
        if (Offset + Length > Context->Record.Resident.ValueLength)
            Length = (ULONG)(Context->Record.Resident.ValueLength - Offset);
        RtlCopyMemory(Buffer, (PCHAR)&Context->Record + Context->Record.Resident.ValueOffset + Offset, Length);
        return Length;
    }

    /*
     * Non-resident attribute
     */

    /*
     * I. Find the corresponding start data run.
     */

    AlreadyRead = 0;

    // FIXME: Cache seems to be non-working. Disable it for now
    //if(Context->CacheRunOffset <= Offset && Offset < Context->CacheRunOffset + Context->CacheRunLength * Volume->ClusterSize)
    if (0)
    {
        DataRun = Context->CacheRun;
        LastLCN = Context->CacheRunLastLCN;
        DataRunStartLCN = Context->CacheRunStartLCN;
        DataRunLength = Context->CacheRunLength;
        CurrentOffset = Context->CacheRunCurrentOffset;
    }
    else
    {
        LastLCN = 0;
        DataRun = (PUCHAR)&Context->Record + Context->Record.NonResident.MappingPairsOffset;
        CurrentOffset = 0;

        while (1)
        {
            DataRun = NtfsDecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunOffset != -1)
            {
                /* Normal data run. */
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
            {
                /* Sparse data run. */
                DataRunStartLCN = -1;
            }

            if (Offset >= CurrentOffset &&
                Offset < CurrentOffset + (DataRunLength * Volume->ClusterSize))
            {
                break;
            }

            if (*DataRun == 0)
            {
                return AlreadyRead;
            }

            CurrentOffset += DataRunLength * Volume->ClusterSize;
        }
    }

    /*
     * II. Go through the run list and read the data
     */

    ReadLength = (ULONG)min(DataRunLength * Volume->ClusterSize - (Offset - CurrentOffset), Length);
    if (DataRunStartLCN == -1)
        RtlZeroMemory(Buffer, ReadLength);
    if (DataRunStartLCN == -1 || NtfsDiskRead(Volume, DataRunStartLCN * Volume->ClusterSize + Offset - CurrentOffset, ReadLength, Buffer))
    {
        Length -= ReadLength;
        Buffer += ReadLength;
        AlreadyRead += ReadLength;

        if (ReadLength == DataRunLength * Volume->ClusterSize - (Offset - CurrentOffset))
        {
            CurrentOffset += DataRunLength * Volume->ClusterSize;
            DataRun = NtfsDecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunOffset != (ULONGLONG)-1)
            {
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
                DataRunStartLCN = -1;
        }

        while (Length > 0)
        {
            ReadLength = (ULONG)min(DataRunLength * Volume->ClusterSize, Length);
            if (DataRunStartLCN == -1)
                RtlZeroMemory(Buffer, ReadLength);
            else if (!NtfsDiskRead(Volume, DataRunStartLCN * Volume->ClusterSize, ReadLength, Buffer))
                break;

            Length -= ReadLength;
            Buffer += ReadLength;
            AlreadyRead += ReadLength;

            /* We finished this request, but there still data in this data run. */
            if (Length == 0 && ReadLength != DataRunLength * Volume->ClusterSize)
                break;

            /*
             * Go to next run in the list.
             */

            if (*DataRun == 0)
                break;
            CurrentOffset += DataRunLength * Volume->ClusterSize;
            DataRun = NtfsDecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunOffset != -1)
            {
                /* Normal data run. */
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
            {
                /* Sparse data run. */
                DataRunStartLCN = -1;
            }
        } /* while */

    } /* if Disk */

    Context->CacheRun = DataRun;
    Context->CacheRunOffset = Offset + AlreadyRead;
    Context->CacheRunStartLCN = DataRunStartLCN;
    Context->CacheRunLength = DataRunLength;
    Context->CacheRunLastLCN = LastLCN;
    Context->CacheRunCurrentOffset = CurrentOffset;

    return AlreadyRead;
}

static PNTFS_ATTR_CONTEXT NtfsFindAttributeHelper(
    PNTFS_VOLUME_INFO Volume,
    ULONGLONG CurrentMftIndex,
    PNTFS_ATTR_RECORD AttrRecord,
    PNTFS_ATTR_RECORD AttrRecordEnd,
    ULONG Type,
    const WCHAR *Name,
    ULONG NameLength,
    ULONG RecursionLimit,
    ULONG Instance);
static BOOLEAN NtfsReadMftRecord(PNTFS_VOLUME_INFO Volume, ULONGLONG MFTIndex, PNTFS_MFT_RECORD Buffer);

static PNTFS_ATTR_CONTEXT NtfsFindAttributeHelperList(
    PNTFS_VOLUME_INFO Volume,
    ULONGLONG ParentMftIndex,
    PNTFS_ATTR_LIST_ATTR AttrListRecord,
    PNTFS_ATTR_LIST_ATTR AttrListRecordEnd,
    ULONG Type,
    const WCHAR *Name,
    ULONG NameLength,
    ULONG RecursionLimit)
{
    ULONGLONG PrevMftIndex = -1;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PNTFS_MFT_RECORD MftRecord;
    if (RecursionLimit < 1)
        return NULL;

    MftRecord = FrLdrTempAlloc(Volume->MftRecordSize, TAG_NTFS_MFT);
    if (!MftRecord)
        return NULL;

    while (AttrListRecord < AttrListRecordEnd)
    {
        ULONGLONG MftIndex = AttrListRecord->BaseFileRef & NTFS_MFT_MASK;
        ULONG AttrType = AttrListRecord->Type;
        ULONG AttrId = AttrListRecord->AttrId;

        if (AttrType == NTFS_ATTR_TYPE_END)
            break;

        TRACE("RecursionLimit = %u, AttrType = 0x%x, MftIndex = %I64u\n", RecursionLimit, AttrType, MftIndex);

        if (MftIndex == ParentMftIndex)
        {
            TRACE("Skipping unnecessary recursion level!\n");
            goto skip;
        }

        if (AttrType == Type &&
            AttrListRecord->NameLength == NameLength)
        {
            PWCHAR AttrListName;

            AttrListName = (PWCHAR)((PCHAR)AttrListRecord + AttrListRecord->NameOffset);
            if (RtlEqualMemory(AttrListName, Name, NameLength * sizeof(WCHAR)))
            {
                PNTFS_ATTR_RECORD AttrRecord;
                PNTFS_ATTR_RECORD AttrRecordEnd;

                if (PrevMftIndex != MftIndex)
                {
                    PrevMftIndex = MftIndex;
                    if (!NtfsReadMftRecord(Volume, MftIndex, MftRecord))
                        goto skip;
                }

                AttrRecord = (PNTFS_ATTR_RECORD)((PCHAR)MftRecord + MftRecord->AttributesOffset);
                AttrRecordEnd = (PNTFS_ATTR_RECORD)((PCHAR)MftRecord + Volume->MftRecordSize);

                Context = NtfsFindAttributeHelper(Volume, MftIndex,
                                                  AttrRecord, AttrRecordEnd,
                                                  Type, Name, NameLength,
                                                  RecursionLimit - 1, AttrId);
                if (Context)
                    break;
            }
        }

skip:
        if (AttrListRecord->RecLength == 0)
            break;
        AttrListRecord = (PNTFS_ATTR_LIST_ATTR)((PCHAR)AttrListRecord + AttrListRecord->RecLength);
    }

    if (MftRecord)
        FrLdrTempFree(MftRecord, TAG_NTFS_MFT);

    return Context;
}

static PNTFS_ATTR_CONTEXT NtfsFindAttributeHelper(
    PNTFS_VOLUME_INFO Volume,
    ULONGLONG CurrentMftIndex,
    PNTFS_ATTR_RECORD AttrRecord,
    PNTFS_ATTR_RECORD AttrRecordEnd,
    ULONG Type,
    const WCHAR *Name,
    ULONG NameLength,
    ULONG RecursionLimit,
    ULONG Instance)
{
    PNTFS_ATTR_CONTEXT Context = NULL;
    PNTFS_ATTR_CONTEXT ListContext = NULL;
    PVOID ListBuffer = NULL;
    ULONGLONG ListSize = 0;
    if (RecursionLimit < 1)
        return NULL;

    while (AttrRecord < AttrRecordEnd)
    {
        ULONG AttrType = AttrRecord->Type;
        ULONG AttrInstance = AttrRecord->Instance;

        if (AttrType == NTFS_ATTR_TYPE_END)
            break;

        TRACE("RecursionLimit = %u, AttrType = 0x%x\n", RecursionLimit, AttrType);

        if (ListContext)
            NtfsReleaseAttributeContext(ListContext);
        if (ListBuffer)
            FrLdrTempFree(ListBuffer, TAG_NTFS_LIST);

        ListContext = ListBuffer = NULL;

        /* Limit the $ATTRIBUTE_LIST recursion or else infinity loop */
        if (AttrType != Type && AttrType == NTFS_ATTR_TYPE_ATTRIBUTE_LIST && RecursionLimit >= 2)
        {
            PNTFS_ATTR_LIST_ATTR ListAttrRecord;
            PNTFS_ATTR_LIST_ATTR ListAttrRecordEnd;

            ListContext = NtfsPrepareAttributeContext(AttrRecord);

            ListSize = NtfsGetAttributeSize(&ListContext->Record);
            if (ListSize <= 0xFFFFFFFF)
                ListBuffer = FrLdrTempAlloc((ULONG)ListSize, TAG_NTFS_LIST);
            else
                ListBuffer = NULL;

            if (!ListBuffer)
            {
                TRACE("Failed to allocate memory: %x\n", (ULONG)ListSize);
                goto skip;
            }

            ListAttrRecord = (PNTFS_ATTR_LIST_ATTR)ListBuffer;
            ListAttrRecordEnd = (PNTFS_ATTR_LIST_ATTR)((PCHAR)ListBuffer + ListSize);

            if (NtfsReadAttribute(Volume, ListContext, 0, ListBuffer, (ULONG)ListSize) == ListSize)
            {
                Context = NtfsFindAttributeHelperList(Volume, CurrentMftIndex,
                                                      ListAttrRecord, ListAttrRecordEnd,
                                                      Type, Name, NameLength,
                                                      RecursionLimit - 1);

                if (Context != NULL)
                    break;
            }
        }

        if (AttrType == Type &&
            AttrRecord->NameLength == NameLength &&
            /* HACK for ntfs3 driver on linux! Because the ntfs3 likes to generate invalid
             * attribute size when mft record increases and I don't know how I will handle this */
            NtfsGetAttributeSize(AttrRecord) != 0 &&
            (Instance == (ULONG)-1 || AttrInstance == Instance))
        {
            PWCHAR AttrName;

            AttrName = (PWCHAR)((PCHAR)AttrRecord + AttrRecord->NameOffset);
            if (RtlEqualMemory(AttrName, Name, NameLength * sizeof(WCHAR)))
            {
                /* Found it, fill up the context and return */
                Context = NtfsPrepareAttributeContext(AttrRecord);
                break;
            }
        }

skip:
        if (AttrRecord->Length == 0)
            break;
        AttrRecord = (PNTFS_ATTR_RECORD)((PCHAR)AttrRecord + AttrRecord->Length);
    }

    if (ListContext)
        NtfsReleaseAttributeContext(ListContext);
    if (ListBuffer)
        FrLdrTempFree(ListBuffer, TAG_NTFS_LIST);

    return Context;
}

static PNTFS_ATTR_CONTEXT NtfsFindAttribute(PNTFS_VOLUME_INFO Volume, PNTFS_MFT_RECORD MftRecord, ULONGLONG MftIndex, ULONG Type, const WCHAR *Name)
{
    PNTFS_ATTR_RECORD AttrRecord;
    PNTFS_ATTR_RECORD AttrRecordEnd;
    ULONG NameLength;

    AttrRecord = (PNTFS_ATTR_RECORD)((PCHAR)MftRecord + MftRecord->AttributesOffset);
    AttrRecordEnd = (PNTFS_ATTR_RECORD)((PCHAR)MftRecord + Volume->MftRecordSize);
    for (NameLength = 0; Name[NameLength] != 0; NameLength++)
        ;

    return NtfsFindAttributeHelper(Volume, MftIndex, AttrRecord, AttrRecordEnd, Type, Name, NameLength, NTFS_MAX_ATTRIBUTE_LIST_RECURSION, -1);
}

static BOOLEAN NtfsFixupRecord(PNTFS_VOLUME_INFO Volume, PNTFS_RECORD Record)
{
    USHORT *USA;
    USHORT USANumber;
    USHORT USACount;
    USHORT *Block;

    USA = (USHORT*)((PCHAR)Record + Record->USAOffset);
    USANumber = *(USA++);
    USACount = Record->USACount - 1; /* Exclude the USA Number. */
    Block = (USHORT*)((PCHAR)Record + Volume->BootSector.BytesPerSector - 2);

    while (USACount)
    {
        if (*Block != USANumber)
            return FALSE;
        *Block = *(USA++);
        Block = (USHORT*)((PCHAR)Block + Volume->BootSector.BytesPerSector);
        USACount--;
    }

    return TRUE;
}

static BOOLEAN NtfsReadMftRecord(PNTFS_VOLUME_INFO Volume, ULONGLONG MFTIndex, PNTFS_MFT_RECORD Buffer)
{
    ULONGLONG BytesRead;

    BytesRead = NtfsReadAttribute(Volume, Volume->MFTContext, MFTIndex * Volume->MftRecordSize, (PCHAR)Buffer, Volume->MftRecordSize);
    if (BytesRead != Volume->MftRecordSize)
        return FALSE;

    /* Apply update sequence array fixups. */
    return NtfsFixupRecord(Volume, (PNTFS_RECORD)Buffer);
}

#if DBG
VOID NtfsPrintFile(PNTFS_INDEX_ENTRY IndexEntry)
{
    PWCHAR FileName;
    UCHAR FileNameLength;
    CHAR AnsiFileName[256];
    UCHAR i;

    FileName = IndexEntry->FileName.FileName;
    FileNameLength = min(IndexEntry->FileName.FileNameLength, 255);

    for (i = 0; i < FileNameLength; i++)
        AnsiFileName[i] = (CHAR)FileName[i];
    AnsiFileName[i] = 0;

    TRACE("- %s (%x)\n", AnsiFileName, (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK));
}
#endif

static BOOLEAN
NtfsCompareFileName(
    _In_ PCCH FileName,
    _In_ SIZE_T FileNameLen,
    _In_ PNTFS_INDEX_ENTRY IndexEntry)
{
    PWCHAR EntryFileName;
    UCHAR EntryFileNameLength;
    UCHAR i;

    EntryFileName = IndexEntry->FileName.FileName;
    EntryFileNameLength = IndexEntry->FileName.FileNameLength;

#if DBG
    TRACE("%s ", FileName);
    NtfsPrintFile(IndexEntry);
#endif

    if (FileNameLen != EntryFileNameLength)
        return FALSE;

    /*
     * Always perform case-insensitive comparison for file names.
     * This is necessary, because when modifying e.g. on Linux a Windows NTFS
     * partition formatted with Windows itself, the NTLDR/BOOTMGR will boot
     * normally ignoring the case of the paths.
     */
    for (i = 0; i < EntryFileNameLength; i++)
    {
        if (tolower(EntryFileName[i]) != tolower(FileName[i]))
            return FALSE;
    }

    return TRUE;
}

static BOOLEAN
NtfsFindMftRecord(
    _In_ PNTFS_VOLUME_INFO Volume,
    _In_ ULONGLONG MFTIndex,
    _In_ PCSTR FileName,
    _Out_ PULONGLONG OutMFTIndex,
    _Out_ PULONG FileAttributes)
{
    PNTFS_MFT_RECORD MftRecord;
    //ULONG Magic;
    PNTFS_ATTR_CONTEXT IndexRootCtx;
    PNTFS_ATTR_CONTEXT IndexBitmapCtx;
    PNTFS_ATTR_CONTEXT IndexAllocationCtx;
    PNTFS_INDEX_ROOT IndexRoot;
    ULONGLONG BitmapDataSize;
    ULONGLONG IndexAllocationSize;
    PCHAR BitmapData;
    PCHAR IndexRecord;
    PNTFS_INDEX_ENTRY IndexEntry, IndexEntryEnd;
    ULONG RecordOffset;
    ULONG IndexBlockSize;
    SIZE_T FileNameLen;

    FileNameLen = strlen(FileName);

    MftRecord = FrLdrTempAlloc(Volume->MftRecordSize, TAG_NTFS_MFT);
    if (MftRecord == NULL)
    {
        return FALSE;
    }

    if (NtfsReadMftRecord(Volume, MFTIndex, MftRecord))
    {
        //Magic = MftRecord->Magic;

        IndexRootCtx = NtfsFindAttribute(Volume, MftRecord, MFTIndex, NTFS_ATTR_TYPE_INDEX_ROOT, L"$I30");
        if (IndexRootCtx == NULL)
        {
            FrLdrTempFree(MftRecord, TAG_NTFS_MFT);
            return FALSE;
        }

        IndexRecord = FrLdrTempAlloc(Volume->IndexRecordSize, TAG_NTFS_INDEX_REC);
        if (IndexRecord == NULL)
        {
            FrLdrTempFree(MftRecord, TAG_NTFS_MFT);
            return FALSE;
        }

        NtfsReadAttribute(Volume, IndexRootCtx, 0, IndexRecord, Volume->IndexRecordSize);
        IndexRoot = (PNTFS_INDEX_ROOT)IndexRecord;
        IndexEntry = (PNTFS_INDEX_ENTRY)((PCHAR)&IndexRoot->IndexHeader + IndexRoot->IndexHeader.EntriesOffset);
        /* Index root is always resident. */
        IndexEntryEnd = (PNTFS_INDEX_ENTRY)(IndexRecord + IndexRootCtx->Record.Resident.ValueLength);
        NtfsReleaseAttributeContext(IndexRootCtx);

        TRACE("IndexRecordSize: %x IndexBlockSize: %x\n", Volume->IndexRecordSize, IndexRoot->IndexBlockSize);

        while (IndexEntry < IndexEntryEnd &&
               !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            if (NtfsCompareFileName(FileName, FileNameLen, IndexEntry))
            {
                *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
                *FileAttributes = IndexEntry->FileName.FileAttributes;
                FrLdrTempFree(IndexRecord, TAG_NTFS_INDEX_REC);
                FrLdrTempFree(MftRecord, TAG_NTFS_MFT);
                return TRUE;
            }
            IndexEntry = (PNTFS_INDEX_ENTRY)((PCHAR)IndexEntry + IndexEntry->Length);
        }

        if (IndexRoot->IndexHeader.Flags & NTFS_LARGE_INDEX)
        {
            TRACE("Large Index!\n");

            IndexBlockSize = IndexRoot->IndexBlockSize;

            IndexBitmapCtx = NtfsFindAttribute(Volume, MftRecord, MFTIndex, NTFS_ATTR_TYPE_BITMAP, L"$I30");
            if (IndexBitmapCtx == NULL)
            {
                TRACE("Corrupted filesystem!\n");
                FrLdrTempFree(MftRecord, TAG_NTFS_MFT);
                return FALSE;
            }
            BitmapDataSize = NtfsGetAttributeSize(&IndexBitmapCtx->Record);
            TRACE("BitmapDataSize: %x\n", (ULONG)BitmapDataSize);
            if(BitmapDataSize <= 0xFFFFFFFF)
                BitmapData = FrLdrTempAlloc((ULONG)BitmapDataSize, TAG_NTFS_BITMAP);
            else
                BitmapData = NULL;

            if (BitmapData == NULL)
            {
                FrLdrTempFree(IndexRecord, TAG_NTFS_INDEX_REC);
                FrLdrTempFree(MftRecord, TAG_NTFS_MFT);
                return FALSE;
            }
            NtfsReadAttribute(Volume, IndexBitmapCtx, 0, BitmapData, (ULONG)BitmapDataSize);
            NtfsReleaseAttributeContext(IndexBitmapCtx);

            IndexAllocationCtx = NtfsFindAttribute(Volume, MftRecord, MFTIndex, NTFS_ATTR_TYPE_INDEX_ALLOCATION, L"$I30");
            if (IndexAllocationCtx == NULL)
            {
                TRACE("Corrupted filesystem!\n");
                FrLdrTempFree(BitmapData, TAG_NTFS_BITMAP);
                FrLdrTempFree(IndexRecord, TAG_NTFS_INDEX_REC);
                FrLdrTempFree(MftRecord, TAG_NTFS_MFT);
                return FALSE;
            }
            IndexAllocationSize = NtfsGetAttributeSize(&IndexAllocationCtx->Record);

            RecordOffset = 0;

            for (;;)
            {
                TRACE("RecordOffset: %x IndexAllocationSize: %x\n", RecordOffset, IndexAllocationSize);
                for (; RecordOffset < IndexAllocationSize;)
                {
                    UCHAR Bit = 1 << ((RecordOffset / IndexBlockSize) & 7);
                    ULONG Byte = (RecordOffset / IndexBlockSize) >> 3;
                    if ((BitmapData[Byte] & Bit))
                        break;
                    RecordOffset += IndexBlockSize;
                }

                if (RecordOffset >= IndexAllocationSize)
                {
                    break;
                }

                NtfsReadAttribute(Volume, IndexAllocationCtx, RecordOffset, IndexRecord, IndexBlockSize);

                if (!NtfsFixupRecord(Volume, (PNTFS_RECORD)IndexRecord))
                {
                    break;
                }

                /* FIXME */
                IndexEntry = (PNTFS_INDEX_ENTRY)(IndexRecord + 0x18 + *(USHORT *)(IndexRecord + 0x18));
                IndexEntryEnd = (PNTFS_INDEX_ENTRY)(IndexRecord + IndexBlockSize);

                while (IndexEntry < IndexEntryEnd &&
                       !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
                {
                    if (NtfsCompareFileName(FileName, FileNameLen, IndexEntry))
                    {
                        TRACE("File found\n");
                        *OutMFTIndex = (IndexEntry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
                        *FileAttributes = IndexEntry->FileName.FileAttributes;
                        NtfsReleaseAttributeContext(IndexAllocationCtx);
                        FrLdrTempFree(BitmapData, TAG_NTFS_BITMAP);
                        FrLdrTempFree(IndexRecord, TAG_NTFS_INDEX_REC);
                        FrLdrTempFree(MftRecord, TAG_NTFS_MFT);
                        return TRUE;
                    }
                    IndexEntry = (PNTFS_INDEX_ENTRY)((PCHAR)IndexEntry + IndexEntry->Length);
                }

                RecordOffset += IndexBlockSize;
            }

            NtfsReleaseAttributeContext(IndexAllocationCtx);
            FrLdrTempFree(BitmapData, TAG_NTFS_BITMAP);
        }

        FrLdrTempFree(IndexRecord, TAG_NTFS_INDEX_REC);
    }
    else
    {
        TRACE("Can't read MFT record\n");
    }
    FrLdrTempFree(MftRecord, TAG_NTFS_MFT);

    return FALSE;
}

static BOOLEAN NtfsLookupFile(PNTFS_VOLUME_INFO Volume, PCSTR FileName, PNTFS_MFT_RECORD MftRecord, PNTFS_FILE_HANDLE FileHandle)
{
    ULONG NumberOfPathParts;
    ULONG i;
    ULONGLONG CurrentMFTIndex;
    ULONG FileAttributes;
    CHAR PathPart[261];

    TRACE("NtfsLookupFile() FileName = %s\n", FileName);

    CurrentMFTIndex = NTFS_FILE_ROOT;

    /* Skip leading path separator, if any */
    if (*FileName == '\\' || *FileName == '/')
        ++FileName;
    PathPart[0] = ANSI_NULL;

    /* Figure out how many sub-directories we are nested in and loop once for each part */
    NumberOfPathParts = FsGetNumPathParts(FileName);
    for (i = 0; i < NumberOfPathParts; i++)
    {
        FsGetFirstNameFromPath(PathPart, FileName);

        for (; (*FileName != '\\') && (*FileName != '/') && (*FileName != '\0'); FileName++)
            ;
        FileName++;

        TRACE("- Lookup: %s\n", PathPart);
        if (!NtfsFindMftRecord(Volume, CurrentMFTIndex, PathPart, &CurrentMFTIndex, &FileAttributes))
        {
            TRACE("- Failed\n");
            return FALSE;
        }
        TRACE("- Lookup: %x\n", CurrentMFTIndex);
    }

    if (!NtfsReadMftRecord(Volume, CurrentMFTIndex, MftRecord))
    {
        TRACE("NtfsLookupFile: Can't read MFT record\n");
        return FALSE;
    }

    FileHandle->DataContext = NtfsFindAttribute(Volume, MftRecord, CurrentMFTIndex, NTFS_ATTR_TYPE_DATA, L"");
    if (FileHandle->DataContext == NULL)
    {
        TRACE("NtfsLookupFile: Can't find data attribute\n");
        return FALSE;
    }

    /* Map the attributes to ARC file attributes */
    FileHandle->Attributes = 0;
    if (FileAttributes & NTFS_FILE_ATTR_READONLY)
        FileHandle->Attributes |= ReadOnlyFile;
    if (FileAttributes & NTFS_FILE_ATTR_HIDDEN)
        FileHandle->Attributes |= HiddenFile;
    if (FileAttributes & NTFS_FILE_ATTR_SYSTEM)
        FileHandle->Attributes |= SystemFile;
    if (FileAttributes & NTFS_FILE_ATTR_ARCHIVE)
        FileHandle->Attributes |= ArchiveFile;
    if (FileAttributes & NTFS_FILE_ATTR_DIRECTORY)
        FileHandle->Attributes |= DirectoryFile;

    /* Copy the file name, perhaps truncated */
    FileHandle->FileNameLength = (ULONG)strlen(PathPart);
    FileHandle->FileNameLength = min(FileHandle->FileNameLength, sizeof(FileHandle->FileName) - 1);
    RtlCopyMemory(FileHandle->FileName, PathPart, FileHandle->FileNameLength);

    return TRUE;
}

ARC_STATUS NtfsClose(ULONG FileId)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);

    NtfsReleaseAttributeContext(FileHandle->DataContext);
    FrLdrTempFree(FileHandle, TAG_NTFS_FILE);

    return ESUCCESS;
}

ARC_STATUS NtfsGetFileInformation(ULONG FileId, FILEINFORMATION* Information)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);

    RtlZeroMemory(Information, sizeof(*Information));
    Information->EndingAddress.QuadPart = NtfsGetAttributeSize(&FileHandle->DataContext->Record);
    Information->CurrentAddress.QuadPart = FileHandle->Offset;

    /* Set the ARC file attributes */
    Information->Attributes = FileHandle->Attributes;

    /* Copy the file name, perhaps truncated, and NUL-terminated */
    Information->FileNameLength = min(FileHandle->FileNameLength, sizeof(Information->FileName) - 1);
    RtlCopyMemory(Information->FileName, FileHandle->FileName, Information->FileNameLength);
    Information->FileName[Information->FileNameLength] = ANSI_NULL;

    TRACE("NtfsGetFileInformation(%lu) -> FileSize = %llu, FilePointer = 0x%llx\n",
          FileId, Information->EndingAddress.QuadPart, Information->CurrentAddress.QuadPart);

    return ESUCCESS;
}

ARC_STATUS NtfsOpen(CHAR* Path, OPENMODE OpenMode, ULONG* FileId)
{
    PNTFS_VOLUME_INFO Volume;
    PNTFS_FILE_HANDLE FileHandle;
    PNTFS_MFT_RECORD MftRecord;
    ULONG DeviceId;

    //
    // Check parameters
    //
    if (OpenMode != OpenReadOnly)
        return EACCES;

    //
    // Get underlying device
    //
    DeviceId = FsGetDeviceId(*FileId);
    Volume = NtfsVolumes[DeviceId];

    TRACE("NtfsOpen() FileName = %s\n", Path);

    //
    // Allocate file structure
    //
    FileHandle = FrLdrTempAlloc(sizeof(NTFS_FILE_HANDLE) + Volume->MftRecordSize,
                                TAG_NTFS_FILE);
    if (!FileHandle)
    {
        return ENOMEM;
    }
    RtlZeroMemory(FileHandle, sizeof(NTFS_FILE_HANDLE) + Volume->MftRecordSize);
    FileHandle->Volume = Volume;

    //
    // Search file entry
    //
    MftRecord = (PNTFS_MFT_RECORD)(FileHandle + 1);
    if (!NtfsLookupFile(Volume, Path, MftRecord, FileHandle))
    {
        FrLdrTempFree(FileHandle, TAG_NTFS_FILE);
        return ENOENT;
    }

    FsSetDeviceSpecific(*FileId, FileHandle);
    return ESUCCESS;
}

ARC_STATUS NtfsRead(ULONG FileId, VOID* Buffer, ULONG N, ULONG* Count)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);
    ULONGLONG BytesRead64;

    /* Read data */
    BytesRead64 = NtfsReadAttribute(FileHandle->Volume, FileHandle->DataContext, FileHandle->Offset, Buffer, N);
    FileHandle->Offset += BytesRead64;
    *Count = (ULONG)BytesRead64;
    if (BytesRead64 > 0)
        return ESUCCESS;
    else
        return EIO;
}

ARC_STATUS NtfsSeek(ULONG FileId, LARGE_INTEGER* Position, SEEKMODE SeekMode)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);
    LARGE_INTEGER NewPosition = *Position;

    switch (SeekMode)
    {
        case SeekAbsolute:
            break;
        case SeekRelative:
            NewPosition.QuadPart += FileHandle->Offset;
            break;
        default:
            ASSERT(FALSE);
            return EINVAL;
    }

    if (NewPosition.QuadPart >= NtfsGetAttributeSize(&FileHandle->DataContext->Record))
        return EINVAL;

    FileHandle->Offset = NewPosition.QuadPart;
    return ESUCCESS;
}


/**
 * @brief
 * Returns the size of the NTFS volume laid on the storage media device
 * opened via @p DeviceId.
 **/
ULONGLONG
NtfsGetVolumeSize(
    _In_ ULONG DeviceId)
{
    PNTFS_VOLUME_INFO Volume = NtfsVolumes[DeviceId];
    ASSERT(Volume);
    return Volume->BootSector.VolumeSectorCount * Volume->BootSector.BytesPerSector;
}


const DEVVTBL NtfsFuncTable =
{
    NtfsClose,
    NtfsGetFileInformation,
    NtfsOpen,
    NtfsRead,
    NtfsSeek,
    L"ntfs",
};

/*
 * The ntfsng driver keeps a metadata journal of its own inside $LogFile (header in page 3, descriptor
 * and data slots from page 5 on, pages at power-of-two offsets skipped).  A crash during the in-place
 * pass of a committed transaction leaves metadata half written until the driver replays it at mount,
 * and the loader reads the volume before that: a committed transaction is therefore shown over the
 * disk here, read-only, exactly as the driver's read-only mount does.
 */
#define NTFSNG_PAGE 4096
#define NTFSNG_HDR_PAGE 3
#define NTFSNG_FIRST_SLOT 5
#define NTFSNG_DESC_PER_PAGE (NTFSNG_PAGE / 16)
#define NTFSNG_MAX_PAGES 65536
#define NTFSNG_MAX_RUNS 32

typedef struct { ULONGLONG Page, Count, Dev; } NTFSNG_EXTENT;

static ULONG NtfsJnlCrcTable[256];

static ULONG NtfsJnlCrc(ULONG Crc, const UCHAR *Data, ULONG Length)
{
    ULONG i, k, c;
    if (!NtfsJnlCrcTable[1])
    {
        for (i = 0; i < 256; i++)
        {
            for (c = i, k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320 ^ (c >> 1) : c >> 1;
            NtfsJnlCrcTable[i] = c;
        }
    }
    Crc = ~Crc;
    while (Length--)
        Crc = NtfsJnlCrcTable[(Crc ^ *Data++) & 0xff] ^ (Crc >> 8);
    return ~Crc;
}

static USHORT NtfsJnlG16(const UCHAR *p) { return p[0] | (p[1] << 8); }
static ULONG NtfsJnlG32(const UCHAR *p) { return NtfsJnlG16(p) | ((ULONG)NtfsJnlG16(p + 2) << 16); }
static ULONGLONG NtfsJnlG64(const UCHAR *p) { return NtfsJnlG32(p) | ((ULONGLONG)NtfsJnlG32(p + 4) << 32); }

static ULONGLONG NtfsJnlSlotPage(ULONGLONG Slot)
{
    ULONGLONG p = NTFSNG_FIRST_SLOT + Slot, q;
    for (q = 8; q <= p; q <<= 1)
        p++;
    return p;
}

static BOOLEAN NtfsJnlRaw(PNTFS_VOLUME_INFO Volume, ULONGLONG Offset, PVOID Buffer, ULONG Length)
{
    LARGE_INTEGER Position;
    ULONG Count;
    Position.QuadPart = Offset;
    return ArcSeek(Volume->DeviceId, &Position, SeekAbsolute) == ESUCCESS &&
           ArcRead(Volume->DeviceId, Buffer, Length, &Count) == ESUCCESS && Count == Length;
}

static BOOLEAN NtfsJnlLfPage(const NTFSNG_EXTENT *Ext, ULONG NumExt, ULONGLONG Page, PULONGLONG Dev)
{
    ULONG i;
    for (i = 0; i < NumExt; i++)
    {
        if (Page >= Ext[i].Page && Page < Ext[i].Page + Ext[i].Count)
        {
            *Dev = Ext[i].Dev + (Page - Ext[i].Page) * NTFSNG_PAGE;
            return TRUE;
        }
    }
    return FALSE;
}

/* MFT record @Index from the first, contiguous records (mirror as fallback), fixups undone. */
static BOOLEAN NtfsJnlRecord(PNTFS_VOLUME_INFO Volume, ULONG Index, PUCHAR Record)
{
    ULONG Size = Volume->MftRecordSize, Copy, i;
    ULONGLONG Lcn[2];
    Lcn[0] = Volume->BootSector.MftLocation;
    Lcn[1] = Volume->BootSector.MftMirrorLocation;
    for (Copy = 0; Copy < 2; Copy++)
    {
        USHORT Uo, Uc;
        BOOLEAN Ok = TRUE;
        if (!NtfsJnlRaw(Volume, Lcn[Copy] * Volume->ClusterSize + (ULONGLONG)Index * Size, Record, Size))
            continue;
        Uo = NtfsJnlG16(Record + 4);
        Uc = NtfsJnlG16(Record + 6);
        if (NtfsJnlG32(Record) != 0x454c4946 || Uc != Size / 512 + 1 || (Uo & 1) || Uo + 2 * Uc > Size)
            continue;
        for (i = 1; i < Uc && Ok; i++)
        {
            PUCHAR e = Record + i * 512 - 2;
            if (e[0] != Record[Uo] || e[1] != Record[Uo + 1])
                Ok = FALSE;
            e[0] = Record[Uo + 2 * i];
            e[1] = Record[Uo + 2 * i + 1];
        }
        if (Ok)
            return TRUE;
    }
    return FALSE;
}

/*
 * FALSE only when a committed transaction of this volume was recognised but cannot be supplied (a read
 * or an allocation failed): the disk then holds half of it, and the volume must not be read as it is.
 */
static BOOLEAN NtfsJournalLoad(PNTFS_VOLUME_INFO Volume)
{
    BOOLEAN Ok = TRUE;
    PUCHAR Rec = NULL, Page = NULL, Desc = NULL, a = NULL, p, End;
    NTFSNG_EXTENT Ext[NTFSNG_MAX_RUNS];
    ULONG NumExt = 0, Off, Len, NPages, NDesc, Crc, i, Uo;
    ULONGLONG LfPages, Covered, Vcn, Dev, Seq;
    LONGLONG Lcn = 0;
    USHORT Usn, UsnOld, UsnNew;

    if (Volume->MftRecordSize < 1024 || Volume->MftRecordSize > 4096 || Volume->ClusterSize < 512 ||
        (Volume->ClusterSize < NTFSNG_PAGE ? NTFSNG_PAGE % Volume->ClusterSize : Volume->ClusterSize % NTFSNG_PAGE))
        return TRUE;
    Rec = FrLdrTempAlloc(Volume->MftRecordSize, TAG_NTFS_DATA);
    Page = FrLdrTempAlloc(NTFSNG_PAGE, TAG_NTFS_DATA);
    Desc = FrLdrTempAlloc(NTFSNG_PAGE, TAG_NTFS_DATA);
    /* Below, a read or allocation failure means "cannot tell", which fails the mount (Ok FALSE) */
    if (!Rec || !Page || !Desc)
        goto unknown;

    /* $LogFile ($MFT record 2): the runs of its unnamed $DATA attribute, in pages */
    if (!NtfsJnlRecord(Volume, 2, Rec))
        goto unknown;
    for (Off = NtfsJnlG16(Rec + 0x14); Off + 16 <= Volume->MftRecordSize; Off += Len)
    {
        Len = NtfsJnlG32(Rec + Off + 4);
        if (NtfsJnlG32(Rec + Off) == 0xffffffff || Len < 16 || Len > Volume->MftRecordSize - Off)
            goto out;
        if (NtfsJnlG32(Rec + Off) == 0x80 && Rec[Off + 9] == 0)
        {
            a = Rec + Off;
            break;
        }
    }
    if (!a || !a[8] || Len < 0x40 || NtfsJnlG16(a + 0x20) >= Len)
        goto out;
    LfPages = NtfsJnlG64(a + 0x30) / NTFSNG_PAGE;
    Vcn = NtfsJnlG64(a + 0x10);
    End = a + Len;
    if (Vcn > ((ULONGLONG)1 << 40))
        goto out;
    for (p = a + NtfsJnlG16(a + 0x20); p < End && *p; )
    {
        ULONG lb = *p & 0xf, ob = *p >> 4, k;
        ULONGLONG Count = 0, Delta = 0;
        if (NumExt == NTFSNG_MAX_RUNS || !lb || lb > 8 || ob > 8 || p + 1 + lb + ob > End || !ob)
            goto out;
        for (k = lb; k > 0; k--)
            Count = (Count << 8) | p[k];
        for (k = ob; k > 0; k--)
            Delta = (Delta << 8) | p[lb + k];
        /* Sign-extend the offset in unsigned arithmetic (8-byte offsets are already full width) */
        if (ob < 8 && (p[lb + ob] & 0x80))
            Delta |= ~(ULONGLONG)0 << (8 * ob);
        if ((LONGLONG)Delta > ((LONGLONG)1 << 41) || (LONGLONG)Delta < -((LONGLONG)1 << 41))
            goto out;   /* out of range anyway; no overflow in the sum */
        Lcn += (LONGLONG)Delta;
        if (!Count || Count > ((ULONGLONG)1 << 40) || Lcn < 0 || Lcn > ((LONGLONG)1 << 40) ||
            ((Vcn * Volume->ClusterSize) % NTFSNG_PAGE) || ((Count * Volume->ClusterSize) % NTFSNG_PAGE))
            goto out;
        Ext[NumExt].Page = Vcn * Volume->ClusterSize / NTFSNG_PAGE;
        Ext[NumExt].Count = Count * Volume->ClusterSize / NTFSNG_PAGE;
        Ext[NumExt].Dev = Lcn * Volume->ClusterSize;
        NumExt++;
        Vcn += Count;
        p += 1 + lb + ob;
    }
    for (Covered = 0, i = 0; i < NumExt && Ext[i].Page == Covered; i++)
        Covered += Ext[i].Count;
    LfPages = min(min(LfPages, Covered), NTFSNG_MAX_PAGES);
    if (LfPages < 256)
        goto out;

    /* Restart pages in use (Windows wrote a log): not ours */
    for (i = 0; i < 2; i++)
    {
        if (!NtfsJnlLfPage(Ext, NumExt, i, &Dev))
            goto out;
        if (!NtfsJnlRaw(Volume, Dev, Page, NTFSNG_PAGE))
            goto unknown;
        if (NtfsJnlG32(Page) != 0xffffffff)
            goto out;
    }

    /* Header: a committed transaction of this volume, format 2, checksum right */
    if (!NtfsJnlLfPage(Ext, NumExt, NTFSNG_HDR_PAGE, &Dev))
        goto out;
    if (!NtfsJnlRaw(Volume, Dev, Page, NTFSNG_PAGE))
        goto unknown;
    if (!RtlEqualMemory(Page, "NTFSNGJ1", 8) || NtfsJnlG32(Page + 8) != 2 || NtfsJnlG32(Page + 12) != 2 ||
        NtfsJnlG32(Page + 64) != NtfsJnlCrc(0, Page, 64) || NtfsJnlG64(Page + 36) != Volume->BootSector.VolumeSerialNumber ||
        NtfsJnlG32(Page + 44) != NTFSNG_PAGE)
        goto out;
    Seq = NtfsJnlG64(Page + 16);
    NPages = NtfsJnlG32(Page + 24);
    NDesc = NtfsJnlG32(Page + 28);
    Crc = NtfsJnlG32(Page + 32);
    UsnOld = NtfsJnlG16(Page + 56);
    UsnNew = NtfsJnlG16(Page + 58);
    if (!NPages || NPages >= LfPages || NDesc != (NPages + NTFSNG_DESC_PER_PAGE - 1) / NTFSNG_DESC_PER_PAGE ||
        NtfsJnlSlotPage(NDesc + NPages - 1) >= LfPages)
        goto out;

    /* Still ours only while $Volume (record 3) carries a number the header recorded */
    if (!NtfsJnlRaw(Volume, Volume->BootSector.MftLocation * Volume->ClusterSize + 3 * Volume->MftRecordSize, Rec, 512))
        goto unknown;
    if (NtfsJnlG32(Rec) != 0x454c4946 || (Uo = NtfsJnlG16(Rec + 4)) + 2 > 512)
        goto out;
    Usn = NtfsJnlG16(Rec + Uo);
    if (Usn != UsnOld && Usn != UsnNew)
        goto out;

    /* Whole payload checksum, then every data page against its descriptor */
    {
        ULONG Sum = 0;
        for (i = 0; i < NDesc + NPages; i++)
        {
            if (!NtfsJnlLfPage(Ext, NumExt, NtfsJnlSlotPage(i), &Dev))
                goto out;
            if (!NtfsJnlRaw(Volume, Dev, Page, NTFSNG_PAGE))
                goto fail;      /* unreadable is not torn: the transaction may be half in place */
            Sum = NtfsJnlCrc(Sum, Page, NTFSNG_PAGE);
        }
        if (Sum != Crc)
            goto out;       /* torn: the transaction never reached its place */
    }
    Volume->JnlBlock = FrLdrTempAlloc(NPages * sizeof(ULONGLONG), TAG_NTFS_DATA);
    Volume->JnlSlot = FrLdrTempAlloc(NPages * sizeof(ULONGLONG), TAG_NTFS_DATA);
    Volume->JnlMask = FrLdrTempAlloc(NPages, TAG_NTFS_DATA);
    if (!Volume->JnlBlock || !Volume->JnlSlot || !Volume->JnlMask)
        goto fail;
    for (i = 0; i < NPages; i++)
    {
        PUCHAR d = Desc + (i % NTFSNG_DESC_PER_PAGE) * 16;
        if (i % NTFSNG_DESC_PER_PAGE == 0 &&
            (!NtfsJnlLfPage(Ext, NumExt, NtfsJnlSlotPage(i / NTFSNG_DESC_PER_PAGE), &Dev) ||
             !NtfsJnlRaw(Volume, Dev, Desc, NTFSNG_PAGE)))
            goto fail;
        if (!NtfsJnlLfPage(Ext, NumExt, NtfsJnlSlotPage(NDesc + i), &Dev) || !NtfsJnlRaw(Volume, Dev, Page, NTFSNG_PAGE) ||
            NtfsJnlCrc(0, Page, NTFSNG_PAGE) != NtfsJnlG32(d + 8) || (i && NtfsJnlG64(d) <= Volume->JnlBlock[i - 1]))
            goto fail;
        Volume->JnlBlock[i] = NtfsJnlG64(d);
        Volume->JnlSlot[i] = Dev;
        Volume->JnlMask[i] = d[12];
    }
    Volume->JnlPage = Page;
    Page = NULL;
    Volume->JnlCount = NPages;
    Volume->JnlLoaded = NPages;
    WARN("NTFS: showing the committed ntfsng journal transaction (seq %I64u, %lu pages)\n", Seq, NPages);
    goto out;
fail:
    if (Volume->JnlBlock)
        FrLdrTempFree(Volume->JnlBlock, TAG_NTFS_DATA);
    if (Volume->JnlSlot)
        FrLdrTempFree(Volume->JnlSlot, TAG_NTFS_DATA);
    if (Volume->JnlMask)
        FrLdrTempFree(Volume->JnlMask, TAG_NTFS_DATA);
    Volume->JnlBlock = Volume->JnlSlot = NULL;
    Volume->JnlMask = NULL;
    ERR("NTFS: the ntfsng journal holds a committed transaction that cannot be read\n");
    Ok = FALSE;
    goto out;
unknown:
    ERR("NTFS: cannot read enough of the volume to look for an ntfsng journal\n");
    Ok = FALSE;
out:
    if (Rec)
        FrLdrTempFree(Rec, TAG_NTFS_DATA);
    if (Page)
        FrLdrTempFree(Page, TAG_NTFS_DATA);
    if (Desc)
        FrLdrTempFree(Desc, TAG_NTFS_DATA);
    return Ok;
}

/*
 * Copies the journal's sectors over a buffer just read from the disk at @Offset.  FALSE: a sector the
 * transaction holds could not be read, so the read must fail rather than return the disk's version.
 */
static BOOLEAN NtfsJournalPatch(PNTFS_VOLUME_INFO Volume, ULONGLONG Offset, ULONGLONG Length, PCHAR Buffer)
{
    ULONGLONG Block, Last, a, z, Start, Stop;
    ULONG Lo, Hi, Mid, s;

    if (!Volume->JnlCount || !Length)
        return TRUE;
    Last = (Offset + Length - 1) / NTFSNG_PAGE;
    for (Block = Offset / NTFSNG_PAGE; Block <= Last; Block++)
    {
        Lo = 0;
        Hi = Volume->JnlCount;
        while (Lo < Hi)
        {
            Mid = (Lo + Hi) / 2;
            if (Volume->JnlBlock[Mid] < Block)
                Lo = Mid + 1;
            else
                Hi = Mid;
        }
        if (Lo == Volume->JnlCount || Volume->JnlBlock[Lo] != Block)
            continue;
        if (Volume->JnlLoaded != Lo)
        {
            Volume->JnlLoaded = Volume->JnlCount;
            if (!NtfsJnlRaw(Volume, Volume->JnlSlot[Lo], Volume->JnlPage, NTFSNG_PAGE))
            {
                ERR("NTFS: cannot read the ntfsng journal\n");
                return FALSE;
            }
            Volume->JnlLoaded = Lo;
        }
        for (s = 0; s < NTFSNG_PAGE / 512; s++)
        {
            if (!(Volume->JnlMask[Lo] & (1 << s)))
                continue;
            Start = Block * NTFSNG_PAGE + s * 512;
            Stop = Start + 512;
            a = max(Start, Offset);
            z = min(Stop, Offset + Length);
            if (a < z)
                RtlCopyMemory(Buffer + (a - Offset), Volume->JnlPage + (a - Block * NTFSNG_PAGE), (SIZE_T)(z - a));
        }
    }
    return TRUE;
}

const DEVVTBL* NtfsMount(ULONG DeviceId)
{
    PNTFS_VOLUME_INFO Volume;
    LARGE_INTEGER Position;
    ULONG Count;
    ARC_STATUS Status;

    TRACE("Enter NtfsMount(%lu)\n", DeviceId);

    //
    // Allocate data for volume information
    //
    Volume = FrLdrTempAlloc(sizeof(NTFS_VOLUME_INFO), TAG_NTFS_VOLUME);
    if (!Volume)
        return NULL;
    RtlZeroMemory(Volume, sizeof(NTFS_VOLUME_INFO));

    //
    // Read the BootSector
    //
    Position.QuadPart = 0;
    Status = ArcSeek(DeviceId, &Position, SeekAbsolute);
    if (Status != ESUCCESS)
    {
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }
    Status = ArcRead(DeviceId, &Volume->BootSector, sizeof(Volume->BootSector), &Count);
    if (Status != ESUCCESS || Count != sizeof(Volume->BootSector))
    {
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }

    //
    // Check if BootSector is valid. If no, return early
    //
    if (!RtlEqualMemory(Volume->BootSector.SystemId, "NTFS", 4))
    {
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }

    //
    // Calculate cluster size and MFT record size
    //
    Volume->ClusterSize = Volume->BootSector.SectorsPerCluster * Volume->BootSector.BytesPerSector;
    if (Volume->BootSector.ClustersPerMftRecord > 0)
        Volume->MftRecordSize = Volume->BootSector.ClustersPerMftRecord * Volume->ClusterSize;
    else
        Volume->MftRecordSize = 1 << (-Volume->BootSector.ClustersPerMftRecord);
    if (Volume->BootSector.ClustersPerIndexRecord > 0)
        Volume->IndexRecordSize = Volume->BootSector.ClustersPerIndexRecord * Volume->ClusterSize;
    else
        Volume->IndexRecordSize = 1 << (-Volume->BootSector.ClustersPerIndexRecord);

    TRACE("ClusterSize: 0x%x\n", Volume->ClusterSize);
    TRACE("ClustersPerMftRecord: %d\n", Volume->BootSector.ClustersPerMftRecord);
    TRACE("ClustersPerIndexRecord: %d\n", Volume->BootSector.ClustersPerIndexRecord);
    TRACE("MftRecordSize: 0x%x\n", Volume->MftRecordSize);
    TRACE("IndexRecordSize: 0x%x\n", Volume->IndexRecordSize);

    //
    // Keep device id
    //
    Volume->DeviceId = DeviceId;

    //
    // A committed ntfsng journal transaction is read over the disk from here on
    //
    if (!NtfsJournalLoad(Volume))
    {
        FileSystemError("The NTFS journal holds changes that cannot be read.");
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }

    //
    // Keep room to read partial sectors
    //
    Volume->TemporarySector = FrLdrTempAlloc(Volume->BootSector.BytesPerSector, TAG_NTFS_DATA);
    if (!Volume->TemporarySector)
    {
        FileSystemError("Failed to allocate memory.");
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }

    //
    // Read MFT index
    //
    TRACE("Reading MFT index...\n");
    Volume->MasterFileTable = FrLdrTempAlloc(Volume->MftRecordSize, TAG_NTFS_MFT);
    if (!Volume->MasterFileTable)
    {
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }
    if (!NtfsDiskRead(Volume, Volume->BootSector.MftLocation * Volume->ClusterSize, Volume->MftRecordSize,
                      (PCHAR)Volume->MasterFileTable))
    {
        FileSystemError("Failed to read the Master File Table record.");
        FrLdrTempFree(Volume->MasterFileTable, TAG_NTFS_MFT);
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }

    //
    // Search DATA attribute
    //
    TRACE("Searching for DATA attribute...\n");
    Volume->MFTContext = NtfsFindAttribute(Volume, Volume->MasterFileTable, 0, NTFS_ATTR_TYPE_DATA, L"");
    if (!Volume->MFTContext)
    {
        FileSystemError("Can't find data attribute for Master File Table.");
        FrLdrTempFree(Volume->MasterFileTable, TAG_NTFS_MFT);
        FrLdrTempFree(Volume, TAG_NTFS_VOLUME);
        return NULL;
    }

    //
    // Remember NTFS volume information
    //
    NtfsVolumes[DeviceId] = Volume;

    //
    // Return success
    //
    TRACE("NtfsMount(%lu) success\n", DeviceId);
    return &NtfsFuncTable;
}

#endif
