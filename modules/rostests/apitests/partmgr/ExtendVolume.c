/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for resizing a mounted volume: IOCTL_DISK_GROW_PARTITION, FSCTL_EXTEND_VOLUME, FSCTL_SHRINK_VOLUME
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * DESTRUCTIVE for the volume whose drive letter is in the environment variable
 * PARTMGR_TEST_VOLUME: a volume on a partition with at least 32 MB of free disk
 * space after it, holding nothing that matters.  Without the variable the test is
 * skipped.  NTFS expectations are those of Windows Server 2008 R2; Windows
 * extends no FAT volume (ERROR_INVALID_FUNCTION), ReactOS extends FAT volumes
 * whose FATs have room for the new clusters.
 */

#include "precomp.h"
#include <winioctl.h>
#include <ntddstor.h>
#include <wingdi.h>
#include <winuser.h>
#include <winreg.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef FSCTL_EXTEND_VOLUME
#define FSCTL_EXTEND_VOLUME CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 60, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef FSCTL_SHRINK_VOLUME
#define FSCTL_SHRINK_VOLUME CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 108, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
typedef enum _SHRINK_VOLUME_REQUEST_TYPES
{
    ShrinkPrepare = 1,
    ShrinkCommit,
    ShrinkAbort
} SHRINK_VOLUME_REQUEST_TYPES;
typedef struct _SHRINK_VOLUME_INFORMATION
{
    SHRINK_VOLUME_REQUEST_TYPES ShrinkRequestType;
    ULONGLONG Flags;
    LONGLONG NewNumberOfSectors;
} SHRINK_VOLUME_INFORMATION;
#endif

#define MiB (1024LL * 1024LL)

static CHAR Letter;

static HANDLE OpenVolume(DWORD Access)
{
    CHAR Name[16];
    sprintf(Name, "\\\\.\\%c:", Letter);
    return CreateFileA(Name, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static HANDLE OpenDisk(ULONG Number, DWORD Access)
{
    CHAR Name[64];
    sprintf(Name, "\\\\.\\PhysicalDrive%lu", Number);
    return CreateFileA(Name, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static DWORD Extend(HANDLE Volume, LONGLONG Sectors, DWORD InLength)
{
    DWORD Returned;
    if (DeviceIoControl(Volume, FSCTL_EXTEND_VOLUME, &Sectors, InLength ? InLength : sizeof(Sectors), NULL, 0, &Returned, NULL))
        return 0;
    return GetLastError();
}

static DWORD Grow(ULONG Disk, ULONG Number, LONGLONG Bytes)
{
    DISK_GROW_PARTITION Grow;
    DWORD Returned, Error = 0;
    HANDLE Handle = OpenDisk(Disk, GENERIC_READ | GENERIC_WRITE);

    Grow.PartitionNumber = Number;
    Grow.BytesToGrow.QuadPart = Bytes;
    if (!DeviceIoControl(Handle, IOCTL_DISK_GROW_PARTITION, &Grow, sizeof(Grow), NULL, 0, &Returned, NULL))
        Error = GetLastError();
    CloseHandle(Handle);
    return Error;
}

static BOOL PartitionOf(HANDLE Volume, PPARTITION_INFORMATION_EX Info)
{
    DWORD Returned;
    return DeviceIoControl(Volume, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, Info, sizeof(*Info), &Returned, NULL);
}

static BOOL NtfsData(HANDLE Volume, PNTFS_VOLUME_DATA_BUFFER Data)
{
    DWORD Returned;
    ZeroMemory(Data, sizeof(*Data));
    return DeviceIoControl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, Data, sizeof(*Data), &Returned, NULL);
}

static ULONGLONG TotalBytes(void)
{
    CHAR Root[4] = { Letter, ':', '\\', 0 };
    ULARGE_INTEGER Available, Total, Free;
    if (!GetDiskFreeSpaceExA(Root, &Available, &Total, &Free))
        return 0;
    return Total.QuadPart;
}

/* Writes Bytes of a pattern to a new file, reads them back and compares. */
static BOOL WriteAndVerify(LONGLONG Bytes)
{
    CHAR Name[32];
    BYTE *Buffer = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)MiB), *Check = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)MiB);
    LONGLONG Done;
    DWORD Count, i;
    HANDLE File;
    BOOL Ok = TRUE;

    sprintf(Name, "%c:\\extend.bin", Letter);
    File = CreateFileA(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    if (File == INVALID_HANDLE_VALUE || !Buffer || !Check)
        return FALSE;
    for (Done = 0; Ok && Done < Bytes; Done += MiB)
    {
        for (i = 0; i < MiB / sizeof(ULONG); i++)
            ((PULONG)Buffer)[i] = (ULONG)(Done / sizeof(ULONG)) + i;
        Ok = WriteFile(File, Buffer, (DWORD)MiB, &Count, NULL) && Count == MiB;
    }
    ok(Ok, "Writing %I64d bytes failed at %I64d: %lu\n", Bytes, Done, GetLastError());
    SetFilePointer(File, 0, NULL, FILE_BEGIN);
    for (Done = 0; Ok && Done < Bytes; Done += MiB)
    {
        for (i = 0; i < MiB / sizeof(ULONG); i++)
            ((PULONG)Buffer)[i] = (ULONG)(Done / sizeof(ULONG)) + i;
        Ok = ReadFile(File, Check, (DWORD)MiB, &Count, NULL) && Count == MiB && !memcmp(Buffer, Check, (SIZE_T)MiB);
    }
    ok(Ok, "Reading back failed at %I64d: %lu\n", Done, GetLastError());
    CloseHandle(File);
    DeleteFileA(Name);
    HeapFree(GetProcessHeap(), 0, Buffer);
    HeapFree(GetProcessHeap(), 0, Check);
    return Ok;
}

/* A file of Bytes of a pattern, written through the cache and left open (not flushed). */
static HANDLE WritePattern(PCSTR Leaf, LONGLONG Bytes)
{
    CHAR Name[32];
    PULONG Buffer = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)MiB);
    LONGLONG Done;
    DWORD Count, i;
    HANDLE File;

    sprintf(Name, "%c:\\%s", Letter, Leaf);
    File = CreateFileA(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    for (Done = 0; File != INVALID_HANDLE_VALUE && Buffer && Done < Bytes; Done += MiB)
    {
        for (i = 0; i < MiB / sizeof(ULONG); i++)
            Buffer[i] = 0x5A000000 ^ (ULONG)(Done / sizeof(ULONG) + i);
        if (!WriteFile(File, Buffer, (DWORD)MiB, &Count, NULL) || Count != MiB)
            break;
    }
    ok(Done >= Bytes, "Writing %s failed at %I64d: %lu\n", Name, Done, GetLastError());
    HeapFree(GetProcessHeap(), 0, Buffer);
    return File;
}

/* Reads the pattern of WritePattern back through File (opened without buffering) and closes it. */
static BOOL VerifyPatternIn(HANDLE File, PCSTR What, LONGLONG Bytes)
{
    PULONG Buffer = VirtualAlloc(NULL, (SIZE_T)MiB, MEM_COMMIT, PAGE_READWRITE);    /* sector-aligned, for uncached reads */
    LONGLONG Done;
    DWORD Count, i;
    BOOL Ok = TRUE;

    Ok = File != INVALID_HANDLE_VALUE && Buffer && GetFileSize(File, NULL) == (DWORD)Bytes;
    for (Done = 0; Ok && Done < Bytes; Done += MiB)
    {
        Ok = ReadFile(File, Buffer, (DWORD)MiB, &Count, NULL) && Count == MiB;
        for (i = 0; Ok && i < MiB / sizeof(ULONG); i++)
            Ok = Buffer[i] == (0x5A000000 ^ (ULONG)(Done / sizeof(ULONG) + i));
    }
    ok(Ok, "%s differs at %I64d (%lu)\n", What, Done, GetLastError());
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    if (Buffer)
        VirtualFree(Buffer, 0, MEM_RELEASE);
    return Ok;
}

static BOOL VerifyPattern(PCSTR Leaf, LONGLONG Bytes)
{
    CHAR Name[32];

    sprintf(Name, "%c:\\%s", Letter, Leaf);
    return VerifyPatternIn(CreateFileA(Name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL),
                           Name, Bytes);
}

/* Dismounts the volume through *Volume (locked first) and opens it again: the next access mounts what is on
 * the disk. A handle left open on the dismounted volume would keep the old mount alive. */
static BOOL Remount(HANDLE *Volume)
{
    DWORD Returned;

    if (!DeviceIoControl(*Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Returned, NULL))
        return FALSE;
    ok(DeviceIoControl(*Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Returned, NULL), "Dismount failed: %lu\n", GetLastError());
    CloseHandle(*Volume);
    *Volume = OpenVolume(GENERIC_READ | GENERIC_WRITE);
    ok(*Volume != INVALID_HANDLE_VALUE, "The volume did not open again: %lu\n", GetLastError());
    return TRUE;
}

/* The boot sector copy in the last sector of the volume's range, read through the disk. */
static BOOL BootCopyAt(ULONG Disk, LONGLONG Offset, LONGLONG Sectors)
{
    BYTE *Sector = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
    HANDLE Handle = OpenDisk(Disk, GENERIC_READ);
    LARGE_INTEGER Position;
    DWORD Read;
    BOOL Ok;

    Position.QuadPart = Offset;
    Ok = SetFilePointerEx(Handle, Position, NULL, FILE_BEGIN) && ReadFile(Handle, Sector, 512, &Read, NULL) && Read == 512 &&
         !memcmp(Sector + 3, "NTFS    ", 8) && *(LONGLONG UNALIGNED *)(Sector + 0x28) == Sectors;
    CloseHandle(Handle);
    VirtualFree(Sector, 0, MEM_RELEASE);
    return Ok;
}

/* GUID_DEVINTERFACE_VOLUME */
static const GUID VolumeInterfaceGuid = { 0x53f5630d, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

/* The device instance of the volume interface whose storage device number is Number's, or 0. */
static DEVINST VolumeDevInst(PSTORAGE_DEVICE_NUMBER Number)
{
    BYTE Buffer[1024];
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail = (PVOID)Buffer;
    SP_DEVICE_INTERFACE_DATA Interface;
    SP_DEVINFO_DATA Device;
    STORAGE_DEVICE_NUMBER Other;
    DEVINST Instance = 0;
    DWORD Index, Returned;
    HDEVINFO Set;
    HANDLE Handle;

    Set = SetupDiGetClassDevsW(&VolumeInterfaceGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (Set == INVALID_HANDLE_VALUE)
        return 0;
    Interface.cbSize = sizeof(Interface);
    for (Index = 0; !Instance && SetupDiEnumDeviceInterfaces(Set, NULL, &VolumeInterfaceGuid, Index, &Interface); Index++)
    {
        Detail->cbSize = sizeof(*Detail);
        Device.cbSize = sizeof(Device);
        if (!SetupDiGetDeviceInterfaceDetailW(Set, &Interface, Detail, sizeof(Buffer), NULL, &Device))
            continue;
        Handle = CreateFileW(Detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (Handle == INVALID_HANDLE_VALUE)
            continue;
        if (DeviceIoControl(Handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &Other, sizeof(Other), &Returned, NULL) &&
            Other.DeviceType == Number->DeviceType && Other.DeviceNumber == Number->DeviceNumber &&
            Other.PartitionNumber == Number->PartitionNumber)
        {
            Instance = Device.DevInst;
        }
        CloseHandle(Handle);
    }
    SetupDiDestroyDeviceInfoList(Set);
    return Instance;
}

/*
 * An orderly removal of the device under a mounted volume that has an open handle (the test's own
 * volume handle) is refused, as Windows file systems refuse it, and the volume stays.  On ReactOS
 * the file system answers the query (it comes through the volume's VPB); the request from user mode
 * is not implemented there yet, so the test is skipped.
 */
static void TestQueryRemoveBusy(PSTORAGE_DEVICE_NUMBER Number)
{
    PNP_VETO_TYPE Veto = PNP_VetoTypeUnknown;
    WCHAR Name[MAX_PATH] = L"";
    DEVINST Instance = VolumeDevInst(Number);
    CONFIGRET Result;

    if (!Instance)
    {
        skip("No volume device for the scratch volume\n");
        return;
    }
    Result = CM_Query_And_Remove_SubTreeW(Instance, &Veto, Name, RTL_NUMBER_OF(Name), CM_REMOVE_NO_RESTART);
    if (Result == CR_CALL_NOT_IMPLEMENTED && is_reactos())
    {
        /* PiControlQueryRemoveDevice is a stub: the query cannot be sent from user mode yet */
        skip("An orderly removal cannot be requested from user mode\n");
        return;
    }
    ok(Result == CR_REMOVE_VETOED, "The removal of a volume with an open handle was not refused: %lu\n", Result);
    ok(TotalBytes() != 0, "The volume is gone: %lu\n", GetLastError());
}

/* TRUE when the volume grew as expected (TestNtfsShrink depends on the free space it added). */
static BOOL TestNtfs(HANDLE Volume, ULONG Disk, PPARTITION_INFORMATION_EX Part)
{
    NTFS_VOLUME_DATA_BUFFER Before, After;
    LONGLONG Sectors, Length;
    ULONGLONG TotalBefore;

    if (!NtfsData(Volume, &Before))
    {
        ok(FALSE, "FSCTL_GET_NTFS_VOLUME_DATA failed: %lu\n", GetLastError());
        return FALSE;
    }
    Sectors = Part->PartitionLength.QuadPart / 512;
    TotalBefore = TotalBytes();

    /* The input counts the boot sector copy: one sector more than the volume. */
    ok_int(Extend(Volume, Before.NumberSectors.QuadPart + 1, 0), ERROR_SUCCESS);
    ok_int(Extend(Volume, Sectors, 0), ERROR_SUCCESS);
    ok_int(Extend(Volume, Sectors - 16 * 2048, 0), ERROR_INVALID_PARAMETER);
    NtfsData(Volume, &After);
    ok(After.NumberSectors.QuadPart == Before.NumberSectors.QuadPart, "NumberSectors %I64d, was %I64d\n",
       After.NumberSectors.QuadPart, Before.NumberSectors.QuadPart);

    if (Grow(Disk, Part->PartitionNumber, 32 * MiB) != ERROR_SUCCESS)
    {
        ok(FALSE, "IOCTL_DISK_GROW_PARTITION failed: %lu\n", GetLastError());
        return FALSE;
    }
    PartitionOf(Volume, Part);
    Length = Part->PartitionLength.QuadPart;
    ok(Length == Sectors * 512 + 32 * MiB, "Partition is %I64d bytes\n", Length);
    NtfsData(Volume, &After);
    ok(After.TotalClusters.QuadPart == Before.TotalClusters.QuadPart, "The volume grew before the extend\n");

    ok_int(Extend(Volume, Length / 512 - 1, 4), ERROR_INVALID_PARAMETER);
    ok_int(Extend(Volume, Length / 512 + 2048, 0), ERROR_INVALID_PARAMETER);
    ok_int(Extend(Volume, Length / 512, 0), ERROR_SUCCESS);
    NtfsData(Volume, &After);
    ok(After.NumberSectors.QuadPart == Length / 512 - 1, "NumberSectors %I64d, expected %I64d\n",
       After.NumberSectors.QuadPart, Length / 512 - 1);
    if (After.NumberSectors.QuadPart != Length / 512 - 1)
        return FALSE;
    ok(After.TotalClusters.QuadPart == After.NumberSectors.QuadPart * 512 / After.BytesPerCluster,
       "TotalClusters %I64d for %I64d sectors\n", After.TotalClusters.QuadPart, After.NumberSectors.QuadPart);
    ok(After.FreeClusters.QuadPart >= Before.FreeClusters.QuadPart + (After.TotalClusters.QuadPart - Before.TotalClusters.QuadPart) - 16,
       "FreeClusters %I64d, was %I64d\n", After.FreeClusters.QuadPart, Before.FreeClusters.QuadPart);
    ok(TotalBytes() >= TotalBefore + 32 * MiB - 1 * MiB, "Total %I64u, was %I64u\n", TotalBytes(), TotalBefore);
    ok(BootCopyAt(Disk, Part->StartingOffset.QuadPart + After.NumberSectors.QuadPart * 512, After.NumberSectors.QuadPart),
       "No boot sector copy after the volume\n");

    /* The same size again, and one sector more: no new cluster, no change. */
    ok_int(Extend(Volume, Length / 512, 0), ERROR_SUCCESS);
    NtfsData(Volume, &Before);
    ok(Before.NumberSectors.QuadPart == After.NumberSectors.QuadPart, "NumberSectors %I64d\n", Before.NumberSectors.QuadPart);

    /* The new space holds data: more than the old volume could. */
    return WriteAndVerify(Sectors * 512 + 8 * MiB);
}

static DWORD Shrink(HANDLE Volume, SHRINK_VOLUME_REQUEST_TYPES Type, LONGLONG Sectors, DWORD InLength)
{
    SHRINK_VOLUME_INFORMATION Info;
    DWORD Returned;

    ZeroMemory(&Info, sizeof(Info));
    Info.ShrinkRequestType = Type;
    Info.NewNumberOfSectors = Sectors;
    if (DeviceIoControl(Volume, FSCTL_SHRINK_VOLUME, &Info, InLength ? InLength : sizeof(Info), NULL, 0, &Returned, NULL))
        return 0;
    return GetLastError();
}

/*
 * Shrinking the NTFS volume (after TestNtfs, which left free space at its end): the size counts the
 * boot sector copy as for FSCTL_EXTEND_VOLUME; a prepared request belongs to its handle.
 */
static void TestNtfsShrink(HANDLE Volume, ULONG Disk, PPARTITION_INFORMATION_EX Part)
{
    NTFS_VOLUME_DATA_BUFFER Data;
    HANDLE Other, Third;
    LONGLONG Sectors, Target, Length;

    if (!NtfsData(Volume, &Data))
    {
        ok(FALSE, "FSCTL_GET_NTFS_VOLUME_DATA failed: %lu\n", GetLastError());
        return;
    }
    Sectors = Data.NumberSectors.QuadPart;
    Target = Sectors + 1 - 16 * 2048;
    Other = OpenVolume(GENERIC_READ | GENERIC_WRITE);

    ok_int(Shrink(Volume, ShrinkCommit, 0, 0), ERROR_INVALID_PARAMETER);
    ok_int(Shrink(Volume, ShrinkAbort, 0, 0), ERROR_INVALID_PARAMETER);
    ok_int(Shrink(Volume, ShrinkPrepare, Target, 8), ERROR_INVALID_PARAMETER);
    ok_int(Shrink(Volume, ShrinkPrepare, Sectors + 1 + 2048, 0), ERROR_INVALID_PARAMETER);
    ok_int(Shrink(Volume, ShrinkPrepare, Sectors + 1, 0), ERROR_INVALID_PARAMETER);

    /* Prepared on one handle: that handle may prepare again; another one is busy and has nothing to commit. */
    ok_int(Shrink(Volume, ShrinkPrepare, Target - 2048, 0), ERROR_SUCCESS);
    ok_int(Shrink(Volume, ShrinkPrepare, Target, 0), ERROR_SUCCESS);
    ok_int(Shrink(Other, ShrinkPrepare, Target, 0), ERROR_BUSY);
    ok_int(Shrink(Other, ShrinkAbort, 0, 0), ERROR_INVALID_PARAMETER);
    ok_int(Shrink(Other, ShrinkCommit, 0, 0), ERROR_INVALID_PARAMETER);
    NtfsData(Volume, &Data);
    ok(Data.NumberSectors.QuadPart == Sectors, "Prepare changed the size to %I64d\n", Data.NumberSectors.QuadPart);

    if (Shrink(Volume, ShrinkCommit, 0, 0) != ERROR_SUCCESS)
    {
        /* The partition must not shrink under a volume that did not. */
        ok(FALSE, "ShrinkCommit failed: %lu\n", GetLastError());
        Shrink(Volume, ShrinkAbort, 0, 0);
        CloseHandle(Other);
        return;
    }
    NtfsData(Volume, &Data);
    ok(Data.NumberSectors.QuadPart == Target - 1, "NumberSectors %I64d, expected %I64d\n", Data.NumberSectors.QuadPart, Target - 1);
    ok(Data.TotalClusters.QuadPart == (Target - 1) * 512 / Data.BytesPerCluster, "TotalClusters %I64d\n", Data.TotalClusters.QuadPart);
    ok(BootCopyAt(Disk, Part->StartingOffset.QuadPart + (Target - 1) * 512, Target - 1), "No boot sector copy after the volume\n");
    ok_int(Shrink(Volume, ShrinkAbort, 0, 0), ERROR_INVALID_PARAMETER);

    /* A request of a handle that closed is gone. */
    Third = OpenVolume(GENERIC_READ | GENERIC_WRITE);
    ok_int(Shrink(Third, ShrinkPrepare, Target - 2048, 0), ERROR_SUCCESS);
    CloseHandle(Third);
    ok_int(Shrink(Other, ShrinkPrepare, Target - 2048, 0), ERROR_SUCCESS);
    ok_int(Shrink(Other, ShrinkAbort, 0, 0), ERROR_SUCCESS);
    CloseHandle(Other);

    /* The partition follows; the volume works at its new size. */
    Length = Part->PartitionLength.QuadPart;
    ok_int(Grow(Disk, Part->PartitionNumber, Target * 512 - Length), ERROR_SUCCESS);
    PartitionOf(Volume, Part);
    ok(Part->PartitionLength.QuadPart == Target * 512, "Partition is %I64d bytes\n", Part->PartitionLength.QuadPart);
    WriteAndVerify(16 * MiB);

    /* Data past the new end: the commit is refused and the request stays until aborted. */
    NtfsData(Volume, &Data);
    Sectors = Data.NumberSectors.QuadPart;
    ok_int(Shrink(Volume, ShrinkPrepare, Sectors / 10, 0), ERROR_SUCCESS);
    ok_int(Shrink(Volume, ShrinkCommit, 0, 0), ERROR_ACCESS_DENIED);
    ok_int(Shrink(Volume, ShrinkAbort, 0, 0), ERROR_SUCCESS);
    NtfsData(Volume, &Data);
    ok(Data.NumberSectors.QuadPart == Sectors, "NumberSectors %I64d, was %I64d\n", Data.NumberSectors.QuadPart, Sectors);
}

/* The GUID path (with its trailing backslash) of the volume whose one extent is Length bytes at Offset on disk Disk. */
static BOOL FindVolumeAt(ULONG Disk, LONGLONG Offset, LONGLONG Length, PWCHAR Guid, DWORD Count)
{
    union { VOLUME_DISK_EXTENTS Extents; BYTE Raw[512]; } Buffer;
    WCHAR Device[MAX_PATH];
    DWORD Returned;
    HANDLE Find, Handle;
    BOOL Found = FALSE;
    SIZE_T Len;

    Find = FindFirstVolumeW(Guid, Count);
    if (Find == INVALID_HANDLE_VALUE)
        return FALSE;
    do
    {
        wcscpy(Device, Guid);
        Len = wcslen(Device);
        if (Len && Device[Len - 1] == L'\\')
            Device[Len - 1] = 0;
        Handle = CreateFileW(Device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (Handle == INVALID_HANDLE_VALUE)
            continue;
        ZeroMemory(&Buffer, sizeof(Buffer));
        Found = DeviceIoControl(Handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &Buffer, sizeof(Buffer), &Returned, NULL) &&
                Buffer.Extents.NumberOfDiskExtents == 1 && Buffer.Extents.Extents[0].DiskNumber == Disk &&
                Buffer.Extents.Extents[0].StartingOffset.QuadPart == Offset &&
                Buffer.Extents.Extents[0].ExtentLength.QuadPart == Length;
        CloseHandle(Handle);
    } while (!Found && FindNextVolumeW(Find, Guid, Count));
    FindVolumeClose(Find);
    return Found;
}

/* Writes, flushes and renames a file through one handle until told to stop, while the volume goes away. */
typedef struct _RACE
{
    HANDLE File;
    volatile LONG Stop;
    LONG Done, Failed;
} RACE, *PRACE;

static DWORD WINAPI RaceThread(PVOID Context)
{
    PRACE Race = Context;
    static BYTE Data[64 * 1024];
    struct { FILE_RENAME_INFORMATION Info; WCHAR More[16]; } Rename;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Zero;
    DWORD Written;
    ULONG n;

    Zero.QuadPart = 0;
    for (n = 0; !Race->Stop && n < 100000; n++)
    {
        BOOL Ok = SetFilePointerEx(Race->File, Zero, NULL, FILE_BEGIN) &&
                  WriteFile(Race->File, Data, sizeof(Data), &Written, NULL) && FlushFileBuffers(Race->File);
        ZeroMemory(&Rename, sizeof(Rename));
        Rename.Info.ReplaceIfExists = TRUE;
        Rename.Info.FileNameLength = 9 * sizeof(WCHAR);
        memcpy(Rename.Info.FileName, (n & 1) ? L"race1.bin" : L"race2.bin", 9 * sizeof(WCHAR));
        Ok = NT_SUCCESS(NtSetInformationFile(Race->File, &Iosb, &Rename, sizeof(Rename), FileRenameInformation)) && Ok;
        InterlockedIncrement(Ok ? &Race->Done : &Race->Failed);
    }
    return 0;
}

/*
 * After a shrink the partition table is written again with the smaller partition while the volume
 * is mounted.  Windows Server 2008 R2 then removes the volume and makes it anew, without the drive
 * letter it had (it gets the next free one), unlike a shrink through IOCTL_DISK_GROW_PARTITION,
 * which keeps the volume (TestNtfsShrink).  The new volume holds the shrunk file system with its
 * files.  On ReactOS the file system on the old partition device gives it up at the removal, and
 * the removal reaches the partition manager.
 */
static void TestNtfsShrinkTable(HANDLE *Volume, ULONG Disk, PPARTITION_INFORMATION_EX Part)
{
    BYTE Buffer[8192];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PVOID)Buffer;
    NTFS_VOLUME_DATA_BUFFER Data;
    LONGLONG Target = 0, Start = Part->StartingOffset.QuadPart;
    WCHAR Guid[MAX_PATH], Path[MAX_PATH];
    DWORD Returned, i;
    HANDLE File, Handle, RaceThreadHandle = NULL;
    BOOL Found = FALSE, Back = FALSE;
    RACE Race = { INVALID_HANDLE_VALUE, 0, 0, 0 };

    File = WritePattern("table.bin", 4 * MiB);
    if (File != INVALID_HANDLE_VALUE)
        CloseHandle(File);
    if (!NtfsData(*Volume, &Data))
    {
        ok(FALSE, "FSCTL_GET_NTFS_VOLUME_DATA failed: %lu\n", GetLastError());
        return;
    }
    Target = Data.NumberSectors.QuadPart + 1 - 8 * 2048;
    ok_int(Shrink(*Volume, ShrinkPrepare, Target, 0), ERROR_SUCCESS);
    if (Shrink(*Volume, ShrinkCommit, 0, 0) != ERROR_SUCCESS)
    {
        ok(FALSE, "ShrinkCommit failed: %lu\n", GetLastError());
        Shrink(*Volume, ShrinkAbort, 0, 0);
        return;
    }
    ok(FlushFileBuffers(*Volume), "Flushing the volume failed: %lu\n", GetLastError());
    CloseHandle(*Volume);
    *Volume = INVALID_HANDLE_VALUE;

    /* A file of the old volume is written, flushed and renamed all the time while it goes away: the
     * requests that are under way then must fail, not crash. */
    sprintf((PCHAR)Buffer, "%c:\\race1.bin", Letter);
    Race.File = CreateFileA((PCSTR)Buffer, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(Race.File != INVALID_HANDLE_VALUE, "Creating %s failed: %lu\n", (PCSTR)Buffer, GetLastError());
    RaceThreadHandle = Race.File != INVALID_HANDLE_VALUE ? CreateThread(NULL, 0, RaceThread, &Race, 0, NULL) : NULL;
    for (i = 0; RaceThreadHandle && i < 100 && !Race.Done; i++)
        Sleep(50);

    Handle = OpenDisk(Disk, GENERIC_READ | GENERIC_WRITE);
    if (!DeviceIoControl(Handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, Layout, sizeof(Buffer), &Returned, NULL))
    {
        ok(FALSE, "IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed: %lu\n", GetLastError());
        CloseHandle(Handle);
        return;
    }
    for (i = 0; i < Layout->PartitionCount; i++)
    {
        Layout->PartitionEntry[i].RewritePartition = FALSE;
        if (Layout->PartitionEntry[i].StartingOffset.QuadPart == Start && Layout->PartitionEntry[i].PartitionLength.QuadPart)
        {
            Layout->PartitionEntry[i].PartitionLength.QuadPart = Target * 512;
            Layout->PartitionEntry[i].RewritePartition = TRUE;
            Found = TRUE;
        }
    }
    ok(Found, "No layout entry at %I64d\n", Start);
    ok(DeviceIoControl(Handle, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, Layout,
                       FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry[Layout->PartitionCount]),
                       NULL, 0, &Returned, NULL),
       "IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed: %lu\n", GetLastError());
    DeviceIoControl(Handle, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &Returned, NULL);
    CloseHandle(Handle);

    /* The new volume may take a moment to appear. */
    for (i = 0; i < 60 && !Back; i++)
    {
        Back = FindVolumeAt(Disk, Start, Target * 512, Guid, MAX_PATH);
        if (!Back)
            Sleep(500);
    }
    if (RaceThreadHandle)
    {
        LONG Before = Race.Failed;
        DWORD Written;

        Sleep(1000);
        InterlockedExchange(&Race.Stop, 1);
        ok(WaitForSingleObject(RaceThreadHandle, 30000) == WAIT_OBJECT_0, "The racing requests did not end\n");
        CloseHandle(RaceThreadHandle);
        trace("racing requests: %ld succeeded, %ld failed (%ld before the new volume was there)\n", Race.Done, Race.Failed, Before);
        ok(Race.Done > 0, "No request on the old volume succeeded before the table changed\n");
        ok(!WriteFile(Race.File, Buffer, 512, &Written, NULL), "A write to the removed volume succeeded\n");
    }
    if (Race.File != INVALID_HANDLE_VALUE)
        CloseHandle(Race.File);
    ok(Back, "No volume with a %I64d-byte partition at %I64d\n", Target * 512, Start);
    if (!Back)
        return;
    wcscpy(Path, Guid);
    Path[wcslen(Path) - 1] = 0;
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "The new volume does not open: %lu\n", GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return;
    ok(NtfsData(Handle, &Data), "No NTFS on the new volume: %lu\n", GetLastError());
    ok(Data.NumberSectors.QuadPart == Target - 1, "NumberSectors %I64d, expected %I64d\n", Data.NumberSectors.QuadPart, Target - 1);
    CloseHandle(Handle);
    wcscat(Guid, L"table.bin");
    ok(VerifyPatternIn(CreateFileW(Guid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL),
                       "table.bin on the new volume", 4 * MiB),
       "The file did not survive the new table\n");
}

/*
 * ReactOS only, with the driver's test hook armed (PARTMGR_TEST_FAULT names the stage, the same value
 * in the ResizeFault DWORD of the Ntfs service key): a resize that fails half way (an extend, or for
 * stage 3 a shrink) leaves the volume read-only for every caller, cached writes included, and a flush
 * of data written before it fails.  What the disk holds is the last state that reached it whole, so
 * the volume mounts again read-write, with the data written before the resize.  Stage 5 (a degraded
 * journal) refuses the extend before anything changes; stage 7 fails the boot-sector commit of an
 * extend that ends half way into a page.
 */
static void TestNtfsFault(HANDLE *Volume, ULONG Disk, PPARTITION_INFORMATION_EX Part, ULONG Fault)
{
    NTFS_VOLUME_DATA_BUFFER Data;
    CHAR Name[32];
    HANDLE File, Dirty;
    DWORD Written, Error;
    BYTE Buffer[4096];

    sprintf(Name, "%c:\\fault.bin", Letter);
    File = WritePattern("fault.bin", 1 * MiB);
    if (File != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(File);
        CloseHandle(File);
    }
    /* Data in the cache only when the resize fails. */
    Dirty = WritePattern("dirty.bin", 1 * MiB);
    if (!NtfsData(*Volume, &Data) || Grow(Disk, Part->PartitionNumber, 32 * MiB + (Fault == 7 ? 512 : 0)) != ERROR_SUCCESS)
    {
        ok(FALSE, "IOCTL_DISK_GROW_PARTITION failed: %lu\n", GetLastError());
        CloseHandle(Dirty);
        return;
    }
    PartitionOf(*Volume, Part);
    if (Fault == 3)
    {
        /* The shrink back to the old size fails half way. */
        ok_int(Extend(*Volume, Part->PartitionLength.QuadPart / 512, 0), ERROR_SUCCESS);
        ok_int(Shrink(*Volume, ShrinkPrepare, Data.NumberSectors.QuadPart + 1, 0), ERROR_SUCCESS);
        Error = Shrink(*Volume, ShrinkCommit, 0, 0);
        ok(Error != ERROR_SUCCESS, "The armed shrink succeeded\n");
    }
    else
    {
        Error = Extend(*Volume, Part->PartitionLength.QuadPart / 512, 0);
        ok(Error != ERROR_SUCCESS, "The armed extend succeeded\n");
    }
    if (Fault == 5)
    {
        /* Refused before any change: the volume keeps its size and stays writable. */
        NTFS_VOLUME_DATA_BUFFER After;
        NtfsData(*Volume, &After);
        ok(After.NumberSectors.QuadPart == Data.NumberSectors.QuadPart, "NumberSectors %I64d, was %I64d\n",
           After.NumberSectors.QuadPart, Data.NumberSectors.QuadPart);
        ok(FlushFileBuffers(Dirty), "A flush after the refused extend failed: %lu\n", GetLastError());
        CloseHandle(Dirty);
        File = WritePattern("after.bin", 1 * MiB);
        if (File != INVALID_HANDLE_VALUE)
        {
            ok(FlushFileBuffers(File), "A flush after the refused extend failed: %lu\n", GetLastError());
            CloseHandle(File);
        }
        return;
    }

    /* The cached data can no longer reach the disk: its flush fails. */
    SetLastError(0xdeadbeef);
    ok(!FlushFileBuffers(Dirty), "A flush after the failed resize succeeded\n");
    CloseHandle(Dirty);

    /* An overwrite inside the file would go to the cache and be lost later: it must be refused. */
    File = CreateFileA(Name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        ok_int(GetLastError(), ERROR_WRITE_PROTECT);
    }
    else
    {
        memset(Buffer, 0xA5, sizeof(Buffer));
        ok(!WriteFile(File, Buffer, sizeof(Buffer), &Written, NULL), "A write after the failed resize succeeded\n");
        ok_int(GetLastError(), ERROR_WRITE_PROTECT);
        CloseHandle(File);
    }

    /* Mounted again, the volume is writable and holds the data flushed before the resize. */
    if (!Remount(Volume))
    {
        ok(FALSE, "The volume could not be locked for a remount: %lu\n", GetLastError());
        return;
    }
    VerifyPattern("fault.bin", 1 * MiB);
    File = WritePattern("after.bin", 1 * MiB);
    if (File != INVALID_HANDLE_VALUE)
    {
        ok(FlushFileBuffers(File), "A flush after the remount failed: %lu\n", GetLastError());
        CloseHandle(File);
        VerifyPattern("after.bin", 1 * MiB);
    }
}

/*
 * ReactOS only, PARTMGR_TEST_HUGE set: the volume (NTFS, 8 KB clusters) is on a disk of more than
 * 16 TiB.  ntfsng mounts nothing of 16 TiB or more (its page index is 32 bits), so an extend on a
 * partition that large is refused and the volume keeps its size; just below the limit it works.
 */
static void TestNtfsHuge(HANDLE Volume, ULONG Disk, PPARTITION_INFORMATION_EX Part)
{
    const LONGLONG Limit = 1LL << 44;
    GET_LENGTH_INFORMATION DiskLength;
    NTFS_VOLUME_DATA_BUFFER Before, After;
    HANDLE Handle = OpenDisk(Disk, GENERIC_READ);
    DWORD Returned;
    LONGLONG End, Length;

    if (!DeviceIoControl(Handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &DiskLength, sizeof(DiskLength), &Returned, NULL) ||
        DiskLength.Length.QuadPart < Limit + 64 * MiB || !NtfsData(Volume, &Before))
    {
        skip("The disk of the volume is not larger than 16 TiB: %lu\n", GetLastError());
        CloseHandle(Handle);
        return;
    }
    CloseHandle(Handle);

    /* Everything after the partition but the backup GPT's last MiB. */
    End = Part->StartingOffset.QuadPart + Part->PartitionLength.QuadPart;
    ok_int(Grow(Disk, Part->PartitionNumber, DiskLength.Length.QuadPart - MiB - End), ERROR_SUCCESS);
    PartitionOf(Volume, Part);
    Length = Part->PartitionLength.QuadPart;
    ok(Length >= Limit, "Partition is %I64d bytes\n", Length);
    ok(Extend(Volume, Length / 512, 0) != ERROR_SUCCESS, "An extend to %I64d bytes succeeded\n", Length);
    ok(Extend(Volume, Limit / 512, 0) != ERROR_SUCCESS, "An extend to 16 TiB succeeded\n");
    NtfsData(Volume, &After);
    ok(After.NumberSectors.QuadPart == Before.NumberSectors.QuadPart, "NumberSectors %I64d, was %I64d\n",
       After.NumberSectors.QuadPart, Before.NumberSectors.QuadPart);

    /* 1 MiB below the limit: the volume takes the whole partition. */
    ok_int(Grow(Disk, Part->PartitionNumber, Limit - MiB - Length), ERROR_SUCCESS);
    PartitionOf(Volume, Part);
    Length = Part->PartitionLength.QuadPart;
    ok(Length == Limit - MiB, "Partition is %I64d bytes\n", Length);
    ok_int(Extend(Volume, Length / 512, 0), ERROR_SUCCESS);
    NtfsData(Volume, &After);
    ok(After.NumberSectors.QuadPart == Length / 512 - 1, "NumberSectors %I64d, expected %I64d\n",
       After.NumberSectors.QuadPart, Length / 512 - 1);
    WriteAndVerify(16 * MiB);
}

static void TestFat(HANDLE *Volume, ULONG Disk, PPARTITION_INFORMATION_EX Part, PCSTR FileSystem)
{
    LONGLONG Sectors = Part->PartitionLength.QuadPart / 512, Length;
    ULONGLONG TotalBefore = TotalBytes();
    HANDLE Pending;
    DWORD Error;

    if (!is_reactos())
    {
        ok_int(Extend(*Volume, Sectors, 0), ERROR_INVALID_FUNCTION);
        return;
    }

    ok_int(Extend(*Volume, Sectors, 0), ERROR_SUCCESS);
    ok_int(Extend(*Volume, Sectors - 2048, 0), ERROR_INVALID_PARAMETER);
    if (Grow(Disk, Part->PartitionNumber, 32 * MiB) != ERROR_SUCCESS)
    {
        ok(FALSE, "IOCTL_DISK_GROW_PARTITION failed: %lu\n", GetLastError());
        return;
    }
    PartitionOf(*Volume, Part);
    Length = Part->PartitionLength.QuadPart;
    ok_int(Extend(*Volume, Length / 512 + 2048, 0), ERROR_INVALID_PARAMETER);
    ok_int(Extend(*Volume, Length / 512, 4), ERROR_INVALID_PARAMETER);

    /* A file whose clusters are allocated but whose FAT changes are still in the cache. */
    Pending = WritePattern("pending.bin", 8 * MiB);
    Error = Extend(*Volume, Length / 512, 0);
    if (Pending != INVALID_HANDLE_VALUE)
        CloseHandle(Pending);
    if (Error == ERROR_DISK_FULL)
    {
        skip("%s volume: its FATs have no room for the new clusters\n", FileSystem);
        return;
    }
    ok_int(Error, ERROR_SUCCESS);
    ok(TotalBytes() >= TotalBefore + 32 * MiB - 1 * MiB, "Total %I64u, was %I64u\n", TotalBytes(), TotalBefore);

    /* The extend kept those FAT changes: the file reads back after a fresh mount. */
    if (Remount(Volume))
        VerifyPattern("pending.bin", 8 * MiB);
    else
        skip("The volume could not be locked for a remount: %lu\n", GetLastError());
    WriteAndVerify(Sectors * 512);
}

START_TEST(ExtendVolume)
{
    CHAR Value[8], Root[4], FileSystem[16];
    PARTITION_INFORMATION_EX Part;
    STORAGE_DEVICE_NUMBER Number;
    DISK_GEOMETRY Geometry;
    DWORD Returned;
    HANDLE Volume;

    if (!GetEnvironmentVariableA("PARTMGR_TEST_VOLUME", Value, sizeof(Value)))
    {
        skip("PARTMGR_TEST_VOLUME is not set: no scratch volume to grow\n");
        return;
    }
    Letter = Value[0];
    sprintf(Root, "%c:\\", Letter);
    if (!GetVolumeInformationA(Root, NULL, 0, NULL, NULL, NULL, FileSystem, sizeof(FileSystem)))
    {
        skip("No volume %s: %lu\n", Root, GetLastError());
        return;
    }

    Volume = OpenVolume(GENERIC_READ | GENERIC_WRITE);
    if (Volume == INVALID_HANDLE_VALUE || !PartitionOf(Volume, &Part) ||
        !DeviceIoControl(Volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &Number, sizeof(Number), &Returned, NULL))
    {
        skip("Volume %s is not a partition: %lu\n", Root, GetLastError());
        return;
    }
    trace("%s %s: disk %lu partition %lu, %I64d bytes at %I64d\n", Root, FileSystem, Number.DeviceNumber,
          Part.PartitionNumber, Part.PartitionLength.QuadPart, Part.StartingOffset.QuadPart);

    /* The sizes below are in 512-byte sectors. */
    if (!DeviceIoControl(Volume, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Geometry, sizeof(Geometry), &Returned, NULL) ||
        Geometry.BytesPerSector != 512)
    {
        skip("Volume %s is not on a 512-byte-sector disk\n", Root);
        CloseHandle(Volume);
        return;
    }

    if (!strcmp(FileSystem, "NTFS"))
    {
        if (GetEnvironmentVariableA("PARTMGR_TEST_FAULT", Value, sizeof(Value)) && is_reactos())
            TestNtfsFault(&Volume, Number.DeviceNumber, &Part, strtoul(Value, NULL, 10));
        else if (GetEnvironmentVariableA("PARTMGR_TEST_HUGE", Value, sizeof(Value)) && is_reactos())
            TestNtfsHuge(Volume, Number.DeviceNumber, &Part);
        else
        {
            TestQueryRemoveBusy(&Number);
            if (TestNtfs(Volume, Number.DeviceNumber, &Part))
            {
                TestNtfsShrink(Volume, Number.DeviceNumber, &Part);
                TestNtfsShrinkTable(&Volume, Number.DeviceNumber, &Part);
            }
            else
                skip("NTFS volume did not grow: no shrink tests\n");
        }
    }
    else if (!strncmp(FileSystem, "FAT", 3))
        TestFat(&Volume, Number.DeviceNumber, &Part, FileSystem);
    else
        skip("No expectations for %s\n", FileSystem);

    if (Volume != INVALID_HANDLE_VALUE)
        CloseHandle(Volume);
}
