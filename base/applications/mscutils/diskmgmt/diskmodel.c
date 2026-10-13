/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disk, partition and volume model (no user interface)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <strsafe.h>

#include "diskmodel.h"

/* The type GUIDs are spelled out here so that no module has to define them. */
static const GUID DmGuidEfi      = { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
static const GUID DmGuidReserved = { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };
static const GUID DmGuidRecovery = { 0xDE94BBA4, 0x06D1, 0x4D40, { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } };
static const GUID DmGuidLdmMeta  = { 0x5808C8AA, 0x7E8F, 0x42E0, { 0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3 } };
static const GUID DmGuidLdmData  = { 0xAF9B60A0, 0x1431, 0x4F62, { 0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD } };

#define DM_MBR_EXTENDED_CHS   0x05
#define DM_MBR_EXTENDED_LBA   0x0F
#define DM_MBR_EXTENDED_LINUX 0x85
#define DM_MBR_RECOVERY       0x27
#define DM_MBR_EFI            0xEF
#define DM_MBR_LDM            0x42

/* A gap is drawn only when it is larger than this; the sliver before an extended
 * boot record and the first mebibyte of the disk stay unmarked, as in Windows. */
#define DM_MIN_GAP DM_MIB

/* ---------------------------------------------------------------- formatting */

VOID
DmFormatSizeMB(ULONGLONG MegaBytes, PWSTR Buffer, SIZE_T Count)
{
    DmFormatSize(MegaBytes * DM_MIB, Buffer, Count);
}

VOID
DmFormatSize(ULONGLONG Bytes, PWSTR Buffer, SIZE_T Count)
{
    ULONGLONG Rounded = (Bytes / DM_MIB) + (((Bytes % DM_MIB) >= (DM_MIB / 2)) ? 1 : 0);

    if (Rounded >= 1024 * 1024)
    {
        ULONGLONG Hundredths = (Bytes / DM_MIB * 100 + 512 * 1024) / (1024 * 1024);
        StringCchPrintfW(Buffer, Count, L"%I64u.%02u TB", Hundredths / 100, (UINT)(Hundredths % 100));
    }
    else if (Rounded >= 1024)
    {
        ULONGLONG Hundredths = (Bytes / (DM_MIB / 4) * 100 + 2048) / 4096;
        StringCchPrintfW(Buffer, Count, L"%I64u.%02u GB", Hundredths / 100, (UINT)(Hundredths % 100));
    }
    else if (Rounded >= 1)
    {
        StringCchPrintfW(Buffer, Count, L"%I64u MB", Rounded);
    }
    else if (Bytes == 0)
    {
        StringCchCopyW(Buffer, Count, L"0 MB");
    }
    else
    {
        StringCchPrintfW(Buffer, Count, L"%I64u KB", Bytes / 1024);
    }
}

VOID
DmFormatPercent(ULONGLONG Part, ULONGLONG Whole, PWSTR Buffer, SIZE_T Count)
{
    ULONGLONG Percent = 0;

    if (Whole != 0)
    {
        /* keep the multiplication inside 64 bits for very large volumes */
        if (Whole > (0xFFFFFFFFFFFFFFFFULL / 100))
        {
            Part >>= 10;
            Whole >>= 10;
        }
        Percent = (Part * 100 + Whole / 2) / Whole;
    }
    StringCchPrintfW(Buffer, Count, L"%u %%", (UINT)Percent);
}

PCWSTR
DmBusText(ULONG BusType)
{
    switch (BusType)
    {
        case BusTypeScsi:       return L"SCSI";
        case BusTypeAtapi:      return L"IDE";
        case BusTypeAta:        return L"IDE";
        case BusType1394:       return L"1394";
        case BusTypeSsa:        return L"SSA";
        case BusTypeFibre:      return L"Fibre";
        case BusTypeUsb:        return L"USB";
        case BusTypeRAID:       return L"RAID";
        case BusTypeiScsi:      return L"iSCSI";
        case BusTypeSas:        return L"SAS";
        case BusTypeSata:       return L"SATA";
        case BusTypeSd:         return L"SD";
        case BusTypeMmc:        return L"MMC";
        default:                return L"UNKNOWN";
    }
}

/* ------------------------------------------------------------ model plumbing */

PDM_MODEL
DmCreateModel(VOID)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DM_MODEL));
}

VOID
DmFreeDisk(PDM_DISK Disk)
{
    if (Disk == NULL)
        return;
    if (Disk->Regions)
        HeapFree(GetProcessHeap(), 0, Disk->Regions);
    HeapFree(GetProcessHeap(), 0, Disk);
}

VOID
DmFreeModel(PDM_MODEL Model)
{
    ULONG i;

    if (Model == NULL)
        return;
    for (i = 0; i < Model->DiskCount; i++)
        DmFreeDisk(Model->Disks[i]);
    if (Model->Disks)
        HeapFree(GetProcessHeap(), 0, Model->Disks);
    HeapFree(GetProcessHeap(), 0, Model);
}

static BOOL
DmDiskBefore(const DM_DISK *a, const DM_DISK *b)
{
    BOOL aOptical = (a->Device == DM_DEV_CDROM);
    BOOL bOptical = (b->Device == DM_DEV_CDROM);

    if (aOptical != bOptical)
        return !aOptical;
    return a->Number < b->Number;
}

BOOL
DmAddDisk(PDM_MODEL Model, PDM_DISK Disk)
{
    PDM_DISK *Array;
    ULONG i, Pos;

    Array = HeapAlloc(GetProcessHeap(), 0, (Model->DiskCount + 1) * sizeof(PDM_DISK));
    if (Array == NULL)
        return FALSE;

    for (Pos = 0; Pos < Model->DiskCount; Pos++)
    {
        if (DmDiskBefore(Disk, Model->Disks[Pos]))
            break;
    }
    for (i = 0; i < Pos; i++)
        Array[i] = Model->Disks[i];
    Array[Pos] = Disk;
    for (i = Pos; i < Model->DiskCount; i++)
        Array[i + 1] = Model->Disks[i];

    if (Model->Disks)
        HeapFree(GetProcessHeap(), 0, Model->Disks);
    Model->Disks = Array;
    Model->DiskCount++;

    for (i = 0; i < Model->DiskCount; i++)
    {
        ULONG r;
        for (r = 0; r < Model->Disks[i]->RegionCount; r++)
            Model->Disks[i]->Regions[r].Disk = Model->Disks[i];
    }
    return TRUE;
}

/* ------------------------------------------------------------ disk building */

typedef struct _DM_PART
{
    ULONGLONG Offset;
    ULONGLONG Length;
    const PARTITION_INFORMATION_EX *Entry;
    BOOL Logical;
    BOOL Hidden;
} DM_PART;

static BOOL
DmIsExtendedType(UCHAR Type)
{
    return Type == DM_MBR_EXTENDED_CHS || Type == DM_MBR_EXTENDED_LBA || Type == DM_MBR_EXTENDED_LINUX;
}

static PDM_REGION
DmAddRegion(PDM_DISK Disk, DM_KIND Kind, BOOL InExtended, ULONGLONG Offset, ULONGLONG Length)
{
    PDM_REGION Region = &Disk->Regions[Disk->RegionCount++];

    Region->Kind = Kind;
    Region->InExtended = InExtended;
    Region->Offset = Offset;
    Region->Length = Length;
    Region->Disk = Disk;
    return Region;
}

static VOID
DmAddGap(PDM_DISK Disk, BOOL InExtended, ULONGLONG From, ULONGLONG To)
{
    if (To > From && (To - From) > DM_MIN_GAP)
        DmAddRegion(Disk, InExtended ? DM_KIND_FREE : DM_KIND_UNALLOCATED, InExtended, From, To - From);
}

static VOID
DmSetStatusText(PDM_DISK Disk, PCWSTR Text)
{
    StringCchCopyW(Disk->StatusText, ARRAYSIZE(Disk->StatusText), Text);
}

BOOL
DmBuildDisk(const DM_DISK_INPUT *Input, PDM_DISK *Result)
{
    PDM_DISK Disk;
    const DRIVE_LAYOUT_INFORMATION_EX *Layout = Input->Layout;
    DM_PART *Parts = NULL;
    ULONG Count = 0, i, j, Max;
    ULONGLONG ExtOffset = 0, ExtLength = 0, UsableStart, UsableEnd, Cursor, Used = 0, Reserved = 0;
    BOOL HaveExt = FALSE;

    *Result = NULL;
    Disk = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DM_DISK));
    if (Disk == NULL)
        return FALSE;

    Disk->Device = Input->Device;
    Disk->Number = Input->Number;
    Disk->Length = Input->DiskLength;
    Disk->BytesPerSector = Input->BytesPerSector;
    Disk->HasMedia = Input->HasMedia;
    Disk->BusType = Input->BusType;
    Disk->ScsiPath = Input->ScsiPath;
    Disk->ScsiTarget = Input->ScsiTarget;
    Disk->ScsiLun = Input->ScsiLun;
    StringCchCopyW(Disk->DeviceName, ARRAYSIZE(Disk->DeviceName), Input->DeviceName);
    StringCchCopyW(Disk->BusText, ARRAYSIZE(Disk->BusText), DmBusText(Input->BusType));
    DmSetStatusText(Disk, L"Online");

    if (Layout == NULL || Input->LayoutError != 0)
    {
        if (Input->Removable && !Input->HasMedia)
            DmSetStatusText(Disk, L"No Media");
        else if (Input->LayoutError == ERROR_NOT_READY)
            DmSetStatusText(Disk, L"Offline");
        else
            DmSetStatusText(Disk, L"Unknown");
        Disk->Style = DM_STYLE_RAW;
        Disk->CapacityMB = Disk->Length / DM_MIB;
        Disk->UnallocatedMB = Disk->CapacityMB;
        *Result = Disk;
        return TRUE;
    }

    switch (Layout->PartitionStyle)
    {
        case PARTITION_STYLE_MBR:
            Disk->Style = DM_STYLE_MBR;
            Disk->Signature = Layout->Mbr.Signature;
            break;
        case PARTITION_STYLE_GPT:
            Disk->Style = DM_STYLE_GPT;
            Disk->DiskId = Layout->Gpt.DiskId;
            break;
        default:
            Disk->Style = DM_STYLE_RAW;
            break;
    }

    if (Disk->Style == DM_STYLE_RAW)
    {
        Disk->CapacityMB = Disk->Length / DM_MIB;
        Disk->UnallocatedMB = Disk->CapacityMB;
        DmSetStatusText(Disk, L"Not Initialized");
        Disk->Regions = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DM_REGION));
        if (Disk->Regions == NULL)
        {
            DmFreeDisk(Disk);
            return FALSE;
        }
        if (Disk->Length > 0)
            DmAddRegion(Disk, DM_KIND_UNALLOCATED, FALSE, 0, Disk->Length);
        *Result = Disk;
        return TRUE;
    }

    Max = Layout->PartitionCount;
    Parts = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (Max + 1) * sizeof(DM_PART));
    Disk->Regions = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (2 * Max + 4) * sizeof(DM_REGION));
    if (Parts == NULL || Disk->Regions == NULL)
    {
        if (Parts)
            HeapFree(GetProcessHeap(), 0, Parts);
        DmFreeDisk(Disk);
        return FALSE;
    }

    /* take what the driver lists */
    for (i = 0; i < Max; i++)
    {
        const PARTITION_INFORMATION_EX *Entry = &Layout->PartitionEntry[i];
        ULONGLONG Length = (ULONGLONG)Entry->PartitionLength.QuadPart;
        ULONGLONG Offset = (ULONGLONG)Entry->StartingOffset.QuadPart;

        if (Length == 0)
            continue;

        if (Disk->Style == DM_STYLE_MBR)
        {
            UCHAR Type = Entry->Mbr.PartitionType;

            if (Type == PARTITION_ENTRY_UNUSED)
                continue;
            if (DmIsExtendedType(Type))
            {
                if (!HaveExt || Length > ExtLength)
                {
                    HaveExt = TRUE;
                    ExtOffset = Offset;
                    ExtLength = Length;
                }
                continue;
            }
            if (Type == DM_MBR_LDM)
                Disk->Dynamic = TRUE;
        }
        else
        {
            const GUID *Type = &Entry->Gpt.PartitionType;
            static const GUID Zero;

            if (memcmp(Type, &Zero, sizeof(GUID)) == 0)
                continue;
            if (memcmp(Type, &DmGuidLdmMeta, sizeof(GUID)) == 0 || memcmp(Type, &DmGuidLdmData, sizeof(GUID)) == 0)
                Disk->Dynamic = TRUE;
            if (memcmp(Type, &DmGuidReserved, sizeof(GUID)) == 0)
                Parts[Count].Hidden = TRUE;
        }

        Parts[Count].Offset = Offset;
        Parts[Count].Length = Length;
        Parts[Count].Entry = Entry;
        Count++;
    }

    /* in disk order */
    for (i = 1; i < Count; i++)
    {
        DM_PART Key = Parts[i];

        for (j = i; j > 0 && Parts[j - 1].Offset > Key.Offset; j--)
            Parts[j] = Parts[j - 1];
        Parts[j] = Key;
    }

    for (i = 0; i < Count; i++)
    {
        Parts[i].Logical = (HaveExt && Parts[i].Offset >= ExtOffset && Parts[i].Offset < ExtOffset + ExtLength);
        if (Parts[i].Hidden)
            Reserved += Parts[i].Length;
        else
            Used += Parts[i].Length;
    }

    /* the usable range of the disk */
    if (Disk->Style == DM_STYLE_GPT)
    {
        UsableStart = (ULONGLONG)Layout->Gpt.StartingUsableOffset.QuadPart;
        UsableEnd = UsableStart + (ULONGLONG)Layout->Gpt.UsableLength.QuadPart;
        /* the backup table takes a few sectors of the last mebibyte: Windows counts that mebibyte as usable */
        UsableEnd = (UsableEnd + DM_MIB / 2) / DM_MIB * DM_MIB;
    }
    else
    {
        UsableStart = DM_MIB;
        UsableEnd = Disk->Length - (Disk->Length % DM_MIB);
        UsableEnd = (UsableEnd > DM_MIB) ? UsableEnd - DM_MIB : 0;
    }
    if (UsableEnd > Disk->Length)
        UsableEnd = Disk->Length - (Disk->Length % DM_MIB);

    Disk->ReservedMB = Reserved / DM_MIB;
    Disk->CapacityMB = (UsableEnd > Reserved ? UsableEnd - Reserved : 0) / DM_MIB;
    Disk->UnallocatedMB = (UsableEnd > Reserved + Used ? UsableEnd - Reserved - Used : 0) / DM_MIB;

    /* walk the disk and lay out the blocks: top-level partitions and the extended partition in disk order */
    {
        ULONG *Top = HeapAlloc(GetProcessHeap(), 0, (Count + 1) * sizeof(ULONG));
        ULONG TopCount = 0, ExtIndex = (ULONG)-1;
        BOOL ExtDone = !HaveExt;

        if (Top == NULL)
        {
            HeapFree(GetProcessHeap(), 0, Parts);
            DmFreeDisk(Disk);
            return FALSE;
        }
        for (i = 0; i < Count; i++)
            if (!Parts[i].Logical)
                Top[TopCount++] = i;

        Cursor = UsableStart;
        for (j = 0; j <= TopCount; j++)
        {
            /* the extended partition goes before the first top-level partition that starts after it */
            BOOL ExtHere = !ExtDone && (j == TopCount || Parts[Top[j]].Offset > ExtOffset);

            if (ExtHere)
            {
                ULONGLONG Inner = ExtOffset, ExtEnd = ExtOffset + ExtLength;

                DmAddGap(Disk, FALSE, Cursor, ExtOffset);
                for (i = 0; i < Count; i++)
                {
                    PDM_REGION Region;

                    if (!Parts[i].Logical || Parts[i].Hidden)
                        continue;
                    DmAddGap(Disk, TRUE, Inner, Parts[i].Offset);
                    Region = DmAddRegion(Disk, DM_KIND_LOGICAL, TRUE, Parts[i].Offset, Parts[i].Length);
                    Region->PartitionNumber = Parts[i].Entry->PartitionNumber;
                    Region->MbrType = Parts[i].Entry->Mbr.PartitionType;
                    Region->IsDynamic = (Region->MbrType == DM_MBR_LDM);
                    Inner = Parts[i].Offset + Parts[i].Length;
                }
                DmAddGap(Disk, TRUE, Inner, ExtEnd);
                if (ExtEnd > Cursor)
                    Cursor = ExtEnd;
                ExtDone = TRUE;
                ExtIndex = j;
            }
            if (j == TopCount)
                break;

            i = Top[j];
            DmAddGap(Disk, FALSE, Cursor, Parts[i].Offset);
            if (!Parts[i].Hidden)
            {
                PDM_REGION Region = DmAddRegion(Disk, DM_KIND_PRIMARY, FALSE, Parts[i].Offset, Parts[i].Length);

                Region->PartitionNumber = Parts[i].Entry->PartitionNumber;
                if (Disk->Style == DM_STYLE_MBR)
                {
                    Region->MbrType = Parts[i].Entry->Mbr.PartitionType;
                    Region->Active = Parts[i].Entry->Mbr.BootIndicator ? TRUE : FALSE;
                    Region->IsRecovery = (Region->MbrType == DM_MBR_RECOVERY);
                    Region->IsEfi = (Region->MbrType == DM_MBR_EFI);
                    Region->IsDynamic = (Region->MbrType == DM_MBR_LDM);
                }
            }
            if (Parts[i].Offset + Parts[i].Length > Cursor)
                Cursor = Parts[i].Offset + Parts[i].Length;
        }
        UNREFERENCED_PARAMETER(ExtIndex);
        HeapFree(GetProcessHeap(), 0, Top);
        DmAddGap(Disk, FALSE, Cursor, UsableEnd);
    }

    /* GPT identity of the visible partitions */
    if (Disk->Style == DM_STYLE_GPT)
    {
        ULONG r;

        for (r = 0; r < Disk->RegionCount; r++)
        {
            PDM_REGION Region = &Disk->Regions[r];

            for (i = 0; i < Count; i++)
            {
                if (Parts[i].Offset == Region->Offset && Region->Kind == DM_KIND_PRIMARY && !Parts[i].Hidden)
                {
                    const PARTITION_INFORMATION_GPT *Gpt = &Parts[i].Entry->Gpt;

                    Region->GptType = Gpt->PartitionType;
                    Region->GptId = Gpt->PartitionId;
                    StringCchCopyNW(Region->GptName, ARRAYSIZE(Region->GptName), Gpt->Name, 36);
                    Region->IsEfi = (memcmp(&Gpt->PartitionType, &DmGuidEfi, sizeof(GUID)) == 0);
                    Region->IsRecovery = (memcmp(&Gpt->PartitionType, &DmGuidRecovery, sizeof(GUID)) == 0);
                    Region->IsDynamic = (memcmp(&Gpt->PartitionType, &DmGuidLdmData, sizeof(GUID)) == 0);
                    break;
                }
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, Parts);
    *Result = Disk;
    return TRUE;
}

/* ------------------------------------------------------------------ volumes */

static PDM_DISK
DmFindDisk(const DM_MODEL *Model, DM_DEVICE Device, ULONG Number)
{
    ULONG i;

    for (i = 0; i < Model->DiskCount; i++)
    {
        PDM_DISK Disk = Model->Disks[i];

        if (Disk->Number != Number)
            continue;
        if ((Device == DM_DEV_CDROM) == (Disk->Device == DM_DEV_CDROM))
            return Disk;
    }
    return NULL;
}

static VOID
DmCopyVolume(PDM_REGION Region, const DM_VOLUME_INFO *Volume)
{
    Region->HasVolume = TRUE;
    StringCchCopyW(Region->VolumeGuid, ARRAYSIZE(Region->VolumeGuid), Volume->Guid);
    StringCchCopyW(Region->Letter, ARRAYSIZE(Region->Letter), Volume->Letter);
    memcpy(Region->MountPaths, Volume->MountPaths, sizeof(Region->MountPaths));
    Region->HasFsInfo = Volume->HasFsInfo;
    if (Volume->HasFsInfo)
    {
        StringCchCopyW(Region->Label, ARRAYSIZE(Region->Label), Volume->Label);
        StringCchCopyW(Region->FileSystem, ARRAYSIZE(Region->FileSystem), Volume->FileSystem);
    }
    Region->HasSpace = Volume->HasSpace;
    Region->VolTotal = Volume->Total;
    Region->VolFree = Volume->Free;
}

BOOL
DmAttachVolume(PDM_MODEL Model, const DM_VOLUME_INFO *Volume)
{
    PDM_DISK Disk;
    ULONG i;

    if (!Volume->HaveDeviceNumber)
        return FALSE;

    if (Volume->DeviceType == FILE_DEVICE_CD_ROM || Volume->DriveType == DRIVE_CDROM)
    {
        PDM_REGION Region;

        Disk = DmFindDisk(Model, DM_DEV_CDROM, Volume->DeviceNumber);
        if (Disk == NULL)
        {
            Disk = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DM_DISK));
            if (Disk == NULL)
                return FALSE;
            Disk->Device = DM_DEV_CDROM;
            Disk->Number = Volume->DeviceNumber;
            Disk->BusType = 0xFF;
            Disk->ScsiPath = Disk->ScsiTarget = Disk->ScsiLun = -1;
            StringCchCopyW(Disk->BusText, ARRAYSIZE(Disk->BusText), DmBusText(0xFF));
            if (!DmAddDisk(Model, Disk))
            {
                HeapFree(GetProcessHeap(), 0, Disk);
                return FALSE;
            }
        }
        if (Volume->Letter[0] && Disk->DriveLetter[0] == 0)
            StringCchCopyW(Disk->DriveLetter, ARRAYSIZE(Disk->DriveLetter), Volume->Letter);
        if (Volume->MediaKind[0])
            StringCchCopyW(Disk->MediaKind, ARRAYSIZE(Disk->MediaKind), Volume->MediaKind);
        Disk->HasMedia = Volume->HasMedia;
        DmSetStatusText(Disk, Volume->HasMedia ? L"Online" : L"No Media");
        if (Disk->Regions == NULL)
            Disk->Regions = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DM_REGION));
        if (Disk->Regions == NULL)
            return FALSE;
        if (Volume->HasMedia && Disk->RegionCount == 0)
        {
            Region = DmAddRegion(Disk, DM_KIND_PRIMARY, FALSE, 0, Volume->HasSpace ? Volume->Total : 0);
            Region->PartitionNumber = 1;
            DmCopyVolume(Region, Volume);
            Disk->Length = Region->Length;
            Disk->CapacityMB = Disk->Length / DM_MIB;
        }
        return TRUE;
    }

    Disk = DmFindDisk(Model, DM_DEV_DISK, Volume->DeviceNumber);
    if (Disk == NULL)
        return FALSE;

    if (Volume->Letter[0] && Disk->DriveLetter[0] == 0)
        StringCchCopyW(Disk->DriveLetter, ARRAYSIZE(Disk->DriveLetter), Volume->Letter);

    /* a medium without a partition table: the volume is the whole disk */
    if ((Volume->PartitionNumber == 0 || Volume->PartitionNumber == (ULONG)-1) &&
        Disk->RegionCount == 1 && Disk->Regions[0].Kind == DM_KIND_UNALLOCATED && Disk->Style == DM_STYLE_RAW)
    {
        Disk->Regions[0].Kind = DM_KIND_PRIMARY;
        Disk->Regions[0].PartitionNumber = 1;
        DmCopyVolume(&Disk->Regions[0], Volume);
        Disk->Style = DM_STYLE_MBR;
        Disk->CapacityMB = Disk->Length / DM_MIB;
        Disk->UnallocatedMB = 0;
        DmSetStatusText(Disk, L"Online");
        return TRUE;
    }

    for (i = 0; i < Disk->RegionCount; i++)
    {
        PDM_REGION Region = &Disk->Regions[i];

        if ((Region->Kind == DM_KIND_PRIMARY || Region->Kind == DM_KIND_LOGICAL) &&
            Region->PartitionNumber == Volume->PartitionNumber)
        {
            if (Region->HasVolume)
                return FALSE;
            DmCopyVolume(Region, Volume);
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------- status */

VOID
DmFinishModel(PDM_MODEL Model, const DM_SYSINFO *Info)
{
    ULONG d, r;

    for (d = 0; d < Model->DiskCount; d++)
    {
        PDM_DISK Disk = Model->Disks[d];

        for (r = 0; r < Disk->RegionCount; r++)
        {
            PDM_REGION Region = &Disk->Regions[r];
            WCHAR Text[160];
            BOOL First = TRUE;

            Region->IsBoot = Region->IsSystem = Region->IsPageFile = Region->IsCrashDump = FALSE;
            Region->Status[0] = 0;
            if (Region->Kind != DM_KIND_PRIMARY && Region->Kind != DM_KIND_LOGICAL)
                continue;

            if (Info && Info->HaveBootDevice)
            {
                if (Disk->Device == DM_DEV_CDROM)
                {
                    Region->IsBoot = Info->BootIsCdrom && Disk->Number == Info->BootDisk;
                }
                else if (!Info->BootIsCdrom && Disk->Number == Info->BootDisk)
                {
                    Region->IsBoot = (Region->PartitionNumber == Info->BootPartition);
                    Region->IsSystem = Region->Active;
                }
            }
            if (Info)
            {
                if (Region->Letter[0] >= L'A' && Region->Letter[0] <= L'Z' &&
                    (Info->PagefileLetters & (1UL << (Region->Letter[0] - L'A'))))
                {
                    Region->IsPageFile = TRUE;
                }
                if (Info->PagefileOnBoot && Region->IsBoot)
                    Region->IsPageFile = TRUE;
                if (Info->CrashDumpEnabled && Region->IsBoot)
                    Region->IsCrashDump = TRUE;
            }

            Text[0] = 0;
#define ADD_TOKEN(Cond, Str) \
    do { if (Cond) { \
        if (!First) StringCchCatW(Text, ARRAYSIZE(Text), L", "); \
        StringCchCatW(Text, ARRAYSIZE(Text), Str); First = FALSE; } } while (0)
            ADD_TOKEN(Region->IsSystem, L"System");
            ADD_TOKEN(Region->IsBoot, L"Boot");
            ADD_TOKEN(Region->IsPageFile, L"Page File");
            ADD_TOKEN(Region->Active, L"Active");
            ADD_TOKEN(Region->IsCrashDump, L"Crash Dump");
            ADD_TOKEN(TRUE, Region->Kind == DM_KIND_LOGICAL ? L"Logical Drive" :
                            Region->IsEfi ? L"EFI System Partition" :
                            Region->IsRecovery ? L"Recovery Partition" : L"Primary Partition");
#undef ADD_TOKEN
            StringCchPrintfW(Region->Status, ARRAYSIZE(Region->Status), L"Healthy (%s)", Text);
        }
    }
}

/* -------------------------------------------------------------------- texts */

static VOID
DmRegionName(const DM_REGION *Region, PWSTR Buffer, SIZE_T Count)
{
    Buffer[0] = 0;
    if (!Region->HasVolume)
        return;

    if (Region->Letter[0])
    {
        if (Region->Label[0])
            StringCchPrintfW(Buffer, Count, L"%s (%s)", Region->Label, Region->Letter);
        else
            StringCchPrintfW(Buffer, Count, L"(%s)", Region->Letter);
    }
    else if (Region->Label[0])
    {
        StringCchCopyW(Buffer, Count, Region->Label);
    }
}

VOID
DmBlockText(const DM_DISK *Disk, const DM_REGION *Region, PDM_BLOCK_TEXT Text)
{
    WCHAR Size[24];

    UNREFERENCED_PARAMETER(Disk);
    Text->Line1[0] = Text->Line2[0] = Text->Line3[0] = 0;
    DmFormatSize(Region->Length, Size, ARRAYSIZE(Size));

    switch (Region->Kind)
    {
        case DM_KIND_UNALLOCATED:
            StringCchCopyW(Text->Line2, ARRAYSIZE(Text->Line2), Size);
            StringCchCopyW(Text->Line3, ARRAYSIZE(Text->Line3), L"Unallocated");
            break;
        case DM_KIND_FREE:
            StringCchCopyW(Text->Line2, ARRAYSIZE(Text->Line2), Size);
            StringCchCopyW(Text->Line3, ARRAYSIZE(Text->Line3), L"Free space");
            break;
        default:
            DmRegionName(Region, Text->Line1, ARRAYSIZE(Text->Line1));
            if (Region->HasVolume && Region->FileSystem[0])
                StringCchPrintfW(Text->Line2, ARRAYSIZE(Text->Line2), L"%s %s", Size, Region->FileSystem);
            else
                StringCchCopyW(Text->Line2, ARRAYSIZE(Text->Line2), Size);
            StringCchCopyW(Text->Line3, ARRAYSIZE(Text->Line3), Region->Status);
            break;
    }
}

VOID
DmLabelText(const DM_DISK *Disk, PDM_LABEL_TEXT Text)
{
    ZeroMemory(Text, sizeof(*Text));

    if (Disk->Device == DM_DEV_CDROM)
    {
        PCWSTR Kind = Disk->MediaKind[0] ? Disk->MediaKind : L"CD-ROM";

        StringCchPrintfW(Text->Line1, ARRAYSIZE(Text->Line1), L"CD-ROM %u", Disk->Number);
        /* with a disc in the drive the letter is on the block of the disc */
        if (Disk->DriveLetter[0] && !Disk->HasMedia)
            StringCchPrintfW(Text->Line2, ARRAYSIZE(Text->Line2), L"%s (%s)", Kind, Disk->DriveLetter);
        else
            StringCchCopyW(Text->Line2, ARRAYSIZE(Text->Line2), Kind);
    }
    else
    {
        StringCchPrintfW(Text->Line1, ARRAYSIZE(Text->Line1), L"Disk %u", Disk->Number);
        if (Disk->Device == DM_DEV_REMOVABLE)
        {
            if (Disk->DriveLetter[0])
                StringCchPrintfW(Text->Line2, ARRAYSIZE(Text->Line2), L"Removable (%s)", Disk->DriveLetter);
            else
                StringCchCopyW(Text->Line2, ARRAYSIZE(Text->Line2), L"Removable");
        }
        else if (Disk->Style == DM_STYLE_RAW)
            StringCchCopyW(Text->Line2, ARRAYSIZE(Text->Line2), L"Unknown");
        else
            StringCchCopyW(Text->Line2, ARRAYSIZE(Text->Line2), Disk->Dynamic ? L"Dynamic" : L"Basic");
    }

    if (!Disk->HasMedia && Disk->Device != DM_DEV_DISK)
    {
        StringCchCopyW(Text->Line3, ARRAYSIZE(Text->Line3), L"No Media");
        return;
    }
    DmFormatSizeMB(Disk->CapacityMB, Text->Line3, ARRAYSIZE(Text->Line3));
    StringCchCopyW(Text->Line4, ARRAYSIZE(Text->Line4), Disk->StatusText);
}

int
DmCompareRowNames(const DM_ROW *a, const DM_ROW *b)
{
    return _wcsicmp(a->Name, b->Name);
}

PDM_ROW
DmBuildVolumeRows(const DM_MODEL *Model, PULONG Count)
{
    PDM_ROW Rows, Sorted;
    ULONG Total = 0, n = 0, d, r, i, j;

    *Count = 0;
    for (d = 0; d < Model->DiskCount; d++)
        for (r = 0; r < Model->Disks[d]->RegionCount; r++)
            if ((Model->Disks[d]->Regions[r].Kind == DM_KIND_PRIMARY || Model->Disks[d]->Regions[r].Kind == DM_KIND_LOGICAL) &&
                !(Model->Disks[d]->Regions[r].IsDynamic && !Model->Disks[d]->Regions[r].HasVolume))
                Total++;
    if (Total == 0)
        return NULL;

    Rows = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Total * sizeof(DM_ROW));
    Sorted = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Total * sizeof(DM_ROW));
    if (Rows == NULL || Sorted == NULL)
    {
        if (Rows)
            HeapFree(GetProcessHeap(), 0, Rows);
        if (Sorted)
            HeapFree(GetProcessHeap(), 0, Sorted);
        return NULL;
    }

    for (d = 0; d < Model->DiskCount; d++)
    {
        PDM_DISK Disk = Model->Disks[d];

        for (r = 0; r < Disk->RegionCount; r++)
        {
            PDM_REGION Region = &Disk->Regions[r];
            PDM_ROW Row = &Rows[n];
            ULONGLONG Capacity, Free;

            if (Region->Kind != DM_KIND_PRIMARY && Region->Kind != DM_KIND_LOGICAL)
                continue;
            /* the volumes of a dynamic disk live inside its container partition, which is not a volume */
            if (Region->IsDynamic && !Region->HasVolume)
                continue;
            n++;

            Row->Disk = Disk;
            Row->Region = Region;
            DmRegionName(Region, Row->Name, ARRAYSIZE(Row->Name));
            StringCchCopyW(Row->Layout, ARRAYSIZE(Row->Layout), L"Simple");
            StringCchCopyW(Row->Type, ARRAYSIZE(Row->Type), Disk->Dynamic ? L"Dynamic" : L"Basic");
            StringCchCopyW(Row->FileSystem, ARRAYSIZE(Row->FileSystem), Region->HasVolume ? Region->FileSystem : L"");
            StringCchCopyW(Row->Status, ARRAYSIZE(Row->Status), Region->Status);

            if (Region->HasSpace)
            {
                Capacity = Region->VolTotal;
                Free = Region->VolFree;
            }
            else
            {
                Capacity = Region->Length;
                Free = Region->Length;
            }
            Row->CapacityBytes = Capacity;
            Row->FreeBytes = Free;
            DmFormatSize(Capacity, Row->Capacity, ARRAYSIZE(Row->Capacity));
            DmFormatSize(Free, Row->Free, ARRAYSIZE(Row->Free));
            DmFormatPercent(Free, Capacity, Row->PercentFree, ARRAYSIZE(Row->PercentFree));
            StringCchCopyW(Row->FaultTolerance, ARRAYSIZE(Row->FaultTolerance), L"No");
            StringCchCopyW(Row->Overhead, ARRAYSIZE(Row->Overhead), L"0%");
        }
    }

    /* by name, equal names keep the disk order */
    for (i = 0; i < n; i++)
    {
        for (j = i; j > 0 && DmCompareRowNames(&Rows[i], &Sorted[j - 1]) < 0; j--)
            Sorted[j] = Sorted[j - 1];
        Sorted[j] = Rows[i];
    }
    HeapFree(GetProcessHeap(), 0, Rows);
    *Count = n;
    return Sorted;
}

PDM_REGION
DmFindRegion(const DM_MODEL *Model, DM_DEVICE Device, ULONG Number, ULONGLONG Offset, PDM_DISK *DiskOut)
{
    PDM_DISK Disk = DmFindDisk(Model, Device, Number);
    ULONG i;

    if (DiskOut)
        *DiskOut = Disk;
    if (Disk == NULL)
        return NULL;
    for (i = 0; i < Disk->RegionCount; i++)
        if (Disk->Regions[i].Offset == Offset)
            return &Disk->Regions[i];
    return NULL;
}

/* --------------------------------------------------------------------- dump */

static VOID
DmPut(HANDLE File, PCWSTR Format, ...)
{
    WCHAR Wide[1024];
    CHAR Narrow[3200];
    va_list Args;
    int Len;
    DWORD Written;

    va_start(Args, Format);
    StringCchVPrintfW(Wide, ARRAYSIZE(Wide), Format, Args);
    va_end(Args);
    Len = WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Narrow, sizeof(Narrow), NULL, NULL);
    if (Len > 1)
        WriteFile(File, Narrow, Len - 1, &Written, NULL);
}

static PCWSTR
DmKindName(DM_KIND Kind)
{
    switch (Kind)
    {
        case DM_KIND_UNALLOCATED: return L"unallocated";
        case DM_KIND_PRIMARY:     return L"primary";
        case DM_KIND_LOGICAL:     return L"logical";
        case DM_KIND_FREE:        return L"free";
    }
    return L"?";
}

BOOL
DmDumpModel(const DM_MODEL *Model, HANDLE File)
{
    ULONG d, r, Count, i;
    PDM_ROW Rows;

    for (d = 0; d < Model->DiskCount; d++)
    {
        PDM_DISK Disk = Model->Disks[d];
        DM_LABEL_TEXT Label;

        DmLabelText(Disk, &Label);
        DmPut(File, L"DISK %u device=%s style=%s type=%s length=%I64u sector=%u capacityMB=%I64u unallocatedMB=%I64u reservedMB=%I64u status=\"%s\" bus=%s name=\"%s\" scsi=%d,%d,%d\r\n",
              Disk->Number,
              Disk->Device == DM_DEV_CDROM ? L"cdrom" : Disk->Device == DM_DEV_REMOVABLE ? L"removable" : L"disk",
              Disk->Style == DM_STYLE_GPT ? L"GPT" : Disk->Style == DM_STYLE_MBR ? L"MBR" : L"RAW",
              Disk->Dynamic ? L"Dynamic" : Disk->Style == DM_STYLE_RAW ? L"Unknown" : L"Basic",
              Disk->Length, Disk->BytesPerSector, Disk->CapacityMB, Disk->UnallocatedMB, Disk->ReservedMB,
              Disk->StatusText, Disk->BusText, Disk->DeviceName, Disk->ScsiPath, Disk->ScsiTarget, Disk->ScsiLun);
        DmPut(File, L"  LABEL \"%s\" | \"%s\" | \"%s\" | \"%s\"\r\n", Label.Line1, Label.Line2, Label.Line3, Label.Line4);
        for (r = 0; r < Disk->RegionCount; r++)
        {
            PDM_REGION Region = &Disk->Regions[r];
            DM_BLOCK_TEXT Block;

            DmBlockText(Disk, Region, &Block);
            DmPut(File, L"  REGION %s number=%u offset=%I64u length=%I64u extended=%d active=%d mbrtype=0x%02X name=\"%s\" size=\"%s\" status=\"%s\"",
                  DmKindName(Region->Kind), Region->PartitionNumber, Region->Offset, Region->Length,
                  Region->InExtended, Region->Active, Region->MbrType, Block.Line1, Block.Line2, Block.Line3);
            if (Region->HasVolume)
                DmPut(File, L" letter=%s label=\"%s\" fs=\"%s\" total=%I64u free=%I64u hasspace=%d",
                      Region->Letter, Region->Label, Region->FileSystem, Region->VolTotal, Region->VolFree, Region->HasSpace);
            DmPut(File, L"\r\n");
        }
    }

    Rows = DmBuildVolumeRows(Model, &Count);
    for (i = 0; i < Count; i++)
    {
        DmPut(File, L"ROW \"%s\" | %s | %s | %s | %s | %s | %s | %s | %s | %s\r\n",
              Rows[i].Name, Rows[i].Layout, Rows[i].Type, Rows[i].FileSystem, Rows[i].Status,
              Rows[i].Capacity, Rows[i].Free, Rows[i].PercentFree, Rows[i].FaultTolerance, Rows[i].Overhead);
    }
    if (Rows)
        HeapFree(GetProcessHeap(), 0, Rows);
    return TRUE;
}
