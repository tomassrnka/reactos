/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the layout arithmetic of the writer of Disk Management
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected numbers were measured with diskpart on Windows Server 2008 R2 (the same volume manager
 * as Disk Management) on blank 4 GiB and 20 GiB disks, so that the test passes on Windows as well.
 */

#include <apitest.h>
#include <wchar.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <strsafe.h>

#include "disklayout.h"

#define MiB (1024ULL * 1024)

static const GUID TestId = { 0x11111111, 0x2222, 0x3333, { 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } };

static PDL_LAYOUT
NewLayout(DM_STYLE Style, ULONGLONG Mib)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));

    DlInit(Layout, Style, Mib * MiB, 512, 0x12345678, &TestId);
    return Layout;
}

static ULONG
Free_(const DL_LAYOUT *Layout, DL_FREE *Out)
{
    ULONG Count = DlFindFree(Layout, Out, 32);

    return Count > 32 ? 32 : Count;
}

/* the gaps inside the extended partition only */
static ULONG
FreeIn(const DL_LAYOUT *Layout, DL_FREE *Out)
{
    DL_FREE All[32];
    ULONG Count = Free_(Layout, All), i, n = 0;

    for (i = 0; i < Count; i++)
        if (All[i].InExtended)
            Out[n++] = All[i];
    return n;
}

static DL_STATUS
Make(PDL_LAYOUT Layout, DL_KIND Kind, ULONGLONG OffsetMib, ULONGLONG LengthMib)
{
    return DlCreate(Layout, Kind, OffsetMib * MiB, LengthMib * MiB, 0, &DlGuidBasicData, &TestId, NULL);
}

static VOID
Test_Empty(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 4096);
    DL_FREE Free[32];
    ULONG Count;

    /* diskpart: "create partition primary" on a blank 4096 MB disk gives 4094 MB at 1024 KB */
    Count = Free_(Layout, Free);
    ok_int(Count, 1);
    ok(Free[0].Offset == 1 * MiB, "gap offset %I64u\n", Free[0].Offset);
    ok(Free[0].Length == 4094 * MiB, "gap length %I64u\n", Free[0].Length);
    ok(Free[0].DataOffset == 1 * MiB && Free[0].MaxLength == 4094 * MiB, "max %I64u\n", Free[0].MaxLength);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1, 4094), DL_OK);
    ok_int(Free_(Layout, Free), 0);

    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Extended(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 4096);
    DL_FREE Free[32];
    ULONG Count;

    /* extended partition over the whole disk: 4094 MB at 1024 KB, the logical drive 4093 MB at 2048 KB */
    ok_int(Make(Layout, DL_NEW_EXTENDED, 1, 4094), DL_OK);
    Count = Free_(Layout, Free);
    ok_int(Count, 1);
    ok(Free[0].InExtended, "not inside the extended partition\n");
    ok(Free[0].DataOffset == 2 * MiB, "data offset %I64u\n", Free[0].DataOffset);
    ok(Free[0].MaxLength == 4093 * MiB, "max %I64u\n", Free[0].MaxLength);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 2, 4093), DL_OK);
    ok_int(Free_(Layout, Free), 0);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* extended 1000 MB, then logical drives of 100, 100 and the rest: 2/100, 103/100, 204/797 */
    Layout = NewLayout(DM_STYLE_MBR, 4096);
    ok_int(Make(Layout, DL_NEW_EXTENDED, 1, 1000), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 2, 100), DL_OK);
    Count = FreeIn(Layout, Free);
    ok(Count >= 1, "no free space after the first drive\n");
    ok(Free[0].InExtended && Free[0].Offset == 102 * MiB && Free[0].DataOffset == 103 * MiB, "after drive 1: %I64u %I64u\n",
       Free[0].Offset / MiB, Free[0].DataOffset / MiB);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 103, 100), DL_OK);
    Count = FreeIn(Layout, Free);
    ok(Free[0].InExtended && Free[0].DataOffset == 204 * MiB, "after drive 2: %I64u\n", Free[0].DataOffset / MiB);
    ok(Free[0].MaxLength == 797 * MiB, "rest %I64u\n", Free[0].MaxLength / MiB);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 204, 797), DL_OK);

    /* a drive that is too long, or in front of the boot record sector, does not fit */
    ok_int(Make(Layout, DL_NEW_LOGICAL, 1002, 1), DL_E_NO_SPACE);
    HeapFree(GetProcessHeap(), 0, Layout);
}

/* the sizes the wizard of Disk Management offers, read from it on Windows Server 2008 R2 */
static VOID
Test_Wizard(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 4096);
    DL_FREE Free[32];
    DL_FREE One;
    ULONG Count;

    /* blank 4096 MB MBR disk: 4093 */
    Count = Free_(Layout, Free);
    ok_int(Count, 1);
    ok(Free[0].WizardMax == 4093 * MiB, "blank MBR disk offers %I64u MB\n", Free[0].WizardMax / MiB);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* 6 GB MBR disk with the extended partition ending at 5121 MB: 1022 MB left, offered 1021; 1534 MB free inside, offered 1533 */
    Layout = NewLayout(DM_STYLE_MBR, 6144);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1, 1024), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1025, 1024), DL_OK);
    ok_int(Make(Layout, DL_NEW_EXTENDED, 2049, 3072), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 2050, 1024), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 3075, 512), DL_OK);
    Count = Free_(Layout, Free);
    ok_int(Count, 2);
    ok(Free[0].WizardMax == 1021 * MiB, "tail of the 6 GB disk offers %I64u MB\n", Free[0].WizardMax / MiB);
    ok(Free[1].InExtended && Free[1].WizardMax == 1533 * MiB, "free inside the extended partition offers %I64u MB\n", Free[1].WizardMax / MiB);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* blank 4 GiB GPT disk: 4062 MB after the reserved partition; 5 GiB GPT disk behind 100, 16, 1024 and 1024 MB: 2953 */
    Layout = NewLayout(DM_STYLE_GPT, 4096);
    ok(DlFindFreeFor(Layout, 1 * MiB, FALSE, &One), "no gap on a blank GPT disk\n");
    ok(One.WizardMax == 4062 * MiB, "blank GPT disk offers %I64u MB\n", One.WizardMax / MiB);
    HeapFree(GetProcessHeap(), 0, Layout);
    Layout = NewLayout(DM_STYLE_GPT, 5120);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1, 100), DL_OK);
    ok_int(DlCreate(Layout, DL_NEW_PRIMARY, 101 * MiB, 16 * MiB, 0, &DlGuidReserved, &TestId, NULL), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 117, 1024), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1141, 1024), DL_OK);
    Count = Free_(Layout, Free);
    ok_int(Count, 1);
    ok(Free[0].WizardMax == 2953 * MiB, "GPT disk with four partitions offers %I64u MB\n", Free[0].WizardMax / MiB);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Gap(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 4096);
    DL_FREE Free[32];
    ULONG Count;

    /* delete the middle one of three drives (2/100, 103/100, 204/100): the gap holds 100 MB at 103 MB, as Windows gave it,
     * and the room behind the last one is 696 MB at 305 MB */
    ok_int(Make(Layout, DL_NEW_EXTENDED, 1, 1000), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 2, 100), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 103, 100), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 204, 100), DL_OK);
    ok_int(DlDelete(Layout, 103 * MiB), DL_OK);
    Count = FreeIn(Layout, Free);
    ok_int(Count, 2);
    ok(Free[0].DataOffset == 103 * MiB && Free[0].MaxLength == 100 * MiB, "gap: %I64u %I64u\n",
       Free[0].DataOffset / MiB, Free[0].MaxLength / MiB);
    ok(Free[1].DataOffset == 305 * MiB && Free[1].MaxLength == 696 * MiB, "tail: %I64u %I64u\n",
       Free[1].DataOffset / MiB, Free[1].MaxLength / MiB);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 103, 50), DL_OK);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Primaries(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 4096);
    DL_FREE Free[32];
    ULONG Count;

    /* three of 100 MB at 1, 101, 201, the fourth takes the rest: 3794 MB at 301 MB */
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1, 100), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 101, 100), DL_OK);
    Count = Free_(Layout, Free);
    ok(!DlWantsExtendedForNext(Layout, &Free[0]), "two primaries ask for an extended partition\n");
    ok_int(Make(Layout, DL_NEW_PRIMARY, 201, 100), DL_OK);
    Count = Free_(Layout, Free);
    ok_int(Count, 1);
    ok(Free[0].DataOffset == 301 * MiB && Free[0].MaxLength == 3794 * MiB, "rest %I64u/%I64u\n",
       Free[0].DataOffset / MiB, Free[0].MaxLength / MiB);
    ok(DlWantsExtendedForNext(Layout, &Free[0]), "the fourth volume does not become an extended partition\n");
    ok_int(Make(Layout, DL_NEW_PRIMARY, 301, 3794), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 301, 1), DL_E_NO_PRIMARY_SLOT);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* three primaries and then the extended partition: 10, 30, 100 MB, extended 3954 MB at 141 MB, drive 100 MB at 142 MB */
    Layout = NewLayout(DM_STYLE_MBR, 4096);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1, 10), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 11, 30), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 41, 100), DL_OK);
    ok_int(Make(Layout, DL_NEW_EXTENDED, 141, 3954), DL_OK);
    ok_int(Make(Layout, DL_NEW_EXTENDED, 141, 1), DL_E_HAVE_EXTENDED);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 142, 100), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 243, 100), DL_OK);
    ok_int(DlDelete(Layout, 141 * MiB), DL_E_EXTENDED_IN_USE);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Errors(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 4096);
    PDL_ENTRY Entry;

    ok_int(Make(Layout, DL_NEW_LOGICAL, 2, 10), DL_E_NO_EXTENDED);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1, 0), DL_E_BAD_ARGUMENT);
    ok_int(DlCreate(Layout, DL_NEW_PRIMARY, 1 * MiB + 512, 10 * MiB, 0, NULL, NULL, NULL), DL_E_BAD_ARGUMENT);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 0, 10), DL_E_NO_SPACE);     /* the first mebibyte is not usable */
    ok_int(Make(Layout, DL_NEW_PRIMARY, 4095, 1), DL_E_NO_SPACE);   /* and neither is the last one */
    ok_int(Make(Layout, DL_NEW_PRIMARY, 4094, 2), DL_E_NO_SPACE);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 4094, 1), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 1, 4000), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 4000, 10), DL_E_NO_SPACE);  /* overlaps */
    ok_int(DlDelete(Layout, 77 * MiB), DL_E_NOT_FOUND);
    ok_int(DlSetActive(Layout, 77 * MiB, TRUE), DL_E_NOT_FOUND);

    /* active: one at a time, and not a logical drive */
    ok_int(DlSetActive(Layout, 1 * MiB, TRUE), DL_OK);
    ok_int(DlSetActive(Layout, 4094 * MiB, TRUE), DL_OK);
    Entry = DlFindEntry(Layout, 1 * MiB);
    ok(Entry && !Entry->Active, "two active partitions\n");
    Entry = DlFindEntry(Layout, 4094 * MiB);
    ok(Entry && Entry->Active, "not active\n");
    ok_int(DlDelete(Layout, 4094 * MiB), DL_OK);
    ok_int(DlDelete(Layout, 1 * MiB), DL_OK);
    ok_int(Make(Layout, DL_NEW_EXTENDED, 1, 100), DL_OK);
    ok_int(Make(Layout, DL_NEW_LOGICAL, 2, 99), DL_OK);
    ok_int(DlSetActive(Layout, 2 * MiB, TRUE), DL_E_BAD_ARGUMENT);
    ok_int(DlSetActive(Layout, 1 * MiB, TRUE), DL_E_BAD_ARGUMENT);
    HeapFree(GetProcessHeap(), 0, Layout);

    Layout = NewLayout(DM_STYLE_GPT, 4096);
    ok_int(Make(Layout, DL_NEW_EXTENDED, 1, 10), DL_E_WRONG_STYLE);
    ok_int(DlSetActive(Layout, 1 * MiB, TRUE), DL_E_WRONG_STYLE);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Gpt(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_GPT, 4096);
    DL_FREE Free;
    PDL_ENTRY Entry;

    /* the first partition of a GPT disk brings a reserved partition: 32 MB at 17 KB on a 4 GiB disk, 128 MB on a large one */
    ok(DlGptNeedsReserved(Layout), "empty GPT disk needs no reserved partition\n");
    ok(DlReservedSize(4096 * MiB) == 32 * MiB, "reserved size of a small disk\n");
    ok(DlReservedSize(20480 * MiB) == 128 * MiB, "reserved size of a large disk\n");
    ok(DlFindFreeFor(Layout, 1 * MiB, FALSE, &Free), "no gap on an empty GPT disk\n");
    /* diskpart: the data partition of the 4 GiB disk is 4062 MB at 33 MB */
    ok(Free.DataOffset == 33 * MiB, "data offset %I64u\n", Free.DataOffset / MiB);
    ok(Free.MaxLength == 4062 * MiB, "max %I64u\n", Free.MaxLength / MiB);
    ok_int(DlCreateReserved(Layout, &TestId), DL_OK);
    Entry = DlFindEntry(Layout, 17408);
    ok(Entry != NULL, "reserved partition is not at 17 KB\n");
    ok(Entry && Entry->Length == 32 * MiB, "reserved length\n");
    ok_int(DlCreateReserved(Layout, &TestId), DL_E_NO_SPACE);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 33, 100), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 133, 3962), DL_OK);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* a 20 GiB disk: 128 MB reserved, then the data at 129 MB */
    Layout = NewLayout(DM_STYLE_GPT, 20480);
    ok(DlFindFreeFor(Layout, 1 * MiB, FALSE, &Free), "no gap on an empty large GPT disk\n");
    ok(Free.DataOffset == 129 * MiB, "large disk data offset %I64u\n", Free.DataOffset / MiB);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Driver(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 6144);
    PDRIVE_LAYOUT_INFORMATION_EX Driver;
    PDL_LAYOUT Copy = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    ULONG Size = 0;
    PDM_DISK Disk = NULL;
    DM_DISK_INPUT Input;

    /* the test disk of the reader tests: FAT32, Btrfs, extended with two drives */
    ok_int(DlCreate(Layout, DL_NEW_PRIMARY, 1 * MiB, 1024 * MiB, 0x0C, NULL, NULL, NULL), DL_OK);
    DlSetActive(Layout, 1 * MiB, TRUE);
    ok_int(DlCreate(Layout, DL_NEW_PRIMARY, 1025 * MiB, 1024 * MiB, 0x83, NULL, NULL, NULL), DL_OK);
    ok_int(Make(Layout, DL_NEW_EXTENDED, 2049, 3072), DL_OK);
    ok_int(DlCreate(Layout, DL_NEW_LOGICAL, 2050 * MiB, 1024 * MiB, 0x0C, NULL, NULL, NULL), DL_OK);
    ok_int(DlCreate(Layout, DL_NEW_LOGICAL, 3075 * MiB, 512 * MiB, 0x06, NULL, NULL, NULL), DL_OK);

    Driver = DlToDriver(Layout, &Size);
    ok(Driver != NULL, "DlToDriver failed\n");
    if (Driver == NULL)
        return;
    ok_int(Driver->PartitionStyle, PARTITION_STYLE_MBR);
    ok_int(Driver->PartitionCount, 12);                 /* the primary table and two boot records */
    ok(Size == FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + 12 * sizeof(PARTITION_INFORMATION_EX), "size %lu\n", Size);
    ok(Driver->PartitionEntry[0].StartingOffset.QuadPart == (LONGLONG)(1 * MiB), "slot 0\n");
    ok_int(Driver->PartitionEntry[0].Mbr.BootIndicator, TRUE);
    ok_int(Driver->PartitionEntry[0].Mbr.PartitionType, 0x0C);
    ok_int(Driver->PartitionEntry[2].Mbr.PartitionType, DL_MBR_TYPE_EXTENDED);
    ok(Driver->PartitionEntry[2].StartingOffset.QuadPart == (LONGLONG)(2049 * MiB), "extended start\n");
    ok_int(Driver->PartitionEntry[3].Mbr.PartitionType, 0);
    /* first boot record: the drive and the link to the next boot record at 3074 MB */
    ok(Driver->PartitionEntry[4].StartingOffset.QuadPart == (LONGLONG)(2050 * MiB), "drive 1 start\n");
    ok_int(Driver->PartitionEntry[4].Mbr.HiddenSectors, 2048);
    ok_int(Driver->PartitionEntry[5].Mbr.PartitionType, DL_MBR_TYPE_EXTENDED);
    ok(Driver->PartitionEntry[5].StartingOffset.QuadPart == (LONGLONG)(3074 * MiB), "link start %I64d\n",
       Driver->PartitionEntry[5].StartingOffset.QuadPart);
    ok(Driver->PartitionEntry[5].PartitionLength.QuadPart == (LONGLONG)(513 * MiB), "link length\n");
    ok_int(Driver->PartitionEntry[6].Mbr.PartitionType, 0);
    /* second boot record: the last drive, no link */
    ok(Driver->PartitionEntry[8].StartingOffset.QuadPart == (LONGLONG)(3075 * MiB), "drive 2 start\n");
    ok_int(Driver->PartitionEntry[9].Mbr.PartitionType, 0);
    ok_int(Driver->PartitionEntry[8].RewritePartition, TRUE);

    /* reading it back gives the layout again */
    ok(DlFromDriver(Driver, 6144 * MiB, 512, Copy), "DlFromDriver failed\n");
    ok_int(Copy->Count, Layout->Count);
    ok_int(DlCountPrimaries(Copy), 3);
    ok(DlFindExtended(Copy) != NULL, "no extended partition after the round trip\n");

    /* and the reader of the program draws the same blocks */
    ZeroMemory(&Input, sizeof(Input));
    Input.Device = DM_DEV_DISK;
    Input.BytesPerSector = 512;
    Input.DiskLength = 6144 * MiB;
    Input.HasMedia = TRUE;
    Input.Layout = Driver;
    Input.BusType = BusTypeScsi;
    Input.ScsiPath = Input.ScsiTarget = Input.ScsiLun = -1;
    ok(DmBuildDisk(&Input, &Disk), "DmBuildDisk failed\n");
    if (Disk)
    {
        /* PRIMFAT, BTRDATA, drive 1, drive 2, 1.5 GB free in the extended partition, 1 GB left */
        ok_int(Disk->RegionCount, 6);
        ok(Disk->UnallocatedMB == 2559, "unallocated %I64u\n", Disk->UnallocatedMB);
        DmFreeDisk(Disk);
    }
    HeapFree(GetProcessHeap(), 0, Driver);

    /* deleting the first drive: the second boot record moves to the start of the extended partition */
    ok_int(DlDelete(Layout, 2050 * MiB), DL_OK);
    Driver = DlToDriver(Layout, &Size);
    ok(Driver != NULL, "DlToDriver failed after the delete\n");
    if (Driver)
    {
        ok_int(Driver->PartitionCount, 8);
        ok(Driver->PartitionEntry[4].StartingOffset.QuadPart == (LONGLONG)(3075 * MiB), "drive start after the delete\n");
        ok_int(Driver->PartitionEntry[4].Mbr.HiddenSectors, (3075 - 2049) * 2048);
        ok_int(Driver->PartitionEntry[5].Mbr.PartitionType, 0);
        HeapFree(GetProcessHeap(), 0, Driver);
    }
    /* deleting the last drive: the extended partition keeps one empty boot record, rewritten, or the old drive would stay on disk */
    ok_int(DlDelete(Layout, 3075 * MiB), DL_OK);
    Driver = DlToDriver(Layout, &Size);
    ok(Driver != NULL, "DlToDriver failed with the extended partition empty\n");
    if (Driver)
    {
        ULONG k;

        ok_int(Driver->PartitionCount, 8);
        for (k = 4; k < 8; k++)
        {
            ok_int(Driver->PartitionEntry[k].Mbr.PartitionType, 0);
            ok_int(Driver->PartitionEntry[k].RewritePartition, TRUE);
        }
        HeapFree(GetProcessHeap(), 0, Driver);
    }
    /* and without the extended partition the table is the four entries */
    ok_int(DlDelete(Layout, 2049 * MiB), DL_OK);
    Driver = DlToDriver(Layout, &Size);
    ok(Driver != NULL && Driver->PartitionCount == 4, "primary table only\n");
    if (Driver)
        HeapFree(GetProcessHeap(), 0, Driver);
    HeapFree(GetProcessHeap(), 0, Copy);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_GptDriver(VOID)
{
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_GPT, 4096);
    PDL_LAYOUT Copy = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    PDRIVE_LAYOUT_INFORMATION_EX Driver;
    ULONG Size;

    ok_int(DlCreateReserved(Layout, &TestId), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 33, 100), DL_OK);
    Driver = DlToDriver(Layout, &Size);
    ok(Driver != NULL, "DlToDriver failed\n");
    if (Driver)
    {
        ok_int(Driver->PartitionStyle, PARTITION_STYLE_GPT);
        ok_int(Driver->PartitionCount, 3);              /* two partitions and the spare entry */
        ok_int(Driver->Gpt.MaxPartitionCount, 128);
        ok(Driver->PartitionEntry[0].StartingOffset.QuadPart == 17408, "reserved partition start\n");
        ok(IsEqualGUID(&Driver->PartitionEntry[0].Gpt.PartitionType, &DlGuidReserved), "reserved type\n");
        ok(IsEqualGUID(&Driver->PartitionEntry[1].Gpt.PartitionType, &DlGuidBasicData), "data type\n");
        ok_int(Driver->PartitionEntry[2].Gpt.PartitionType.Data1, 0);
        ok(DlFromDriver(Driver, 4096 * MiB, 512, Copy), "DlFromDriver failed\n");
        ok_int(Copy->Count, 2);
        HeapFree(GetProcessHeap(), 0, Driver);
    }
    HeapFree(GetProcessHeap(), 0, Copy);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Types(VOID)
{
    /* the type after "format" on Windows Server 2008 R2: 10 MB FAT 01, 30 MB FAT 04, 100 MB FAT 06, FAT32 0B, NTFS 07, exFAT 07 */
    ok_int(DlMbrTypeForFileSystem(L"FAT", TRUE, FALSE, 1 * MiB, 10 * MiB, 0x06), 0x01);
    ok_int(DlMbrTypeForFileSystem(L"FAT", FALSE, TRUE, 11 * MiB, 30 * MiB, 0x06), 0x04);
    ok_int(DlMbrTypeForFileSystem(L"FAT", FALSE, TRUE, 41 * MiB, 100 * MiB, 0x06), 0x06);
    ok_int(DlMbrTypeForFileSystem(L"FAT32", FALSE, FALSE, 243 * MiB, 100 * MiB, 0x06), 0x0B);
    ok_int(DlMbrTypeForFileSystem(L"NTFS", FALSE, FALSE, 344 * MiB, 100 * MiB, 0x06), 0x07);
    ok_int(DlMbrTypeForFileSystem(L"exFAT", FALSE, FALSE, 445 * MiB, 100 * MiB, 0x06), 0x07);
    /* beyond the CHS limit of 8 GB the LBA types */
    ok_int(DlMbrTypeForFileSystem(L"FAT32", FALSE, FALSE, 9000 * MiB, 1000 * MiB, 0x06), 0x0C);
    ok_int(DlMbrTypeForFileSystem(L"FAT", FALSE, TRUE, 9000 * MiB, 1000 * MiB, 0x06), 0x0E);
    /* a file system the table has no byte for loses a FAT type and keeps any other */
    ok_int(DlMbrTypeForFileSystem(L"Btrfs", FALSE, FALSE, 1 * MiB, 100 * MiB, 0x06), 0x07);
    ok_int(DlMbrTypeForFileSystem(L"Btrfs", FALSE, FALSE, 1 * MiB, 100 * MiB, 0x83), 0x83);
    ok_int(DlMbrTypeForFileSystem(NULL, FALSE, FALSE, 1 * MiB, 100 * MiB, 0x0B), 0x0B);
}

static VOID
Test_Legacy(VOID)
{
    /* a layout made by an old tool: first partition at 63 sectors, no alignment anywhere */
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    PDRIVE_LAYOUT_INFORMATION_EX Driver;
    DL_FREE Free[32];
    ULONG Count, Size;

    DlInit(Layout, DM_STYLE_MBR, 1000 * MiB, 512, 1, NULL);
    ok_int(Layout->Count, 0);
    Layout->Count = 1;
    Layout->Entry[0].Offset = 63 * 512;
    Layout->Entry[0].Length = 100 * MiB + 77 * 512;
    Layout->Entry[0].MbrType = 0x06;
    Count = DlFindFree(Layout, Free, 32);
    ok_int(Count, 1);
    /* the gap behind it starts on the next mebibyte */
    ok(Free[0].DataOffset == 101 * MiB, "legacy data offset %I64u\n", Free[0].DataOffset);
    Driver = DlToDriver(Layout, &Size);
    ok(Driver != NULL, "DlToDriver failed\n");
    if (Driver)
    {
        ok(Driver->PartitionEntry[0].StartingOffset.QuadPart == 63 * 512, "legacy start kept\n");
        HeapFree(GetProcessHeap(), 0, Driver);
    }
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_Limits(VOID)
{
    PDL_LAYOUT Layout;
    PDL_ENTRY Entry;
    DL_FREE Free[32];
    PDRIVE_LAYOUT_INFORMATION_EX Driver;
    ULONG Count, Size, i;

    /* an MBR holds the start and the length of a partition as 32-bit counts of sectors: 2 TiB less a mebibyte of 512-byte sectors */
    Layout = NewLayout(DM_STYLE_MBR, 3 * 1024 * 1024);
    Count = Free_(Layout, Free);
    ok_int(Count, 1);
    ok(Free[0].DataOffset == 1 * MiB && Free[0].MaxLength == (2 * 1024 * 1024 - 2) * MiB, "max on a 3 TiB disk: %I64u MiB\n",
       Free[0].MaxLength / MiB);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 2 * 1024 * 1024 - 1, 1), DL_E_NO_SPACE);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 2 * 1024 * 1024 - 2, 1), DL_OK);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 2 * 1024 * 1024, 1), DL_E_NO_SPACE);
    ok(DlIsEncodable(Layout), "a partition below the limit is not encodable\n");
    Entry = DlFindEntry(Layout, (2 * 1024 * 1024 - 2) * MiB);
    if (Entry)
        Entry->Offset = 2 * 1024 * 1024 * MiB;
    ok(!DlIsEncodable(Layout), "a start beyond 32 bits of sectors is encodable\n");
    HeapFree(GetProcessHeap(), 0, Layout);

    /* a partition whose last sector is the last one an MBR can number is fine; one sector more is not */
    Layout = NewLayout(DM_STYLE_MBR, 3 * 1024 * 1024);
    Layout->Count = 1;
    Layout->Entry[0].Offset = 2048 * 512;
    Layout->Entry[0].Length = (0x100000000ULL - 2048) * 512;
    ok(DlIsEncodable(Layout), "a partition that ends with sector 0xFFFFFFFF is not encodable\n");
    Layout->Entry[0].Length += 512;
    ok(!DlIsEncodable(Layout), "a partition that ends beyond sector 0xFFFFFFFF is encodable\n");
    HeapFree(GetProcessHeap(), 0, Layout);

    /* ranges that wrap around are refused, whatever the free space says */
    Layout = NewLayout(DM_STYLE_MBR, 4096);
    ok_int(DlCreate(Layout, DL_NEW_PRIMARY, 0xFFFFFFFFFFF00000ULL, 1 * MiB, 0, NULL, NULL, NULL), DL_E_BAD_ARGUMENT);
    ok_int(DlCreate(Layout, DL_NEW_PRIMARY, 1 * MiB, 0xFFFFFFFFFFF00000ULL, 0, NULL, NULL, NULL), DL_E_BAD_ARGUMENT);
    ok_int(DlCreate(Layout, DL_NEW_EXTENDED, 0xFFFFFFFFFFF00000ULL, 1 * MiB, 0, NULL, NULL, NULL), DL_E_BAD_ARGUMENT);
    ok_int(Layout->Count, 0);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* a GPT table with all its 128 entries in use has no room for the spare, empty entry */
    Layout = NewLayout(DM_STYLE_GPT, 4096);
    for (i = 0; i < 127; i++)
        ok_int(Make(Layout, DL_NEW_PRIMARY, 1 + i, 1), DL_OK);
    Driver = DlToDriver(Layout, &Size);
    ok(Driver && Driver->PartitionCount == 128, "127 entries and the spare: %lu\n", Driver ? Driver->PartitionCount : 0);
    if (Driver)
        HeapFree(GetProcessHeap(), 0, Driver);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 128, 1), DL_OK);
    Driver = DlToDriver(Layout, &Size);
    ok(Driver && Driver->PartitionCount == 128, "128 entries and no spare: %lu\n", Driver ? Driver->PartitionCount : 0);
    if (Driver)
        HeapFree(GetProcessHeap(), 0, Driver);
    ok_int(Make(Layout, DL_NEW_PRIMARY, 129, 1), DL_E_TOO_MANY);
    HeapFree(GetProcessHeap(), 0, Layout);
}

static VOID
Test_LinuxExtended(VOID)
{
    /* the partition manager takes only 0x05 and 0x0F as extended partitions; an extra group of table entries for a 0x85
     * partition would be written over the table, so it is an ordinary partition here too */
    PDL_LAYOUT Layout = NewLayout(DM_STYLE_MBR, 1000);
    PDL_LAYOUT Back = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    PDRIVE_LAYOUT_INFORMATION_EX Driver, Again;
    ULONG Size;

    Layout->Count = 1;
    Layout->Entry[0].Offset = 1 * MiB;
    Layout->Entry[0].Length = 100 * MiB;
    Layout->Entry[0].MbrType = 0x85;
    Driver = DlToDriver(Layout, &Size);
    ok(Driver && Driver->PartitionCount == 4, "a table of %lu entries\n", Driver ? Driver->PartitionCount : 0);
    if (Driver)
    {
        ok(DlFromDriver(Driver, 1000 * MiB, 512, Back), "DlFromDriver failed\n");
        ok(Back->Count == 1 && !Back->Entry[0].Container, "a 0x85 partition read as an extended partition\n");
        Again = DlToDriver(Back, &Size);
        ok(Again && Again->PartitionCount == 4, "written back as %lu entries\n", Again ? Again->PartitionCount : 0);
        if (Again)
            HeapFree(GetProcessHeap(), 0, Again);
        HeapFree(GetProcessHeap(), 0, Driver);
    }
    HeapFree(GetProcessHeap(), 0, Back);
    HeapFree(GetProcessHeap(), 0, Layout);
}

START_TEST(DiskLayout)
{
    Test_Empty();
    Test_Extended();
    Test_Wizard();
    Test_Gap();
    Test_Primaries();
    Test_Errors();
    Test_Gpt();
    Test_Driver();
    Test_GptDriver();
    Test_Types();
    Test_Legacy();
    Test_Limits();
    Test_LinuxExtended();
}
