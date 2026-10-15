/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for IOCTL_DISK_GROW_PARTITION
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * DESTRUCTIVE: repartitions the disk whose number is in the environment
 * variable PARTMGR_TEST_DISK (at least 512 MB, nothing on it that matters).
 * Without that variable the test is skipped.  The expectations are those
 * of Windows Server 2008 R2.
 */

#include "precomp.h"
#include <winioctl.h>
#include <stdio.h>

#define MiB (1024LL * 1024LL)

static ULONG DiskNumber;

static const GUID BasicDataGuid = { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

static HANDLE OpenDisk(DWORD Access)
{
    CHAR Name[64];
    sprintf(Name, "\\\\.\\PhysicalDrive%lu", DiskNumber);
    return CreateFileA(Name, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static HANDLE OpenPartition(ULONG Number, DWORD Access)
{
    CHAR Name[64];
    sprintf(Name, "\\\\?\\GLOBALROOT\\Device\\Harddisk%lu\\Partition%lu", DiskNumber, Number);
    return CreateFileA(Name, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

/* Grows partition Number by Bytes; returns the Win32 error (0 on success). */
static DWORD Grow(HANDLE Disk, ULONG Number, LONGLONG Bytes, DWORD InLength)
{
    DISK_GROW_PARTITION Grow;
    DWORD Returned;

    Grow.PartitionNumber = Number;
    Grow.BytesToGrow.QuadPart = Bytes;
    if (DeviceIoControl(Disk, IOCTL_DISK_GROW_PARTITION, &Grow, InLength ? InLength : sizeof(Grow), NULL, 0, &Returned, NULL))
        return 0;
    return GetLastError();
}

static BOOL GetLayout(PDRIVE_LAYOUT_INFORMATION_EX Layout, DWORD Size)
{
    DWORD Returned;
    HANDLE Disk = OpenDisk(GENERIC_READ);
    BOOL Ok = DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, Layout, Size, &Returned, NULL);
    CloseHandle(Disk);
    return Ok;
}

/* The length of the layout entry with partition number Number, or -1. */
static LONGLONG LayoutLength(ULONG Number)
{
    BYTE Buffer[8192];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PVOID)Buffer;
    ULONG i;

    if (!GetLayout(Layout, sizeof(Buffer)))
        return -1;
    for (i = 0; i < Layout->PartitionCount; i++)
    {
        if (Layout->PartitionEntry[i].PartitionNumber == Number)
            return Layout->PartitionEntry[i].PartitionLength.QuadPart;
    }
    return -1;
}

/* The length of the layout entry at index Index. */
static LONGLONG LayoutEntryLength(ULONG Index)
{
    BYTE Buffer[8192];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PVOID)Buffer;

    if (!GetLayout(Layout, sizeof(Buffer)) || Index >= Layout->PartitionCount)
        return -1;
    return Layout->PartitionEntry[Index].PartitionLength.QuadPart;
}

/* The length the partition device itself reports. */
static LONGLONG DeviceLength(ULONG Number)
{
    GET_LENGTH_INFORMATION Length;
    DWORD Returned;
    HANDLE Partition = OpenPartition(Number, GENERIC_READ);
    BOOL Ok;

    if (Partition == INVALID_HANDLE_VALUE)
        return -1;
    Ok = DeviceIoControl(Partition, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &Length, sizeof(Length), &Returned, NULL);
    CloseHandle(Partition);
    return Ok ? Length.Length.QuadPart : -1;
}

/* The sector count of MBR entry Slot as written in sector 0. */
static DWORD MbrSectorCount(ULONG Slot)
{
    BYTE *Sector = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
    HANDLE Disk = OpenDisk(GENERIC_READ);
    DWORD Read, Count = 0;

    if (ReadFile(Disk, Sector, 512, &Read, NULL) && Read == 512)
        Count = *(DWORD UNALIGNED *)(Sector + 446 + 16 * Slot + 12);
    CloseHandle(Disk);
    VirtualFree(Sector, 0, MEM_RELEASE);
    return Count;
}

static BOOL SetLayout(PDRIVE_LAYOUT_INFORMATION_EX Layout, DWORD Size)
{
    DWORD Returned;
    HANDLE Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);
    BOOL Ok = DeviceIoControl(Disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, Layout, Size, NULL, 0, &Returned, NULL);
    DeviceIoControl(Disk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &Returned, NULL);
    CloseHandle(Disk);
    /* The partition devices appear asynchronously. */
    Sleep(2000);
    return Ok;
}

/* Writes the table and returns at once, before the partition devices change. */
static BOOL SetLayoutNow(PDRIVE_LAYOUT_INFORMATION_EX Layout, DWORD Size)
{
    DWORD Returned;
    HANDLE Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);
    BOOL Ok = DeviceIoControl(Disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, Layout, Size, NULL, 0, &Returned, NULL);
    CloseHandle(Disk);
    return Ok;
}

static BOOL CreateDiskStyle(PARTITION_STYLE Style)
{
    CREATE_DISK Create;
    DWORD Returned;
    HANDLE Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);
    BOOL Ok;

    ZeroMemory(&Create, sizeof(Create));
    Create.PartitionStyle = Style;
    if (Style == PARTITION_STYLE_MBR)
    {
        Create.Mbr.Signature = 0x0DCE0001;
    }
    else
    {
        Create.Gpt.DiskId.Data1 = 0x0DCE0002;
        Create.Gpt.MaxPartitionCount = 128;
    }
    Ok = DeviceIoControl(Disk, IOCTL_DISK_CREATE_DISK, &Create, sizeof(Create), NULL, 0, &Returned, NULL);
    DeviceIoControl(Disk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &Returned, NULL);
    CloseHandle(Disk);
    Sleep(1000);
    return Ok;
}

static void MbrEntry(PPARTITION_INFORMATION_EX Entry, LONGLONG Start, LONGLONG Length, BYTE Type, DWORD Hidden)
{
    ZeroMemory(Entry, sizeof(*Entry));
    Entry->PartitionStyle = PARTITION_STYLE_MBR;
    Entry->StartingOffset.QuadPart = Start;
    Entry->PartitionLength.QuadPart = Length;
    Entry->RewritePartition = TRUE;
    Entry->Mbr.PartitionType = Type;
    Entry->Mbr.RecognizedPartition = Type != 0 && Type != PARTITION_EXTENDED;
    Entry->Mbr.HiddenSectors = Hidden;
}

static void GptEntry(PPARTITION_INFORMATION_EX Entry, LONGLONG Start, LONGLONG Length, DWORD Id)
{
    ZeroMemory(Entry, sizeof(*Entry));
    Entry->PartitionStyle = PARTITION_STYLE_GPT;
    Entry->StartingOffset.QuadPart = Start;
    Entry->PartitionLength.QuadPart = Length;
    Entry->RewritePartition = TRUE;
    Entry->Gpt.PartitionType = BasicDataGuid;
    Entry->Gpt.PartitionId.Data1 = Id;
    Entry->Gpt.PartitionId.Data2 = 0x0dce;
    wcscpy(Entry->Gpt.Name, L"growtest");
}

static void TestMbrPrimary(void)
{
    BYTE Buffer[8192];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PVOID)Buffer;
    HANDLE Disk, ReadOnly, Partition;

    /* P1 at 1 MiB, 64 MiB long; free space up to P2 at 129 MiB, 32 MiB long; free space after. */
    ok(CreateDiskStyle(PARTITION_STYLE_MBR), "IOCTL_DISK_CREATE_DISK failed: %lu\n", GetLastError());
    ZeroMemory(Buffer, sizeof(Buffer));
    Layout->PartitionStyle = PARTITION_STYLE_MBR;
    Layout->PartitionCount = 4;
    Layout->Mbr.Signature = 0x0DCE0001;
    MbrEntry(&Layout->PartitionEntry[0], 1 * MiB, 64 * MiB, PARTITION_IFS, 2048);
    MbrEntry(&Layout->PartitionEntry[1], 129 * MiB, 32 * MiB, PARTITION_IFS, 264192);
    MbrEntry(&Layout->PartitionEntry[2], 0, 0, 0, 0);
    MbrEntry(&Layout->PartitionEntry[3], 0, 0, 0, 0);
    if (!SetLayout(Layout, FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry[4])))
    {
        skip("IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed: %lu\n", GetLastError());
        return;
    }
    ok(LayoutLength(1) == 64 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));

    Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);
    ReadOnly = OpenDisk(GENERIC_READ);
    ok(Disk != INVALID_HANDLE_VALUE, "Opening disk %lu failed: %lu\n", DiskNumber, GetLastError());

    /* The control code requires read and write access, and the whole input. */
    ok_int(Grow(ReadOnly, 1, 16 * MiB, 0), ERROR_ACCESS_DENIED);
    ok_int(Grow(Disk, 1, 16 * MiB, sizeof(DWORD)), ERROR_BAD_LENGTH);
    ok(LayoutLength(1) == 64 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));

    /* Into the free space after it: the table on disk, the cached layout and the device follow. */
    ok_int(Grow(Disk, 1, 16 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 80 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));
    ok_long(MbrSectorCount(0), 80 * 2048);
    ok_long(MbrSectorCount(1), 32 * 2048);
    ok(DeviceLength(1) == 80 * MiB, "P1 device is %I64d bytes\n", DeviceLength(1));

    /* Bytes become whole sectors: an MBR entry holds a sector count, so a partial sector is dropped. */
    ok_int(Grow(Disk, 1, 1, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 80 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));
    ok_int(Grow(Disk, 1, 512, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 80 * MiB + 512, "P1 is %I64d bytes\n", LayoutLength(1));
    ok_int(Grow(Disk, 1, 0, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 80 * MiB + 512, "P1 is %I64d bytes\n", LayoutLength(1));

    /* Not into the next partition, nor past the end of the disk. */
    ok_int(Grow(Disk, 1, 64 * MiB, 0), ERROR_INVALID_PARAMETER);
    ok(LayoutLength(1) == 80 * MiB + 512, "P1 is %I64d bytes\n", LayoutLength(1));
    ok_int(Grow(Disk, 2, 1000000 * MiB, 0), ERROR_INVALID_PARAMETER);

    /* A negative size shrinks, down to a last sector, not to nothing. */
    ok_int(Grow(Disk, 1, -8 * MiB - 512, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 72 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));
    ok(DeviceLength(1) == 72 * MiB, "P1 device is %I64d bytes\n", DeviceLength(1));
    ok_int(Grow(Disk, 1, -72 * MiB, 0), ERROR_INVALID_PARAMETER);
    ok_int(Grow(Disk, 1, -200 * MiB, 0), ERROR_INVALID_PARAMETER);
    ok(LayoutLength(1) == 72 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));

    /* Partition numbers that do not exist. */
    ok_int(Grow(Disk, 0, 16 * MiB, 0), ERROR_FILE_NOT_FOUND);
    ok_int(Grow(Disk, 3, 16 * MiB, 0), ERROR_FILE_NOT_FOUND);
    ok_int(Grow(Disk, 99, 16 * MiB, 0), ERROR_FILE_NOT_FOUND);

    /* The last partition grows into the free space at the end of the disk. */
    ok_int(Grow(Disk, 2, 100 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(2) == 132 * MiB, "P2 is %I64d bytes\n", LayoutLength(2));

    /* A handle to a partition device works too. */
    Partition = OpenPartition(1, GENERIC_READ | GENERIC_WRITE);
    ok(Partition != INVALID_HANDLE_VALUE, "Opening partition 1 failed: %lu\n", GetLastError());
    if (Partition != INVALID_HANDLE_VALUE)
    {
        ok_int(Grow(Partition, 1, 1 * MiB, 0), ERROR_SUCCESS);
        CloseHandle(Partition);
    }
    ok(LayoutLength(1) == 73 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));

    CloseHandle(ReadOnly);
    CloseHandle(Disk);
}

static void TestMbrLogical(void)
{
    BYTE Buffer[8192];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PVOID)Buffer;
    HANDLE Disk;
    ULONG i;

    /* Extended partition at 1 MiB, 128 MiB long: L1 at 2 MiB (32 MiB), L2 at 64 MiB (16 MiB, its EBR at 63 MiB). */
    ok(CreateDiskStyle(PARTITION_STYLE_MBR), "IOCTL_DISK_CREATE_DISK failed: %lu\n", GetLastError());
    ZeroMemory(Buffer, sizeof(Buffer));
    Layout->PartitionStyle = PARTITION_STYLE_MBR;
    Layout->PartitionCount = 12;
    Layout->Mbr.Signature = 0x0DCE0001;
    for (i = 0; i < 12; i++)
        MbrEntry(&Layout->PartitionEntry[i], 0, 0, 0, 0);
    MbrEntry(&Layout->PartitionEntry[0], 1 * MiB, 128 * MiB, PARTITION_EXTENDED, 2048);
    MbrEntry(&Layout->PartitionEntry[4], 2 * MiB, 32 * MiB, PARTITION_IFS, 2048);
    MbrEntry(&Layout->PartitionEntry[5], 63 * MiB, 17 * MiB, PARTITION_EXTENDED, 126976);
    MbrEntry(&Layout->PartitionEntry[8], 64 * MiB, 16 * MiB, PARTITION_IFS, 2048);
    if (!SetLayout(Layout, FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry[12])))
    {
        skip("IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed: %lu\n", GetLastError());
        return;
    }
    ok(LayoutLength(1) == 32 * MiB, "L1 is %I64d bytes\n", LayoutLength(1));
    ok(LayoutLength(2) == 16 * MiB, "L2 is %I64d bytes\n", LayoutLength(2));

    Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);

    /* L1 grows up to the EBR of L2, not into it. */
    ok_int(Grow(Disk, 1, 8 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 40 * MiB, "L1 is %I64d bytes\n", LayoutLength(1));
    ok_int(Grow(Disk, 1, 30 * MiB, 0), ERROR_INVALID_PARAMETER);

    /* L2 grows inside the container; the link entry of the EBR before it follows. */
    ok_int(Grow(Disk, 2, 40 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(2) == 56 * MiB, "L2 is %I64d bytes\n", LayoutLength(2));
    ok(LayoutEntryLength(5) == 57 * MiB, "link is %I64d bytes\n", LayoutEntryLength(5));
    ok(LayoutEntryLength(0) == 128 * MiB, "container is %I64d bytes\n", LayoutEntryLength(0));

    /* Past the end of the container: the container grows with it. */
    ok_int(Grow(Disk, 2, 100 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(2) == 156 * MiB, "L2 is %I64d bytes\n", LayoutLength(2));
    ok(LayoutEntryLength(5) == 157 * MiB, "link is %I64d bytes\n", LayoutEntryLength(5));
    ok(LayoutEntryLength(0) == 219 * MiB, "container is %I64d bytes\n", LayoutEntryLength(0));

    CloseHandle(Disk);
}

static void TestGpt(void)
{
    BYTE Buffer[8192];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PVOID)Buffer;
    LONGLONG UsableEnd;
    HANDLE Disk;

    ok(CreateDiskStyle(PARTITION_STYLE_GPT), "IOCTL_DISK_CREATE_DISK failed: %lu\n", GetLastError());
    ZeroMemory(Buffer, sizeof(Buffer));
    if (!GetLayout(Layout, sizeof(Buffer)))
    {
        skip("No GPT layout: %lu\n", GetLastError());
        return;
    }
    UsableEnd = Layout->Gpt.StartingUsableOffset.QuadPart + Layout->Gpt.UsableLength.QuadPart;
    Layout->PartitionCount = 2;
    GptEntry(&Layout->PartitionEntry[0], 1 * MiB, 64 * MiB, 1);
    GptEntry(&Layout->PartitionEntry[1], 129 * MiB, 32 * MiB, 2);
    if (!SetLayout(Layout, FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry[2])))
    {
        skip("IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed: %lu\n", GetLastError());
        return;
    }

    Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);
    ok_int(Grow(Disk, 1, 16 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 80 * MiB, "P1 is %I64d bytes\n", LayoutLength(1));
    ok(DeviceLength(1) == 80 * MiB, "P1 device is %I64d bytes\n", DeviceLength(1));
    ok_int(Grow(Disk, 1, 64 * MiB, 0), ERROR_INVALID_PARAMETER);

    /* A GPT entry holds the last sector: a partial sector takes in the whole one. */
    ok_int(Grow(Disk, 1, 1, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 80 * MiB + 512, "P1 is %I64d bytes\n", LayoutLength(1));
    ok_int(Grow(Disk, 1, -8 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(1) == 72 * MiB + 512, "P1 is %I64d bytes\n", LayoutLength(1));

    /* Up to the end of the usable area, not one sector further. */
    ok_int(Grow(Disk, 2, 1000000 * MiB, 0), ERROR_INVALID_PARAMETER);
    ok_int(Grow(Disk, 2, UsableEnd - 161 * MiB, 0), ERROR_SUCCESS);
    ok(LayoutLength(2) == UsableEnd - 129 * MiB, "P2 is %I64d bytes\n", LayoutLength(2));
    ok_int(Grow(Disk, 2, 512, 0), ERROR_INVALID_PARAMETER);

    CloseHandle(Disk);
}

/*
 * A partition device keeps the instance ID it got when it appeared, also after it grew. A table
 * written afterwards that gives the partition its first length again makes a new device, while
 * the old one still exists (here an open handle keeps it, as a mounted file system does); the two
 * must not have the same ID (a duplicate device instance stops ReactOS with bugcheck 0xCA). The
 * same for a partition deleted and made again while the old device still exists.
 */
static void TestSetLayoutAfterGrow(void)
{
    BYTE Buffer[8192];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PVOID)Buffer;
    DWORD Size = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry[4]);
    HANDLE Disk, Old;
    ULONG Round;

    ok(CreateDiskStyle(PARTITION_STYLE_MBR), "IOCTL_DISK_CREATE_DISK failed: %lu\n", GetLastError());
    ZeroMemory(Buffer, sizeof(Buffer));
    Layout->PartitionStyle = PARTITION_STYLE_MBR;
    Layout->PartitionCount = 4;
    Layout->Mbr.Signature = 0x0DCE0001;
    MbrEntry(&Layout->PartitionEntry[0], 1 * MiB, 64 * MiB, PARTITION_IFS, 2048);
    MbrEntry(&Layout->PartitionEntry[1], 0, 0, 0, 0);
    MbrEntry(&Layout->PartitionEntry[2], 0, 0, 0, 0);
    MbrEntry(&Layout->PartitionEntry[3], 0, 0, 0, 0);
    if (!SetLayout(Layout, Size))
    {
        skip("IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed: %lu\n", GetLastError());
        return;
    }
    ok(DeviceLength(1) == 64 * MiB, "P1 device is %I64d bytes\n", DeviceLength(1));

    for (Round = 0; Round < 2; Round++)
    {
        Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);
        ok_int(Grow(Disk, 1, 16 * MiB, 0), ERROR_SUCCESS);
        CloseHandle(Disk);
        ok(DeviceLength(1) == 80 * MiB, "round %lu: P1 device is %I64d bytes after growing\n", Round, DeviceLength(1));

        Old = OpenPartition(1, GENERIC_READ);
        ok(Old != INVALID_HANDLE_VALUE, "round %lu: opening P1 failed: %lu\n", Round, GetLastError());
        MbrEntry(&Layout->PartitionEntry[0], 1 * MiB, 64 * MiB, PARTITION_IFS, 2048);
        ok(SetLayout(Layout, Size), "round %lu: IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed: %lu\n", Round, GetLastError());
        ok(LayoutLength(1) == 64 * MiB, "round %lu: P1 is %I64d bytes\n", Round, LayoutLength(1));
        ok(DeviceLength(1) == 64 * MiB, "round %lu: P1 device is %I64d bytes after the table\n", Round, DeviceLength(1));
        if (Old != INVALID_HANDLE_VALUE)
            CloseHandle(Old);
    }

    /* deleted and made again while the old device is open, at once and then once more */
    for (Round = 0; Round < 2; Round++)
    {
        Old = OpenPartition(1, GENERIC_READ);
        ok(Old != INVALID_HANDLE_VALUE, "round %lu: opening P1 failed: %lu\n", Round, GetLastError());
        MbrEntry(&Layout->PartitionEntry[0], 0, 0, 0, 0);
        ok(SetLayout(Layout, Size), "round %lu: deleting P1 failed: %lu\n", Round, GetLastError());
        MbrEntry(&Layout->PartitionEntry[0], 1 * MiB, 64 * MiB, PARTITION_IFS, 2048);
        ok(SetLayoutNow(Layout, Size), "round %lu: making P1 again failed: %lu\n", Round, GetLastError());
        Sleep(2000);
        ok(LayoutLength(1) == 64 * MiB, "round %lu: P1 is %I64d bytes\n", Round, LayoutLength(1));
        ok(DeviceLength(1) == 64 * MiB, "round %lu: P1 device is %I64d bytes after it was made again\n", Round, DeviceLength(1));
        if (Old != INVALID_HANDLE_VALUE)
            CloseHandle(Old);
    }
}

START_TEST(GrowPartition)
{
    CHAR Value[16];
    DISK_GEOMETRY_EX Geometry;
    DWORD Returned;
    HANDLE Disk;

    if (!GetEnvironmentVariableA("PARTMGR_TEST_DISK", Value, sizeof(Value)))
    {
        skip("PARTMGR_TEST_DISK is not set: no scratch disk to repartition\n");
        return;
    }
    DiskNumber = strtoul(Value, NULL, 10);

    Disk = OpenDisk(GENERIC_READ);
    if (Disk == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &Geometry, sizeof(Geometry), &Returned, NULL) ||
        Geometry.Geometry.BytesPerSector != 512 || Geometry.DiskSize.QuadPart < 512 * MiB)
    {
        skip("Disk %lu is not a 512-byte-sector disk of at least 512 MB\n", DiskNumber);
        if (Disk != INVALID_HANDLE_VALUE)
            CloseHandle(Disk);
        return;
    }
    CloseHandle(Disk);

    /* A handle to the whole disk flushes the disk's write cache (the FAT provider's offline growth relies on it). */
    Disk = OpenDisk(GENERIC_READ | GENERIC_WRITE);
    ok(FlushFileBuffers(Disk), "Flushing disk %lu failed: %lu\n", DiskNumber, GetLastError());
    CloseHandle(Disk);

    TestMbrPrimary();
    TestMbrLogical();
    TestGpt();
    TestSetLayoutAfterGrow();
}
