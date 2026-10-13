/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the disk, partition and volume model of Disk Management
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values of the synthetic layouts were read from Disk Management on Windows Server 2008 R2
 * for the same layouts (two disks created with sfdisk and sgdisk and attached to the VM).
 */

#include <apitest.h>
#include <wchar.h>
#include <wctype.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <strsafe.h>

#include "diskmodel.h"

#define MiB (1024ULL * 1024)

typedef struct
{
    ULONGLONG Offset;
    ULONGLONG Length;
    UCHAR Type;
    BOOLEAN Boot;
    ULONG Number;
} MBR_PART;

static const GUID GuidEsp      = { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
static const GUID GuidMsr      = { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } };
static const GUID GuidBasic    = { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } };

static PDRIVE_LAYOUT_INFORMATION_EX
AllocLayout(ULONG Entries)
{
    SIZE_T Size = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + Entries * sizeof(PARTITION_INFORMATION_EX);
    PDRIVE_LAYOUT_INFORMATION_EX Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);

    Layout->PartitionCount = Entries;
    return Layout;
}

static PDRIVE_LAYOUT_INFORMATION_EX
MakeMbrLayout(const MBR_PART *Parts, ULONG Count, ULONG Unused)
{
    PDRIVE_LAYOUT_INFORMATION_EX Layout = AllocLayout(Count + Unused);
    ULONG i;

    Layout->PartitionStyle = PARTITION_STYLE_MBR;
    Layout->Mbr.Signature = 0x5A1C0DE1;
    for (i = 0; i < Count; i++)
    {
        PARTITION_INFORMATION_EX *Entry = &Layout->PartitionEntry[i];

        Entry->PartitionStyle = PARTITION_STYLE_MBR;
        Entry->StartingOffset.QuadPart = Parts[i].Offset;
        Entry->PartitionLength.QuadPart = Parts[i].Length;
        Entry->PartitionNumber = Parts[i].Number;
        Entry->RewritePartition = FALSE;
        Entry->Mbr.PartitionType = Parts[i].Type;
        Entry->Mbr.BootIndicator = Parts[i].Boot;
        Entry->Mbr.RecognizedPartition = TRUE;
    }
    /* the driver lists unused slots of the partition table as well */
    for (i = Count; i < Count + Unused; i++)
        Layout->PartitionEntry[i].PartitionStyle = PARTITION_STYLE_MBR;
    return Layout;
}

static VOID
SetGpt(PARTITION_INFORMATION_EX *Entry, ULONGLONG Offset, ULONGLONG Length, const GUID *Type, ULONG Number)
{
    Entry->PartitionStyle = PARTITION_STYLE_GPT;
    Entry->StartingOffset.QuadPart = Offset;
    Entry->PartitionLength.QuadPart = Length;
    Entry->PartitionNumber = Number;
    Entry->Gpt.PartitionType = *Type;
    Entry->Gpt.PartitionId = *Type;
    Entry->Gpt.PartitionId.Data1 ^= 0x1000 + Number;
    StringCchCopyW(Entry->Gpt.Name, ARRAYSIZE(Entry->Gpt.Name), L"Basic data partition");
}

static PDM_DISK
BuildDisk(DM_DEVICE Device, ULONG Number, ULONGLONG Length, const DRIVE_LAYOUT_INFORMATION_EX *Layout,
          BOOL Removable, BOOL HasMedia, ULONG LayoutError)
{
    DM_DISK_INPUT Input;
    PDM_DISK Disk = NULL;
    BOOL Ok;

    ZeroMemory(&Input, sizeof(Input));
    Input.Device = Device;
    Input.Number = Number;
    Input.BytesPerSector = 512;
    Input.DiskLength = Length;
    Input.Removable = Removable;
    Input.HasMedia = HasMedia;
    Input.LayoutError = LayoutError;
    Input.Layout = Layout;
    Input.BusType = BusTypeScsi;
    Input.ScsiPath = Input.ScsiTarget = Input.ScsiLun = -1;
    StringCchCopyW(Input.DeviceName, ARRAYSIZE(Input.DeviceName), L"Test Disk");
    Ok = DmBuildDisk(&Input, &Disk);
    ok(Ok, "DmBuildDisk failed\n");
    return Disk;
}

static PDM_MODEL
ModelOf(PDM_DISK Disk)
{
    PDM_MODEL Model = DmCreateModel();

    ok(Model != NULL, "DmCreateModel failed\n");
    ok(DmAddDisk(Model, Disk), "DmAddDisk failed\n");
    return Model;
}

static VOID
CheckRegion(const DM_DISK *Disk, ULONG Index, DM_KIND Kind, BOOL InExtended, ULONGLONG OffsetMiB, ULONGLONG LengthMiB, ULONG Number)
{
    const DM_REGION *Region;

    ok(Index < Disk->RegionCount, "region %lu does not exist (count %lu)\n", Index, Disk->RegionCount);
    if (Index >= Disk->RegionCount)
        return;
    Region = &Disk->Regions[Index];
    ok(Region->Kind == Kind, "region %lu: kind %d, expected %d\n", Index, Region->Kind, Kind);
    ok(Region->InExtended == InExtended, "region %lu: extended %d, expected %d\n", Index, Region->InExtended, InExtended);
    ok(Region->Offset == OffsetMiB * MiB, "region %lu: offset %I64u MiB, expected %I64u\n", Index, Region->Offset / MiB, OffsetMiB);
    ok(Region->Length == LengthMiB * MiB, "region %lu: length %I64u MiB, expected %I64u\n", Index, Region->Length / MiB, LengthMiB);
    ok(Region->PartitionNumber == Number, "region %lu: number %lu, expected %lu\n", Index, Region->PartitionNumber, Number);
}

static VOID
CheckText(PCWSTR Actual, PCWSTR Expected, PCSTR What)
{
    ok(wcscmp(Actual, Expected) == 0, "%s: '%ls', expected '%ls'\n", What, Actual, Expected);
}

static VOID
Test_Format(VOID)
{
    WCHAR Text[64];

    DmFormatSize(1071624192ULL, Text, ARRAYSIZE(Text));
    CheckText(Text, L"1022 MB", "FAT32 volume of a 1 GiB partition");
    DmFormatSize(1024 * MiB, Text, ARRAYSIZE(Text));
    CheckText(Text, L"1.00 GB", "1 GiB");
    DmFormatSize(512 * MiB, Text, ARRAYSIZE(Text));
    CheckText(Text, L"512 MB", "512 MiB");
    DmFormatSize(1534 * MiB, Text, ARRAYSIZE(Text));
    CheckText(Text, L"1.50 GB", "free space of the extended partition");
    DmFormatSize(100 * MiB, Text, ARRAYSIZE(Text));
    CheckText(Text, L"100 MB", "100 MiB");
    DmFormatSizeMB(5104, Text, ARRAYSIZE(Text));
    CheckText(Text, L"4.98 GB", "GPT disk capacity");
    DmFormatSizeMB(6143, Text, ARRAYSIZE(Text));
    CheckText(Text, L"6.00 GB", "MBR disk capacity");
    DmFormatSizeMB(65535, Text, ARRAYSIZE(Text));
    CheckText(Text, L"64.00 GB", "64 GiB disk capacity");
    DmFormatSize(1024ULL * 1024 * MiB, Text, ARRAYSIZE(Text));
    CheckText(Text, L"1.00 TB", "1 TiB");
    DmFormatSize(300 * 1024, Text, ARRAYSIZE(Text));
    CheckText(Text, L"300 KB", "300 KiB");
    DmFormatSize(0, Text, ARRAYSIZE(Text));
    CheckText(Text, L"0 MB", "nothing, as in the free space of a disc");

    DmFormatPercent(966762496ULL, 1071624192ULL, Text, ARRAYSIZE(Text));
    CheckText(Text, L"90 %", "922 of 1022 MB");
    DmFormatPercent(1024 * MiB, 1024 * MiB, Text, ARRAYSIZE(Text));
    CheckText(Text, L"100 %", "all free");
    DmFormatPercent(40750, 63900, Text, ARRAYSIZE(Text));
    CheckText(Text, L"64 %", "40.75 of 63.90 GB");
    DmFormatPercent(0, 0, Text, ARRAYSIZE(Text));
    CheckText(Text, L"0 %", "empty");
}

/* A 6 GiB MBR disk: two primaries, an extended partition with two logical drives and free space,
 * and unallocated space at the end. */
static const MBR_PART MbrDisk[] =
{
    { 1 * MiB,    1024 * MiB, 0x0C, TRUE,  1 },
    { 1025 * MiB, 1024 * MiB, 0x83, FALSE, 2 },
    { 2049 * MiB, 3072 * MiB, 0x05, FALSE, 0 },
    { 2050 * MiB, 1024 * MiB, 0x0C, FALSE, 3 },
    { 3075 * MiB, 512 * MiB,  0x0E, FALSE, 4 },
};

static VOID
Test_MbrLayout(VOID)
{
    PDRIVE_LAYOUT_INFORMATION_EX Layout = MakeMbrLayout(MbrDisk, ARRAYSIZE(MbrDisk), 3);
    PDM_DISK Disk = BuildDisk(DM_DEV_DISK, 1, 6144 * MiB, Layout, FALSE, TRUE, 0);
    DM_BLOCK_TEXT Text;
    DM_LABEL_TEXT Label;

    ok(Disk != NULL, "no disk\n");
    if (Disk == NULL)
        return;

    ok(Disk->Style == DM_STYLE_MBR, "style %d\n", Disk->Style);
    ok(!Disk->Dynamic, "dynamic\n");
    ok(Disk->CapacityMB == 6143, "capacity %I64u MB, expected 6143\n", Disk->CapacityMB);
    ok(Disk->UnallocatedMB == 2559, "unallocated %I64u MB, expected 2559\n", Disk->UnallocatedMB);
    ok(Disk->ReservedMB == 0, "reserved %I64u MB\n", Disk->ReservedMB);
    ok(Disk->RegionCount == 6, "region count %lu, expected 6\n", Disk->RegionCount);

    CheckRegion(Disk, 0, DM_KIND_PRIMARY, FALSE, 1, 1024, 1);
    CheckRegion(Disk, 1, DM_KIND_PRIMARY, FALSE, 1025, 1024, 2);
    CheckRegion(Disk, 2, DM_KIND_LOGICAL, TRUE, 2050, 1024, 3);
    CheckRegion(Disk, 3, DM_KIND_LOGICAL, TRUE, 3075, 512, 4);
    CheckRegion(Disk, 4, DM_KIND_FREE, TRUE, 3587, 1534, 0);
    CheckRegion(Disk, 5, DM_KIND_UNALLOCATED, FALSE, 5121, 1022, 0);
    if (Disk->RegionCount == 6)
        ok(Disk->Regions[0].Active && !Disk->Regions[1].Active, "active flags\n");

    DmLabelText(Disk, &Label);
    CheckText(Label.Line1, L"Disk 1", "label 1");
    CheckText(Label.Line2, L"Basic", "label 2");
    CheckText(Label.Line3, L"6.00 GB", "label 3");
    CheckText(Label.Line4, L"Online", "label 4");

    if (Disk->RegionCount == 6)
    {
        DmBlockText(Disk, &Disk->Regions[4], &Text);
        CheckText(Text.Line1, L"", "free line 1");
        CheckText(Text.Line2, L"1.50 GB", "free line 2");
        CheckText(Text.Line3, L"Free space", "free line 3");
        DmBlockText(Disk, &Disk->Regions[5], &Text);
        CheckText(Text.Line2, L"1022 MB", "unallocated line 2");
        CheckText(Text.Line3, L"Unallocated", "unallocated line 3");
    }

    DmFreeModel(ModelOf(Disk));
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_GptLayout(VOID)
{
    PDRIVE_LAYOUT_INFORMATION_EX Layout = AllocLayout(4);
    PDM_DISK Disk;
    DM_LABEL_TEXT Label;
    DM_BLOCK_TEXT Text;
    PDM_MODEL Model;
    DM_SYSINFO Info;
    ULONG Count;
    PDM_ROW Rows;

    Layout->PartitionStyle = PARTITION_STYLE_GPT;
    Layout->Gpt.StartingUsableOffset.QuadPart = 34 * 512;
    Layout->Gpt.UsableLength.QuadPart = (10485726ULL - 34 + 1) * 512;
    Layout->Gpt.MaxPartitionCount = 128;
    SetGpt(&Layout->PartitionEntry[0], 1 * MiB, 100 * MiB, &GuidEsp, 1);
    SetGpt(&Layout->PartitionEntry[1], 101 * MiB, 16 * MiB, &GuidMsr, 2);
    SetGpt(&Layout->PartitionEntry[2], 117 * MiB, 1024 * MiB, &GuidBasic, 3);
    SetGpt(&Layout->PartitionEntry[3], 1141 * MiB, 1024 * MiB, &GuidBasic, 4);

    Disk = BuildDisk(DM_DEV_DISK, 2, 5120 * MiB, Layout, FALSE, TRUE, 0);
    ok(Disk != NULL, "no disk\n");
    if (Disk == NULL)
        return;

    ok(Disk->Style == DM_STYLE_GPT, "style %d\n", Disk->Style);
    ok(Disk->ReservedMB == 16, "reserved %I64u MB, expected 16\n", Disk->ReservedMB);
    ok(Disk->CapacityMB == 5104, "capacity %I64u MB, expected 5104\n", Disk->CapacityMB);
    ok(Disk->UnallocatedMB == 2956, "unallocated %I64u MB, expected 2956\n", Disk->UnallocatedMB);
    ok(Disk->RegionCount == 4, "region count %lu, expected 4 (the reserved partition is not drawn)\n", Disk->RegionCount);
    CheckRegion(Disk, 0, DM_KIND_PRIMARY, FALSE, 1, 100, 1);
    CheckRegion(Disk, 1, DM_KIND_PRIMARY, FALSE, 117, 1024, 3);
    CheckRegion(Disk, 2, DM_KIND_PRIMARY, FALSE, 1141, 1024, 4);
    CheckRegion(Disk, 3, DM_KIND_UNALLOCATED, FALSE, 2165, 2955, 0);
    if (Disk->RegionCount == 4)
        ok(Disk->Regions[0].IsEfi && !Disk->Regions[1].IsEfi, "EFI flags\n");

    Model = ModelOf(Disk);
    ZeroMemory(&Info, sizeof(Info));
    DmFinishModel(Model, &Info);
    DmLabelText(Disk, &Label);
    CheckText(Label.Line3, L"4.98 GB", "label size");
    if (Disk->RegionCount == 4)
    {
        DmBlockText(Disk, &Disk->Regions[0], &Text);
        CheckText(Text.Line2, L"100 MB", "ESP size");
        CheckText(Text.Line3, L"Healthy (EFI System Partition)", "ESP status");
        DmBlockText(Disk, &Disk->Regions[1], &Text);
        CheckText(Text.Line3, L"Healthy (Primary Partition)", "data status");
        DmBlockText(Disk, &Disk->Regions[3], &Text);
        CheckText(Text.Line2, L"2.89 GB", "unallocated size");
    }

    Rows = DmBuildVolumeRows(Model, &Count);
    ok(Count == 3, "row count %lu, expected 3\n", Count);
    if (Rows)
        HeapFree(GetProcessHeap(), 0, Rows);
    DmFreeModel(Model);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
MakeVolume(PDM_VOLUME_INFO Volume, ULONG Disk, ULONG Partition, PCWSTR Letter, PCWSTR Label, PCWSTR Fs,
           ULONGLONG Total, ULONGLONG Free)
{
    ZeroMemory(Volume, sizeof(*Volume));
    Volume->HaveDeviceNumber = TRUE;
    Volume->DeviceType = FILE_DEVICE_DISK;
    Volume->DeviceNumber = Disk;
    Volume->PartitionNumber = Partition;
    Volume->DriveType = DRIVE_FIXED;
    StringCchCopyW(Volume->Guid, ARRAYSIZE(Volume->Guid), L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\");
    StringCchCopyW(Volume->Letter, ARRAYSIZE(Volume->Letter), Letter);
    if (Fs)
    {
        Volume->HasFsInfo = TRUE;
        StringCchCopyW(Volume->Label, ARRAYSIZE(Volume->Label), Label);
        StringCchCopyW(Volume->FileSystem, ARRAYSIZE(Volume->FileSystem), Fs);
        Volume->HasSpace = TRUE;
        Volume->Total = Total;
        Volume->Free = Free;
    }
    Volume->HasMedia = TRUE;
}

static VOID
Test_VolumeList(VOID)
{
    PDRIVE_LAYOUT_INFORMATION_EX Layout = MakeMbrLayout(MbrDisk, ARRAYSIZE(MbrDisk), 3);
    PDM_DISK Disk = BuildDisk(DM_DEV_DISK, 1, 6144 * MiB, Layout, FALSE, TRUE, 0);
    PDM_MODEL Model = ModelOf(Disk);
    DM_VOLUME_INFO Volume;
    DM_SYSINFO Info;
    DM_BLOCK_TEXT Text;
    PDM_ROW Rows;
    ULONG Count, i;

    MakeVolume(&Volume, 1, 1, L"F:", L"PRIMFAT", L"FAT32", 1071624192ULL, 966762496ULL);
    ok(DmAttachVolume(Model, &Volume), "attach F:\n");
    MakeVolume(&Volume, 1, 3, L"G:", L"LOGIC1", L"FAT32", 1071624192ULL, 1040187392ULL);
    ok(DmAttachVolume(Model, &Volume), "attach G:\n");
    MakeVolume(&Volume, 1, 4, L"H:", L"LOGIC2", L"FAT", 536608768ULL, 531628032ULL);
    ok(DmAttachVolume(Model, &Volume), "attach H:\n");
    MakeVolume(&Volume, 1, 9, L"Z:", L"NOPE", L"NTFS", 1, 1);
    ok(!DmAttachVolume(Model, &Volume), "a volume of a partition that does not exist was attached\n");
    MakeVolume(&Volume, 7, 1, L"Y:", L"NODISK", L"NTFS", 1, 1);
    ok(!DmAttachVolume(Model, &Volume), "a volume of a disk that does not exist was attached\n");
    MakeVolume(&Volume, 1, 1, L"X:", L"AGAIN", L"NTFS", 1, 1);
    ok(!DmAttachVolume(Model, &Volume), "a second volume was attached to the same partition\n");

    ZeroMemory(&Info, sizeof(Info));
    Info.HaveBootDevice = TRUE;
    Info.BootDisk = 1;
    Info.BootPartition = 1;
    Info.PagefileLetters = 1UL << ('F' - 'A');
    Info.CrashDumpEnabled = TRUE;
    DmFinishModel(Model, &Info);

    CheckText(Disk->Regions[0].Status, L"Healthy (System, Boot, Page File, Active, Crash Dump, Primary Partition)", "boot partition");
    CheckText(Disk->Regions[1].Status, L"Healthy (Primary Partition)", "unformatted primary");
    CheckText(Disk->Regions[2].Status, L"Healthy (Logical Drive)", "logical drive");

    DmBlockText(Disk, &Disk->Regions[0], &Text);
    CheckText(Text.Line1, L"PRIMFAT (F:)", "block name");
    CheckText(Text.Line2, L"1.00 GB FAT32", "block size and file system");
    DmBlockText(Disk, &Disk->Regions[3], &Text);
    CheckText(Text.Line1, L"LOGIC2 (H:)", "second logical name");
    CheckText(Text.Line2, L"512 MB FAT", "second logical size");
    DmBlockText(Disk, &Disk->Regions[1], &Text);
    CheckText(Text.Line1, L"", "unformatted name");
    CheckText(Text.Line2, L"1.00 GB", "unformatted size");

    Rows = DmBuildVolumeRows(Model, &Count);
    ok(Count == 4, "row count %lu, expected 4\n", Count);
    if (Rows && Count == 4)
    {
        /* by name; the unnamed partition first */
        CheckText(Rows[0].Name, L"", "row 0 name");
        CheckText(Rows[1].Name, L"LOGIC1 (G:)", "row 1 name");
        CheckText(Rows[2].Name, L"LOGIC2 (H:)", "row 2 name");
        CheckText(Rows[3].Name, L"PRIMFAT (F:)", "row 3 name");

        CheckText(Rows[0].Layout, L"Simple", "layout");
        CheckText(Rows[0].Type, L"Basic", "type");
        CheckText(Rows[0].FileSystem, L"", "file system of the unformatted partition");
        CheckText(Rows[0].Capacity, L"1.00 GB", "capacity of the unformatted partition");
        CheckText(Rows[0].Free, L"1.00 GB", "free space of the unformatted partition");
        CheckText(Rows[0].PercentFree, L"100 %", "free percent of the unformatted partition");

        CheckText(Rows[1].Capacity, L"1022 MB", "LOGIC1 capacity");
        CheckText(Rows[1].Free, L"992 MB", "LOGIC1 free");
        CheckText(Rows[1].PercentFree, L"97 %", "LOGIC1 percent");
        CheckText(Rows[2].FileSystem, L"FAT", "LOGIC2 file system");
        CheckText(Rows[2].Capacity, L"512 MB", "LOGIC2 capacity");
        CheckText(Rows[2].Free, L"507 MB", "LOGIC2 free");
        CheckText(Rows[2].PercentFree, L"99 %", "LOGIC2 percent");
        CheckText(Rows[3].Free, L"922 MB", "PRIMFAT free");
        CheckText(Rows[3].PercentFree, L"90 %", "PRIMFAT percent");
        CheckText(Rows[3].FaultTolerance, L"No", "fault tolerance");
        CheckText(Rows[3].Overhead, L"0%", "overhead");
    }
    for (i = 0; Rows && i < Count; i++)
        ok(Rows[i].Region != NULL && Rows[i].Disk == Disk, "row %lu is not tied to its block\n", i);
    if (Rows)
        HeapFree(GetProcessHeap(), 0, Rows);

    ok(DmFindRegion(Model, DM_DEV_DISK, 1, 3075 * MiB, NULL) == &Disk->Regions[3], "DmFindRegion\n");
    ok(DmFindRegion(Model, DM_DEV_DISK, 1, 5 * MiB, NULL) == NULL, "DmFindRegion on a missing offset\n");
    ok(DmFindRegion(Model, DM_DEV_DISK, 2, 1 * MiB, NULL) == NULL, "DmFindRegion on a missing disk\n");
    DmFreeModel(Model);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_OtherDisks(VOID)
{
    PDRIVE_LAYOUT_INFORMATION_EX Layout;
    PDM_DISK Disk;
    DM_LABEL_TEXT Label;
    PDM_MODEL Model;
    DM_VOLUME_INFO Volume;
    DM_BLOCK_TEXT Text;

    /* a disk that was never initialised */
    Layout = AllocLayout(0);
    Layout->PartitionStyle = PARTITION_STYLE_RAW;
    Disk = BuildDisk(DM_DEV_DISK, 3, 2048 * MiB, Layout, FALSE, TRUE, 0);
    ok(Disk != NULL, "no disk\n");
    if (Disk)
    {
        ok(Disk->Style == DM_STYLE_RAW, "style %d\n", Disk->Style);
        ok(Disk->RegionCount == 1 && Disk->Regions[0].Kind == DM_KIND_UNALLOCATED && Disk->Regions[0].Length == 2048 * MiB,
           "a raw disk is one block of unallocated space\n");
        DmLabelText(Disk, &Label);
        CheckText(Label.Line2, L"Unknown", "raw disk type");
        CheckText(Label.Line3, L"2.00 GB", "raw disk size");
        CheckText(Label.Line4, L"Not Initialized", "raw disk status");
        DmFreeModel(ModelOf(Disk));
    }
    HeapFree(GetProcessHeap(), 0, Layout);

    /* a card reader without a card */
    Disk = BuildDisk(DM_DEV_REMOVABLE, 4, 0, NULL, TRUE, FALSE, ERROR_NOT_READY);
    ok(Disk != NULL, "no disk\n");
    if (Disk)
    {
        DmLabelText(Disk, &Label);
        CheckText(Label.Line2, L"Removable", "removable type");
        CheckText(Label.Line3, L"No Media", "removable without media");
        ok(Disk->RegionCount == 0, "block count %lu\n", Disk->RegionCount);
        DmFreeModel(ModelOf(Disk));
    }

    /* a medium formatted without a partition table */
    Layout = AllocLayout(0);
    Layout->PartitionStyle = PARTITION_STYLE_RAW;
    Disk = BuildDisk(DM_DEV_REMOVABLE, 5, 1024 * MiB, Layout, TRUE, TRUE, 0);
    Model = ModelOf(Disk);
    MakeVolume(&Volume, 5, 0, L"E:", L"STICK", L"FAT32", 1000 * MiB, 900 * MiB);
    ok(DmAttachVolume(Model, &Volume), "attach the volume of a medium without partitions\n");
    DmFinishModel(Model, NULL);
    ok(Disk->UnallocatedMB == 0, "a medium that is one volume has %I64u MB unallocated\n", Disk->UnallocatedMB);
    DmLabelText(Disk, &Label);
    CheckText(Label.Line2, L"Removable (E:)", "removable type with letter");
    if (Disk->RegionCount == 1)
    {
        DmBlockText(Disk, &Disk->Regions[0], &Text);
        CheckText(Text.Line1, L"STICK (E:)", "whole-disk volume name");
        CheckText(Text.Line3, L"Healthy (Primary Partition)", "whole-disk volume status");
    }
    DmFreeModel(Model);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* a dynamic disk: the container partition is no volume */
    {
        static const MBR_PART Dynamic[] = { { 1 * MiB, 2000 * MiB, 0x42, FALSE, 1 } };
        ULONG Count;
        PDM_ROW Rows;

        Layout = MakeMbrLayout(Dynamic, 1, 3);
        Disk = BuildDisk(DM_DEV_DISK, 6, 2048 * MiB, Layout, FALSE, TRUE, 0);
        ok(Disk != NULL && Disk->Dynamic, "the 0x42 partition does not make a dynamic disk\n");
        if (Disk)
        {
            DmLabelText(Disk, &Label);
            CheckText(Label.Line2, L"Dynamic", "dynamic disk type");
            Model = ModelOf(Disk);
            DmFinishModel(Model, NULL);
            Rows = DmBuildVolumeRows(Model, &Count);
            ok(Count == 0, "%lu volume rows for a dynamic container\n", Count);
            if (Rows)
                HeapFree(GetProcessHeap(), 0, Rows);
            DmFreeModel(Model);
        }
        HeapFree(GetProcessHeap(), 0, Layout);
    }

    /* an optical drive with a disc, and one without */
    Model = DmCreateModel();
    ZeroMemory(&Volume, sizeof(Volume));
    Volume.HaveDeviceNumber = TRUE;
    Volume.DeviceType = FILE_DEVICE_CD_ROM;
    Volume.DeviceNumber = 0;
    Volume.DriveType = DRIVE_CDROM;
    StringCchCopyW(Volume.Letter, ARRAYSIZE(Volume.Letter), L"D:");
    StringCchCopyW(Volume.Label, ARRAYSIZE(Volume.Label), L"AJARCD");
    StringCchCopyW(Volume.FileSystem, ARRAYSIZE(Volume.FileSystem), L"CDFS");
    StringCchCopyW(Volume.MediaKind, ARRAYSIZE(Volume.MediaKind), L"DVD");
    Volume.HasFsInfo = Volume.HasSpace = Volume.HasMedia = TRUE;
    Volume.Total = 20 * MiB;
    ok(DmAttachVolume(Model, &Volume), "attach a disc\n");
    ZeroMemory(&Volume, sizeof(Volume));
    Volume.HaveDeviceNumber = TRUE;
    Volume.DeviceType = FILE_DEVICE_CD_ROM;
    Volume.DeviceNumber = 1;
    Volume.DriveType = DRIVE_CDROM;
    StringCchCopyW(Volume.Letter, ARRAYSIZE(Volume.Letter), L"E:");
    ok(DmAttachVolume(Model, &Volume), "attach an empty drive\n");
    DmFinishModel(Model, NULL);
    ok(Model->DiskCount == 2, "disc count %lu\n", Model->DiskCount);
    if (Model->DiskCount == 2)
    {
        DmLabelText(Model->Disks[0], &Label);
        CheckText(Label.Line1, L"CD-ROM 0", "optical label 1");
        CheckText(Label.Line2, L"DVD", "optical label 2 (the letter is on the block of the disc)");
        CheckText(Label.Line3, L"20 MB", "optical label 3");
        ok(Model->Disks[0]->RegionCount == 1, "disc block count\n");
        if (Model->Disks[0]->RegionCount == 1)
        {
            DmBlockText(Model->Disks[0], &Model->Disks[0]->Regions[0], &Text);
            CheckText(Text.Line1, L"AJARCD (D:)", "disc name");
            CheckText(Text.Line2, L"20 MB CDFS", "disc size");
            CheckText(Text.Line3, L"Healthy (Primary Partition)", "disc status");
        }
        DmLabelText(Model->Disks[1], &Label);
        CheckText(Label.Line1, L"CD-ROM 1", "empty drive label 1");
        CheckText(Label.Line2, L"CD-ROM (E:)", "empty drive label 2");
        CheckText(Label.Line3, L"No Media", "empty drive label 3");
        ok(Model->Disks[1]->RegionCount == 0, "empty drive block count\n");
    }
    DmFreeModel(Model);
}

static VOID
Test_Live(VOID)
{
    PDM_MODEL Model = DmCollect();
    WCHAR Windows[MAX_PATH];
    ULONG d, r, Boots = 0;
    BOOL FoundWindowsDrive = FALSE;

    ok(Model != NULL, "DmCollect failed\n");
    if (Model == NULL)
        return;
    GetWindowsDirectoryW(Windows, ARRAYSIZE(Windows));

    for (d = 0; d < Model->DiskCount; d++)
    {
        const DM_DISK *Disk = Model->Disks[d];
        ULONGLONG End = 0;

        if (Disk->Device == DM_DEV_DISK)
            ok(Disk->CapacityMB > 0, "disk %lu has no capacity\n", Disk->Number);
        for (r = 0; r < Disk->RegionCount; r++)
        {
            const DM_REGION *Region = &Disk->Regions[r];

            ok(Region->Length > 0, "disk %lu block %lu is empty\n", Disk->Number, r);
            ok(Region->Offset >= End, "disk %lu block %lu overlaps the one before it\n", Disk->Number, r);
            End = Region->Offset + Region->Length;
            if (Disk->Device == DM_DEV_DISK)
                ok(End <= Disk->Length, "disk %lu block %lu ends beyond the disk\n", Disk->Number, r);
            if (Region->Kind == DM_KIND_PRIMARY || Region->Kind == DM_KIND_LOGICAL)
            {
                ok(wcsncmp(Region->Status, L"Healthy (", 9) == 0, "status '%ls'\n", Region->Status);
                if (Region->IsBoot)
                    Boots++;
                if (Region->Letter[0] && Windows[1] == L':' && towupper(Windows[0]) == Region->Letter[0])
                {
                    FoundWindowsDrive = TRUE;
                    ok(Region->IsBoot, "the volume of the Windows directory is not marked Boot\n");
                    ok(wcsstr(Region->Status, L"Boot") != NULL, "status '%ls' has no Boot\n", Region->Status);
                    ok(Region->HasFsInfo && Region->FileSystem[0], "no file system name for the boot volume\n");
                    ok(Region->HasSpace && Region->VolTotal > 0, "no space data for the boot volume\n");
                }
            }
        }
    }
    ok(FoundWindowsDrive, "the drive of the Windows directory is in no block\n");
    ok(Boots == 1, "%lu blocks are marked Boot, expected 1\n", Boots);
    DmFreeModel(Model);
}

static VOID
CheckFit(ULONG Count, const ULONGLONG *Lengths, ULONG Bar, ULONG Min, PCSTR What)
{
    ULONG Widths[256], i, Sum = 0, Smallest = 0xFFFFFFFF;

    DmFitBlockWidths(Lengths, Count, Bar, Min, Widths);
    for (i = 0; i < Count; i++)
    {
        Sum += Widths[i];
        if (Widths[i] < Smallest)
            Smallest = Widths[i];
    }
    ok(Sum == Bar, "%s: the widths add up to %lu, expected %lu\n", What, Sum, Bar);
    if (Count > 0 && Min > Bar / Count)
        Min = Bar / Count;
    ok(Smallest >= Min, "%s: a block is %lu wide, the minimum is %lu\n", What, Smallest, Min);
}

static VOID
Test_BusText(VOID)
{
    CheckText(DmBusText(BusTypeScsi), L"SCSI", "SCSI");
    CheckText(DmBusText(BusTypeAta), L"IDE", "ATA");
    CheckText(DmBusText(BusTypeAtapi), L"IDE", "ATAPI");
    CheckText(DmBusText(BusTypeUsb), L"USB", "USB");
    CheckText(DmBusText(BusTypeSas), L"UNKNOWN", "SAS is UNKNOWN in the disk list of Windows Server 2008 R2");
}

static VOID
Test_Fit(VOID)
{
    ULONGLONG Lengths[256];
    ULONG i, Widths[3];

    /* the disk of the lane: 100 MiB, 1 GiB, 1 GiB and 2.89 GiB give 155, 237, 237 and 275 pixels on Windows,
     * which is the log of the size in proportion; the widths here follow the same rule */
    Lengths[0] = 100 * MiB; Lengths[1] = 1024 * MiB; Lengths[2] = 1024 * MiB; Lengths[3] = 2955 * MiB;
    CheckFit(4, Lengths, 905, 24, "four blocks");
    {
        ULONG W[4];

        DmFitBlockWidths(Lengths, 4, 905, 24, W);
        ok(W[0] > 150 && W[0] < 170, "100 MiB block is %lu wide, Windows draws it 155\n", W[0]);
        ok(W[1] > 230 && W[1] < 245 && W[2] == W[1], "1 GiB blocks are %lu and %lu wide, Windows draws them 237\n", W[1], W[2]);
        ok(W[3] > 265 && W[3] < 290, "2.89 GiB block is %lu wide, Windows draws it 275\n", W[3]);
    }

    /* many equal blocks: 128 GPT partitions in 300 pixels */
    for (i = 0; i < 128; i++)
        Lengths[i] = 100 * MiB;
    CheckFit(128, Lengths, 300, 2, "128 equal blocks");

    /* more blocks than pixels: 128 partitions and 127 gaps in 200 pixels, no overflow */
    for (i = 0; i < 255; i++)
        Lengths[i] = (i & 1) ? 100 * MiB : 2 * MiB;
    CheckFit(255, Lengths, 200, 1, "255 blocks in 200 pixels");
    CheckFit(255, Lengths, 255, 1, "255 blocks in 255 pixels");
    CheckFit(255, Lengths, 0, 1, "255 blocks in no pixel");

    /* twenty huge blocks and a tiny one: the sum of the minimum raises the total, the excess comes off the wide blocks */
    for (i = 0; i < 20; i++)
        Lengths[i] = 100 * 1024 * MiB;
    Lengths[20] = 1 * MiB;
    CheckFit(21, Lengths, 751, 24, "twenty huge blocks and a tiny one");

    /* one block, and the blocks of an empty disk */
    Lengths[0] = 5 * MiB;
    DmFitBlockWidths(Lengths, 1, 400, 24, Widths);
    ok(Widths[0] == 400, "a single block is %lu wide, expected 400\n", Widths[0]);
    DmFitBlockWidths(Lengths, 0, 400, 24, Widths);
}

START_TEST(DiskModel)
{
    Test_Format();
    Test_BusText();
    Test_Fit();
    Test_MbrLayout();
    Test_GptLayout();
    Test_VolumeList();
    Test_OtherDisks();
    Test_Live();
}
