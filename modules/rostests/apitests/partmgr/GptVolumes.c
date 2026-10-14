/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the volumes of the partitions on GPT disks
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <winioctl.h>
#include <mountmgr.h>

/* Read-only: it needs a GPT disk to be attached and skips otherwise */

static const GUID BasicDataGuid =
    { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };
static const GUID SystemPartitionTypes[] =
{
    { 0xc12a7328, 0xf81f, 0x11d2, { 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b } }, /* EFI system */
    { 0xe3c9e316, 0x0b5c, 0x4db8, { 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae } }, /* Microsoft reserved */
    { 0xde94bba4, 0x06d1, 0x4d40, { 0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac } }, /* Windows recovery */
};

static BOOL IsSystemPartitionType(const GUID *Type)
{
    ULONG i;

    for (i = 0; i < _countof(SystemPartitionTypes); i++)
    {
        if (IsEqualGUID(Type, &SystemPartitionTypes[i]))
            return TRUE;
    }
    return FALSE;
}

#define GPT_ATTR_HIDDEN          0x4000000000000000ULL
#define GPT_ATTR_NO_DRIVE_LETTER 0x8000000000000000ULL

typedef enum { FsNone, FsFat, FsFat32, FsNtfs, FsBtrfs } ON_DISK_FS;

static PCWSTR FsName(ON_DISK_FS Fs)
{
    switch (Fs)
    {
        case FsFat:   return L"FAT";
        case FsFat32: return L"FAT32";
        case FsNtfs:  return L"NTFS";
        case FsBtrfs: return L"btrfs";
        default:      return L"(none)";
    }
}

static BOOL ReadAt(HANDLE Disk, LONGLONG Offset, PVOID Buffer, DWORD Length)
{
    LARGE_INTEGER Pos;
    DWORD Read = 0;

    Pos.QuadPart = Offset;
    if (!SetFilePointerEx(Disk, Pos, NULL, FILE_BEGIN))
        return FALSE;
    return ReadFile(Disk, Buffer, Length, &Read, NULL) && Read == Length;
}

static ON_DISK_FS ClassifyPartition(HANDLE Disk, LONGLONG Start, LONGLONG Length, PUCHAR Buffer)
{
    if (!ReadAt(Disk, Start, Buffer, 4096))
        return FsNone;
    if (Buffer[510] == 0x55 && Buffer[511] == 0xAA)
    {
        if (!memcmp(Buffer + 3, "NTFS    ", 8))
            return FsNtfs;
        if (!memcmp(Buffer + 0x52, "FAT32   ", 8))
            return FsFat32;
        if (!memcmp(Buffer + 0x36, "FAT12   ", 8) || !memcmp(Buffer + 0x36, "FAT16   ", 8))
            return FsFat;
    }
    if (Length > 0x10000 + 4096 &&
        ReadAt(Disk, Start + 0x10000, Buffer, 4096) &&
        !memcmp(Buffer + 0x40, "_BHRfS_M", 8))
    {
        return FsBtrfs;
    }
    return FsNone;
}

/* Symbolic links the mount manager has for DeviceName: drive letters and volume GUID names */
static BOOL QueryMountPoints(PCWSTR DeviceName, const GUID *PartitionId, PULONG Letters, PULONG Guids,
                             PWCHAR Letter, PBOOL GuidIsPartitionId)
{
    WCHAR Expected[64];
    HANDLE MountMgr;
    UCHAR InBuffer[sizeof(MOUNTMGR_MOUNT_POINT) + 512];
    PMOUNTMGR_MOUNT_POINT In = (PMOUNTMGR_MOUNT_POINT)InBuffer;
    PMOUNTMGR_MOUNT_POINTS Out;
    DWORD OutSize = 0x10000, Returned, i;
    USHORT NameLength = (USHORT)(wcslen(DeviceName) * sizeof(WCHAR));
    BOOL Ret;

    *Letters = *Guids = 0;
    *Letter = 0;
    *GuidIsPartitionId = FALSE;
    _snwprintf(Expected, _countof(Expected),
               L"\\??\\Volume{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
               PartitionId->Data1, PartitionId->Data2, PartitionId->Data3,
               PartitionId->Data4[0], PartitionId->Data4[1], PartitionId->Data4[2], PartitionId->Data4[3],
               PartitionId->Data4[4], PartitionId->Data4[5], PartitionId->Data4[6], PartitionId->Data4[7]);
    if (NameLength > 512)
        return FALSE;
    MountMgr = CreateFileW(MOUNTMGR_DOS_DEVICE_NAME, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (MountMgr == INVALID_HANDLE_VALUE)
        return FALSE;
    ZeroMemory(InBuffer, sizeof(InBuffer));
    In->DeviceNameOffset = sizeof(MOUNTMGR_MOUNT_POINT);
    In->DeviceNameLength = NameLength;
    memcpy(InBuffer + sizeof(MOUNTMGR_MOUNT_POINT), DeviceName, NameLength);
    Out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, OutSize);
    Ret = Out && DeviceIoControl(MountMgr, IOCTL_MOUNTMGR_QUERY_POINTS,
                                 In, sizeof(MOUNTMGR_MOUNT_POINT) + NameLength,
                                 Out, OutSize, &Returned, NULL);
    if (Ret)
    {
        for (i = 0; i < Out->NumberOfMountPoints; i++)
        {
            PWCHAR Link = (PWCHAR)((PUCHAR)Out + Out->MountPoints[i].SymbolicLinkNameOffset);
            USHORT Chars = Out->MountPoints[i].SymbolicLinkNameLength / sizeof(WCHAR);

            trace("    mount point %s\n", wine_dbgstr_wn(Link, Chars));
            if (Chars == 14 && !_wcsnicmp(Link, L"\\DosDevices\\", 12) && Link[13] == L':')
            {
                (*Letters)++;
                *Letter = Link[12];
            }
            else if (Chars >= 48 && !_wcsnicmp(Link, L"\\??\\Volume{", 11))
            {
                (*Guids)++;
                if (Chars == wcslen(Expected) && !_wcsnicmp(Link, Expected, Chars))
                    *GuidIsPartitionId = TRUE;
            }
        }
    }
    else
    {
        trace("    IOCTL_MOUNTMGR_QUERY_POINTS: error %lu\n", GetLastError());
    }
    if (Out)
        HeapFree(GetProcessHeap(), 0, Out);
    CloseHandle(MountMgr);
    return Ret;
}

static void TestPartition(ULONG DiskNumber, HANDLE Disk, PPARTITION_INFORMATION_EX Entry, PUCHAR Buffer)
{
    const PARTITION_INFORMATION_GPT *Gpt = &Entry->Gpt;
    BOOL IsBasicData = IsEqualGUID(&Gpt->PartitionType, &BasicDataGuid);
    BOOL Hidden = (Gpt->Attributes & GPT_ATTR_HIDDEN) != 0;
    BOOL NoLetter = (Gpt->Attributes & GPT_ATTR_NO_DRIVE_LETTER) != 0;
    ON_DISK_FS Fs;
    WCHAR Path[MAX_PATH], DeviceName[MAX_PATH], Letter;
    HANDLE Volume;
    DWORD Returned;
    BOOL Ret;
    PARTITION_INFORMATION PartInfo;
    PARTITION_INFORMATION_EX PartInfoEx;
    GET_LENGTH_INFORMATION LengthInfo;
    struct { MOUNTDEV_NAME Name; WCHAR Buffer[MAX_PATH]; } MountDevName;
    struct { FILE_FS_ATTRIBUTE_INFORMATION Info; WCHAR Buffer[MAX_PATH]; } FsAttr;
    FILE_FS_SIZE_INFORMATION FsSize;
    ULONGLONG VolumeBytes = 0;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    WCHAR Mounted[MAX_PATH] = L"";
    ULONG Letters, Guids;
    BOOL Listed, GuidIsPartitionId;

    Fs = ClassifyPartition(Disk, Entry->StartingOffset.QuadPart, Entry->PartitionLength.QuadPart, Buffer);
    trace("disk %lu partition %lu: offset 0x%I64x length 0x%I64x type %s attributes 0x%I64x name %s, on disk %s\n",
          DiskNumber, Entry->PartitionNumber, Entry->StartingOffset.QuadPart,
          Entry->PartitionLength.QuadPart, wine_dbgstr_guid(&Gpt->PartitionType),
          Gpt->Attributes, wine_dbgstr_w(Gpt->Name), wine_dbgstr_w(FsName(Fs)));

    _snwprintf(Path, _countof(Path), L"\\\\.\\GLOBALROOT\\Device\\Harddisk%lu\\Partition%lu", DiskNumber, Entry->PartitionNumber);

    /* The volume device name, through a handle that does not mount the volume */
    Volume = CreateFileW(Path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Volume != INVALID_HANDLE_VALUE, "partition %lu: open failed, error %lu\n", Entry->PartitionNumber, GetLastError());
    if (Volume == INVALID_HANDLE_VALUE)
        return;
    MountDevName.Name.NameLength = 0;
    Ret = DeviceIoControl(Volume, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, &MountDevName, sizeof(MountDevName), &Returned, NULL);
    ok(Ret, "partition %lu: IOCTL_MOUNTDEV_QUERY_DEVICE_NAME failed, error %lu\n", Entry->PartitionNumber, GetLastError());
    CloseHandle(Volume);
    if (!Ret)
        return;
    memcpy(DeviceName, MountDevName.Name.Name, MountDevName.Name.NameLength);
    DeviceName[MountDevName.Name.NameLength / sizeof(WCHAR)] = UNICODE_NULL;
    trace("    device %s\n", wine_dbgstr_w(DeviceName));

    /* Mount manager names: a basic data partition that is not hidden has one volume name, the
     * partition GUID, and a drive letter unless it has the no-drive-letter attribute. Hidden
     * partitions and Windows' system partitions get no drive letter. */
    Listed = QueryMountPoints(DeviceName, &Gpt->PartitionId, &Letters, &Guids, &Letter, &GuidIsPartitionId);
    trace("    mount manager: %s, %lu drive letter(s) %c, %lu volume name(s)%s\n",
          Listed ? "listed" : "not listed", Letters, Letter ? (char)Letter : '-', Guids,
          GuidIsPartitionId ? ", one is the partition GUID" : "");
    if ((IsBasicData && Hidden) || IsSystemPartitionType(&Gpt->PartitionType))
    {
        ok(Letters == 0, "partition %lu: hidden or system partition, but drive letter %c\n",
           Entry->PartitionNumber, (char)Letter);
    }
    else if (IsBasicData)
    {
        ok(Guids == 1, "partition %lu: %lu volume name(s)\n", Entry->PartitionNumber, Guids);
        ok(GuidIsPartitionId, "partition %lu: the volume name is not the partition GUID %s\n",
           Entry->PartitionNumber, wine_dbgstr_guid(&Gpt->PartitionId));
        if (NoLetter)
            ok(Letters == 0, "partition %lu: no-drive-letter attribute, but drive letter %c\n",
               Entry->PartitionNumber, (char)Letter);
        else
            ok(Letters == 1, "partition %lu: %lu drive letter(s)\n", Entry->PartitionNumber, Letters);
    }

    /* Opening the volume for reading mounts it */
    Volume = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Volume != INVALID_HANDLE_VALUE, "partition %lu: volume open failed, error %lu\n", Entry->PartitionNumber, GetLastError());
    if (Volume == INVALID_HANDLE_VALUE)
        return;

    SetLastError(0xdeadbeef);
    Ret = DeviceIoControl(Volume, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &LengthInfo, sizeof(LengthInfo), &Returned, NULL);
    ok(Ret, "partition %lu: IOCTL_DISK_GET_LENGTH_INFO failed, error %lu\n", Entry->PartitionNumber, GetLastError());
    if (Ret)
        ok(LengthInfo.Length.QuadPart == Entry->PartitionLength.QuadPart,
           "partition %lu: length 0x%I64x, expected 0x%I64x\n", Entry->PartitionNumber,
           LengthInfo.Length.QuadPart, Entry->PartitionLength.QuadPart);

    SetLastError(0xdeadbeef);
    Ret = DeviceIoControl(Volume, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &PartInfoEx, sizeof(PartInfoEx), &Returned, NULL);
    ok(Ret, "partition %lu: IOCTL_DISK_GET_PARTITION_INFO_EX failed, error %lu\n", Entry->PartitionNumber, GetLastError());
    if (Ret)
    {
        ok(PartInfoEx.PartitionStyle == PARTITION_STYLE_GPT, "partition %lu: style %d\n",
           Entry->PartitionNumber, PartInfoEx.PartitionStyle);
        ok(PartInfoEx.PartitionLength.QuadPart == Entry->PartitionLength.QuadPart,
           "partition %lu: EX length 0x%I64x\n", Entry->PartitionNumber, PartInfoEx.PartitionLength.QuadPart);
    }

    SetLastError(0xdeadbeef);
    ZeroMemory(&PartInfo, sizeof(PartInfo));
    Returned = 0;
    Ret = DeviceIoControl(Volume, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &PartInfo, sizeof(PartInfo), &Returned, NULL);
    /* The MBR-only request: callers must use the extended one or the length */
    ok(!Ret && GetLastError() == ERROR_INVALID_FUNCTION,
       "partition %lu: IOCTL_DISK_GET_PARTITION_INFO returned %d, error %lu\n",
       Entry->PartitionNumber, Ret, GetLastError());

    /* The file system the volume is mounted with, and its size */
    Status = NtQueryVolumeInformationFile(Volume, &Iosb, &FsAttr, sizeof(FsAttr), FileFsAttributeInformation);
    if (NT_SUCCESS(Status))
    {
        ULONG Chars = min(FsAttr.Info.FileSystemNameLength / sizeof(WCHAR), _countof(Mounted) - 1);

        memcpy(Mounted, FsAttr.Info.FileSystemName, Chars * sizeof(WCHAR));
        Mounted[Chars] = UNICODE_NULL;
    }
    else
    {
        trace("    FileFsAttributeInformation: 0x%08lx\n", Status);
    }
    Status = NtQueryVolumeInformationFile(Volume, &Iosb, &FsSize, sizeof(FsSize), FileFsSizeInformation);
    if (NT_SUCCESS(Status))
    {
        VolumeBytes = FsSize.TotalAllocationUnits.QuadPart * FsSize.SectorsPerAllocationUnit * FsSize.BytesPerSector;
        trace("    size: %I64u units of %lu sectors of %lu bytes, 0x%I64x bytes\n",
              FsSize.TotalAllocationUnits.QuadPart, FsSize.SectorsPerAllocationUnit,
              FsSize.BytesPerSector, VolumeBytes);
    }
    else
    {
        trace("    FileFsSizeInformation: 0x%08lx\n", Status);
    }
    CloseHandle(Volume);
    trace("    mounted as %s\n", Mounted[0] ? wine_dbgstr_w(Mounted) : "(not mounted)");
    if (IsBasicData && (Fs == FsNtfs || Fs == FsFat32 || Fs == FsFat))
    {
        ok(!wcscmp(Mounted, FsName(Fs)), "partition %lu: %s volume mounted as %s\n",
           Entry->PartitionNumber, wine_dbgstr_w(FsName(Fs)), Mounted[0] ? wine_dbgstr_w(Mounted) : "(nothing)");
    }
    if (!wcscmp(Mounted, L"RAW"))
    {
        /* The raw file system reports the partition, not the disk */
        ok(VolumeBytes == Entry->PartitionLength.QuadPart, "partition %lu: RAW volume size 0x%I64x, expected 0x%I64x\n",
           Entry->PartitionNumber, VolumeBytes, Entry->PartitionLength.QuadPart);
    }
}

START_TEST(GptVolumes)
{
    ULONG DiskNumber, GptDisks = 0, i;
    PDRIVE_LAYOUT_INFORMATION_EX Layout;
    const DWORD LayoutSize = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry[128]);
    PUCHAR Buffer;

    Layout = HeapAlloc(GetProcessHeap(), 0, LayoutSize);
    Buffer = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (!Layout || !Buffer)
    {
        skip("out of memory\n");
        return;
    }

    for (DiskNumber = 0; DiskNumber < 32; DiskNumber++)
    {
        WCHAR Path[64];
        HANDLE Disk;
        DWORD Returned;

        _snwprintf(Path, _countof(Path), L"\\\\.\\PhysicalDrive%lu", DiskNumber);
        Disk = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (Disk == INVALID_HANDLE_VALUE)
            continue;
        if (!DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, Layout, LayoutSize, &Returned, NULL) ||
            Layout->PartitionStyle != PARTITION_STYLE_GPT)
        {
            CloseHandle(Disk);
            continue;
        }
        GptDisks++;
        trace("disk %lu: GPT, %lu partition entries, disk id %s\n", DiskNumber, Layout->PartitionCount,
              wine_dbgstr_guid(&Layout->Gpt.DiskId));
        for (i = 0; i < Layout->PartitionCount; i++)
        {
            if (Layout->PartitionEntry[i].PartitionNumber == 0)
                continue;
            TestPartition(DiskNumber, Disk, &Layout->PartitionEntry[i], Buffer);
        }
        CloseHandle(Disk);
    }

    if (!GptDisks)
        skip("no GPT disk attached\n");

    VirtualFree(Buffer, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, Layout);
}
