/*
 * PROJECT:         ReactOS DiskPart
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            base/system/diskpart/extend.c
 * PURPOSE:         Manages all the partitions of the OS in an interactive way.
 * PROGRAMMERS:     Lee Schroeder
 *                  Tomas Srnka (extend, 2026)
 */

#include "diskpart.h"

#define NDEBUG
#include <debug.h>

#ifndef FSCTL_EXTEND_VOLUME
#define FSCTL_EXTEND_VOLUME CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 60, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* WinBtrfs' own interface (drivers/filesystems/btrfs/btrfsioctl.h): its FSCTL_EXTEND_VOLUME is a stub. */
#define FSCTL_BTRFS_GET_DEVICES CTL_CODE(FILE_DEVICE_UNKNOWN, 0x82e, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define FSCTL_BTRFS_RESIZE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x848, METHOD_IN_DIRECT, FILE_ANY_ACCESS)

typedef struct _BTRFS_DEVICE
{
    UINT32 next_entry;
    UINT64 dev_id;
    UINT64 size;
    UINT64 max_size;
    BOOL readonly;
    BOOL missing;
    ULONG device_number;
    ULONG partition_number;
    UINT64 stats[5];
    USHORT namelen;
    WCHAR name[1];
} BTRFS_DEVICE, *PBTRFS_DEVICE;

typedef struct _BTRFS_RESIZE
{
    UINT64 device;
    UINT64 size;    /* 0: the size of the device */
} BTRFS_RESIZE;

/* A one-device Btrfs volume takes its whole (grown) device. */
static
NTSTATUS
ExtendBtrfs(
    _In_ HANDLE hVolume)
{
    BYTE Buffer[1024];
    PBTRFS_DEVICE Device = (PBTRFS_DEVICE)Buffer;
    BTRFS_RESIZE Resize;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    Status = NtFsControlFile(hVolume, NULL, NULL, NULL, &Iosb, FSCTL_BTRFS_GET_DEVICES, NULL, 0, Buffer, sizeof(Buffer));
    if (!NT_SUCCESS(Status))
        return Status;
    if (Device->next_entry != 0 || Device->readonly || Device->missing)
        return STATUS_NOT_SUPPORTED;

    Resize.device = Device->dev_id;
    Resize.size = 0;
    return NtFsControlFile(hVolume, NULL, NULL, NULL, &Iosb, FSCTL_BTRFS_RESIZE, &Resize, sizeof(Resize), NULL, 0);
}

NTSTATUS
OpenPartitionDevice(
    _In_ PPARTENTRY PartEntry,
    _In_ BOOL bWholeDisk,
    _Out_ PHANDLE phDevice)
{
    WCHAR szName[MAX_PATH];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK Iosb;

    StringCchPrintfW(szName, ARRAYSIZE(szName),
                     L"\\Device\\Harddisk%lu\\Partition%lu",
                     PartEntry->DiskEntry->DiskNumber,
                     bWholeDisk ? 0 : PartEntry->PartitionNumber);
    RtlInitUnicodeString(&Name, szName);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    return NtOpenFile(phDevice,
                      GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                      &ObjectAttributes,
                      &Iosb,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      FILE_SYNCHRONOUS_IO_NONALERT);
}

/* The file system on the partition ("NTFS", "FAT32", "RAW", ...), empty if it cannot be queried. */
VOID
GetPartitionFileSystem(
    _In_ HANDLE hVolume,
    _Out_writes_(cchFileSystem) PWSTR pszFileSystem,
    _In_ ULONG cchFileSystem)
{
    struct
    {
        FILE_FS_ATTRIBUTE_INFORMATION Info;
        WCHAR Buffer[32];
    } Attributes;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    pszFileSystem[0] = UNICODE_NULL;
    Status = NtQueryVolumeInformationFile(hVolume, &Iosb, &Attributes, sizeof(Attributes),
                                          FileFsAttributeInformation);
    if (NT_SUCCESS(Status))
    {
        StringCchCopyNW(pszFileSystem, cchFileSystem, Attributes.Info.FileSystemName,
                        Attributes.Info.FileSystemNameLength / sizeof(WCHAR));
    }
}

typedef enum _FAT_EXTEND_KIND
{
    FatExtendInPlace,       /* the FATs describe the new clusters: FSCTL_EXTEND_VOLUME */
    FatExtendOffline,       /* the FATs must grow: the FAT provider, on the locked volume */
    FatExtendTooBig,        /* more clusters than the FAT type allows */
    FatExtendUnknown        /* the boot sector cannot be read or makes no sense */
} FAT_EXTEND_KIND;

/*
 * How the FAT volume on hVolume can grow to ullNewSectors sectors.  The FAT
 * type follows from the cluster count, which may not cross a type boundary
 * (FAT12 below 4085 clusters, FAT16 up to 65524).  *pullMaxSectors gets the
 * largest size the type allows (an upper bound when the FATs grow).
 */
static
FAT_EXTEND_KIND
GetFatExtendKind(
    _In_ HANDLE hVolume,
    _In_ ULONGLONG ullNewSectors,
    _Out_ PULONGLONG pullMaxSectors)
{
    BYTE Boot[512];
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Offset;
    ULONG ulBytesPerSector, ulReserved, ulFats, ulRootSectors, ulFatSectors, ulSpc;
    ULONG ulBits, ulDescribable, ulOldClusters, ulOldSectors, ulFirstData, ulMaxClusters;
    ULONGLONG ullNewClusters;
    NTSTATUS Status;

    *pullMaxSectors = 0;
    Offset.QuadPart = 0;
    Status = NtReadFile(hVolume, NULL, NULL, NULL, &Iosb, Boot, sizeof(Boot), &Offset, NULL);
    if (!NT_SUCCESS(Status))
        return FatExtendUnknown;

    ulBytesPerSector = *(PUSHORT)&Boot[0x0B];
    ulSpc = Boot[0x0D];
    ulReserved = *(PUSHORT)&Boot[0x0E];
    ulFats = Boot[0x10];
    ulRootSectors = ulBytesPerSector ? (*(PUSHORT)&Boot[0x11] * 32 + ulBytesPerSector - 1) / ulBytesPerSector : 0;
    ulFatSectors = *(PUSHORT)&Boot[0x16] ? *(PUSHORT)&Boot[0x16] : *(PULONG)&Boot[0x24];
    ulOldSectors = *(PUSHORT)&Boot[0x13] ? *(PUSHORT)&Boot[0x13] : *(PULONG)&Boot[0x20];
    ulFirstData = ulReserved + ulFats * ulFatSectors + ulRootSectors;
    if (!ulBytesPerSector || !ulSpc || ulOldSectors <= ulFirstData)
        return FatExtendUnknown;

    ulOldClusters = (ulOldSectors - ulFirstData) / ulSpc;
    ulBits = (*(PUSHORT)&Boot[0x16] == 0) ? 32 : ((ulOldClusters < 4085) ? 12 : 16);
    ulMaxClusters = (ulBits == 12) ? 4084 : ((ulBits == 16) ? 65524 : 0x0FFFFFF5);
    *pullMaxSectors = min((ULONGLONG)ulFirstData + (ULONGLONG)ulMaxClusters * ulSpc, MAXULONG);

    if (ullNewSectors > MAXULONG || ullNewSectors <= ulFirstData)
        return FatExtendTooBig;
    ullNewClusters = (ullNewSectors - ulFirstData) / ulSpc;
    if (ullNewClusters > ulMaxClusters)
        return FatExtendTooBig;

    ulDescribable = (ULONG)(((ULONGLONG)ulFatSectors * ulBytesPerSector * 8) / ulBits) - 2;
    if (ullNewClusters > ulDescribable || (ulBits == 12 && ullNewClusters >= 4087))
        return FatExtendOffline;
    return FatExtendInPlace;
}

typedef NTSTATUS (NTAPI *PULIB_EXTEND)(PUNICODE_STRING DriveRoot, ULONGLONG NewSectors);

/*
 * Grows a FAT volume whose FATs are too small, offline: the FAT provider
 * (ufat) locks and dismounts the volume, grows the FATs and moves what is in
 * their way.  No handle of ours may be open on the volume.
 */
static
NTSTATUS
ExtendFatOffline(
    _In_ PPARTENTRY PartEntry,
    _In_ ULONGLONG ullNewSectors)
{
    WCHAR szName[MAX_PATH];
    UNICODE_STRING Name;
    HMODULE hModule;
    PULIB_EXTEND pExtend;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    if (!GetFsModule(L"FAT", &hModule))
        return STATUS_NOT_SUPPORTED;
    pExtend = (PULIB_EXTEND)GetProcAddress(hModule, "Extend");
    if (pExtend != NULL)
    {
        StringCchPrintfW(szName, ARRAYSIZE(szName), L"\\Device\\Harddisk%lu\\Partition%lu",
                         PartEntry->DiskEntry->DiskNumber, PartEntry->PartitionNumber);
        RtlInitUnicodeString(&Name, szName);
        Status = pExtend(&Name, ullNewSectors);
    }
    FreeLibrary(hModule);
    return Status;
}

/* Whether the volume can be locked now (nothing else open on it); hVolume stays unlocked. */
static
BOOL
CanLockVolume(
    _In_ HANDLE hVolume)
{
    IO_STATUS_BLOCK Iosb;

    if (!NT_SUCCESS(NtFsControlFile(hVolume, NULL, NULL, NULL, &Iosb, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0)))
        return FALSE;
    NtFsControlFile(hVolume, NULL, NULL, NULL, &Iosb, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
    return TRUE;
}

/* The partition with the focus: the selected volume's, else the selected partition. */
PPARTENTRY
GetFocusPartition(VOID)
{
    if (CurrentVolume != NULL)
        return GetPartitionForVolume(CurrentVolume);
    return CurrentPartition;
}

/*
 * Reads the disks and volumes again after a partition changed size, and gives
 * the focus back to the same disk, partition and (if it had it) volume.
 */
VOID
RefreshAfterResize(
    _In_ ULONG ulDiskNumber,
    _In_ ULONGLONG ullStartSector,
    _In_ BOOL bVolumeFocus)
{
    PLIST_ENTRY Entry, PartListEntry;
    PDISKENTRY DiskEntry;
    PPARTENTRY PartEntry, Found = NULL;

    CurrentDisk = NULL;
    CurrentPartition = NULL;
    CurrentVolume = NULL;

    DestroyPartitionList();
    CreatePartitionList();

    for (Entry = DiskListHead.Flink; Entry != &DiskListHead; Entry = Entry->Flink)
    {
        DiskEntry = CONTAINING_RECORD(Entry, DISKENTRY, ListEntry);
        if (DiskEntry->DiskNumber != ulDiskNumber)
            continue;

        CurrentDisk = DiskEntry;
        for (PartListEntry = DiskEntry->PrimaryPartListHead.Flink;
             PartListEntry != &DiskEntry->PrimaryPartListHead && !Found;
             PartListEntry = PartListEntry->Flink)
        {
            PartEntry = CONTAINING_RECORD(PartListEntry, PARTENTRY, ListEntry);
            if (PartEntry->IsPartitioned && PartEntry->StartSector.QuadPart == ullStartSector)
                Found = PartEntry;
        }
        for (PartListEntry = DiskEntry->LogicalPartListHead.Flink;
             PartListEntry != &DiskEntry->LogicalPartListHead && !Found;
             PartListEntry = PartListEntry->Flink)
        {
            PartEntry = CONTAINING_RECORD(PartListEntry, PARTENTRY, ListEntry);
            if (PartEntry->IsPartitioned && PartEntry->StartSector.QuadPart == ullStartSector)
                Found = PartEntry;
        }
        break;
    }

    CurrentPartition = Found;
    if (Found != NULL && bVolumeFocus)
        SelectVolumeOfNewPartition(Found);
    else
    {
        DestroyVolumeList();
        CreateVolumeList();
    }
}

/* The unpartitioned space right after the partition, in its MBR container or after the container. */
static
PPARTENTRY
GetFreeSpaceAfter(
    _In_ PPARTENTRY PartEntry)
{
    PDISKENTRY DiskEntry = PartEntry->DiskEntry;
    PPARTENTRY Next = GetNextUnpartitionedEntry(PartEntry);

    if (Next != NULL)
        return Next;

    /* The last logical partition grows past its container, which grows with it. */
    if (PartEntry->LogicalPartition && DiskEntry->ExtendedPartition != NULL &&
        PartEntry->ListEntry.Flink == &DiskEntry->LogicalPartListHead &&
        PartEntry->StartSector.QuadPart + PartEntry->SectorCount.QuadPart ==
        DiskEntry->ExtendedPartition->StartSector.QuadPart + DiskEntry->ExtendedPartition->SectorCount.QuadPart)
    {
        return GetNextUnpartitionedEntry(DiskEntry->ExtendedPartition);
    }

    return NULL;
}

static
NTSTATUS
GrowPartition(
    _In_ PPARTENTRY PartEntry,
    _In_ LONGLONG llBytes)
{
    DISK_GROW_PARTITION Grow;
    IO_STATUS_BLOCK Iosb;
    HANDLE hDisk;
    NTSTATUS Status;

    Status = OpenPartitionDevice(PartEntry, TRUE, &hDisk);
    if (!NT_SUCCESS(Status))
        return Status;

    Grow.PartitionNumber = PartEntry->PartitionNumber;
    Grow.BytesToGrow.QuadPart = llBytes;
    Status = NtDeviceIoControlFile(hDisk, NULL, NULL, NULL, &Iosb, IOCTL_DISK_GROW_PARTITION,
                                   &Grow, sizeof(Grow), NULL, 0);
    NtClose(hDisk);
    return Status;
}

/*
 * The file system on the partition takes the whole partition (NTFS keeps the
 * last sector for its boot sector copy).  The volume must be open on hVolume.
 */
static
NTSTATUS
ExtendFileSystem(
    _In_ HANDLE hVolume)
{
    GET_LENGTH_INFORMATION Length;
    DISK_GEOMETRY Geometry;
    IO_STATUS_BLOCK Iosb;
    LONGLONG llSectors;
    WCHAR szFileSystem[32];
    NTSTATUS Status;

    GetPartitionFileSystem(hVolume, szFileSystem, ARRAYSIZE(szFileSystem));
    if (_wcsicmp(szFileSystem, L"Btrfs") == 0)
        return ExtendBtrfs(hVolume);

    Status = NtDeviceIoControlFile(hVolume, NULL, NULL, NULL, &Iosb, IOCTL_DISK_GET_LENGTH_INFO,
                                   NULL, 0, &Length, sizeof(Length));
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtDeviceIoControlFile(hVolume, NULL, NULL, NULL, &Iosb, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL, 0, &Geometry, sizeof(Geometry));
    if (!NT_SUCCESS(Status) || Geometry.BytesPerSector == 0)
        Geometry.BytesPerSector = 512;

    llSectors = Length.Length.QuadPart / Geometry.BytesPerSector;
    return NtFsControlFile(hVolume, NULL, NULL, NULL, &Iosb, FSCTL_EXTEND_VOLUME,
                           &llSectors, sizeof(llSectors), NULL, 0);
}

static
BOOL
IsExtendableFileSystem(
    _In_ PCWSTR pszFileSystem)
{
    return (_wcsicmp(pszFileSystem, L"NTFS") == 0) ||
           (_wcsicmp(pszFileSystem, L"FAT") == 0) ||
           (_wcsicmp(pszFileSystem, L"FAT32") == 0) ||
           (_wcsicmp(pszFileSystem, L"Btrfs") == 0);
}

EXIT_CODE
extend_main(
    _In_ INT argc,
    _In_ PWSTR *argv)
{
    PPARTENTRY PartEntry, FreeEntry;
    PWSTR pszSuffix = NULL;
    WCHAR szFileSystem[32];
    ULONGLONG ullSize = 0, ullFree, ullBytes;
    ULONG ulDiskNumber;
    ULONGLONG ullStartSector;
    BOOL bFileSystemOnly = FALSE, bVolumeFocus, bHasFileSystem, bFat, bOffline;
    ULONGLONG ullNewSectors, ullMaxSectors;
    FAT_EXTEND_KIND Kind;
    HANDLE hVolume = NULL;
    NTSTATUS Status;
    INT i;

    for (i = 1; i < argc; i++)
    {
        if (HasPrefix(argv[i], L"size=", &pszSuffix))
        {
            /* size=<N> (MB) */
            ullSize = _wcstoui64(pszSuffix, NULL, 10);
            if (ullSize == 0)
            {
                ConResPuts(StdErr, IDS_ERROR_INVALID_ARGS);
                return EXIT_SUCCESS;
            }
        }
        else if (HasPrefix(argv[i], L"disk=", &pszSuffix))
        {
            /* disk=<N>: space on another disk makes a spanned volume (dynamic disks) */
            ConPuts(StdOut, L"The DISK option is not supported yet!\n");
            return EXIT_SUCCESS;
        }
        else if (_wcsicmp(argv[i], L"filesystem") == 0)
        {
            bFileSystemOnly = TRUE;
        }
        else if (_wcsicmp(argv[i], L"noerr") == 0)
        {
            /* noerr */
        }
        else
        {
            ConResPuts(StdErr, IDS_ERROR_INVALID_ARGS);
            return EXIT_SUCCESS;
        }
    }

    bVolumeFocus = (CurrentVolume != NULL);
    PartEntry = GetFocusPartition();
    if (PartEntry == NULL || !PartEntry->IsPartitioned)
    {
        ConResPuts(StdOut, bVolumeFocus ? IDS_EXTEND_NOT_PARTITION : IDS_SELECT_NO_VOLUME);
        return EXIT_SUCCESS;
    }
    ulDiskNumber = PartEntry->DiskEntry->DiskNumber;
    ullStartSector = PartEntry->StartSector.QuadPart;

    /* The file system decides first, so that nothing changes when it cannot follow. */
    szFileSystem[0] = UNICODE_NULL;
    Status = OpenPartitionDevice(PartEntry, FALSE, &hVolume);
    if (NT_SUCCESS(Status))
        GetPartitionFileSystem(hVolume, szFileSystem, ARRAYSIZE(szFileSystem));
    else
        hVolume = NULL;
    bHasFileSystem = (szFileSystem[0] != UNICODE_NULL) && (_wcsicmp(szFileSystem, L"RAW") != 0);

    if (bHasFileSystem && !IsExtendableFileSystem(szFileSystem))
    {
        ConResPuts(StdOut, IDS_EXTEND_FS_UNSUPPORTED);
        goto done;
    }

    /* WinBtrfs resizes only for a caller whose manage-volume privilege is enabled. */
    if (bHasFileSystem && _wcsicmp(szFileSystem, L"Btrfs") == 0)
    {
        BOOLEAN bOldState;

        Status = RtlAdjustPrivilege(SE_MANAGE_VOLUME_PRIVILEGE, TRUE, FALSE, &bOldState);
        if (Status != STATUS_SUCCESS)
        {
            ConResPrintf(StdOut, IDS_EXTEND_FAIL, Status == STATUS_NOT_ALL_ASSIGNED ? STATUS_PRIVILEGE_NOT_HELD : Status);
            goto done;
        }
    }

    if (bFileSystemOnly)
    {
        if (!bHasFileSystem || hVolume == NULL)
        {
            ConResPuts(StdOut, IDS_EXTEND_FS_UNSUPPORTED);
            goto done;
        }
        bFat = (_wcsnicmp(szFileSystem, L"FAT", 3) == 0);
        Kind = bFat ? GetFatExtendKind(hVolume, PartEntry->SectorCount.QuadPart, &ullMaxSectors) : FatExtendInPlace;
        if (Kind == FatExtendTooBig)
        {
            ConResPrintf(StdOut, IDS_EXTEND_FAT_TOO_BIG, szFileSystem,
                         ullMaxSectors * PartEntry->DiskEntry->BytesPerSector / SIZE_1MB);
            goto done;
        }
        if (Kind == FatExtendOffline)
        {
            if (!CanLockVolume(hVolume))
            {
                ConResPuts(StdOut, IDS_EXTEND_IN_USE);
                goto done;
            }
            NtClose(hVolume);
            hVolume = NULL;
            Status = ExtendFatOffline(PartEntry, PartEntry->SectorCount.QuadPart);
        }
        else
        {
            Status = ExtendFileSystem(hVolume);
        }
        if (NT_SUCCESS(Status))
            ConResPuts(StdOut, IDS_EXTEND_FS_SUCCESS);
        else
            ConResPrintf(StdOut, IDS_EXTEND_FAIL, Status);
        goto done;
    }

    FreeEntry = GetFreeSpaceAfter(PartEntry);
    ullFree = FreeEntry ? FreeEntry->SectorCount.QuadPart * PartEntry->DiskEntry->BytesPerSector : 0;
    ullBytes = ullSize ? ullSize * SIZE_1MB : ullFree;
    if (ullFree == 0 || ullBytes > ullFree)
    {
        ConResPuts(StdOut, IDS_EXTEND_NO_SPACE);
        goto done;
    }

    /*
     * A FAT volume extends in place while its FATs can describe the new
     * clusters; otherwise its FATs grow offline, which needs the volume to
     * itself: checked before anything changes.
     */
    ullNewSectors = (PartEntry->SectorCount.QuadPart * PartEntry->DiskEntry->BytesPerSector + ullBytes) /
                    PartEntry->DiskEntry->BytesPerSector;
    Kind = FatExtendInPlace;
    if (bHasFileSystem && _wcsnicmp(szFileSystem, L"FAT", 3) == 0)
        Kind = GetFatExtendKind(hVolume, ullNewSectors, &ullMaxSectors);
    if (Kind == FatExtendTooBig)
    {
        ConResPrintf(StdOut, IDS_EXTEND_FAT_TOO_BIG, szFileSystem,
                     ullMaxSectors * PartEntry->DiskEntry->BytesPerSector / SIZE_1MB);
        goto done;
    }
    bOffline = (Kind == FatExtendOffline);
    if (bOffline && !CanLockVolume(hVolume))
    {
        ConResPuts(StdOut, IDS_EXTEND_IN_USE);
        goto done;
    }

    Status = GrowPartition(PartEntry, (LONGLONG)ullBytes);
    if (!NT_SUCCESS(Status))
    {
        ConResPrintf(StdOut, IDS_EXTEND_FAIL, Status);
        goto done;
    }

    if (bOffline)
    {
        NtClose(hVolume);
        hVolume = NULL;
        Status = ExtendFatOffline(PartEntry, ullNewSectors);
        if (!NT_SUCCESS(Status))
        {
            ConResPrintf(StdOut, IDS_EXTEND_FS_FAIL, Status);
            goto refresh;
        }
    }
    else if (bHasFileSystem)
    {
        Status = ExtendFileSystem(hVolume);
        if (!NT_SUCCESS(Status))
        {
            /* The partition grew; "extend filesystem" can finish the job later. */
            ConResPrintf(StdOut, IDS_EXTEND_FS_FAIL, Status);
            goto refresh;
        }
    }

    ConResPuts(StdOut, IDS_EXTEND_SUCCESS);

refresh:
    if (hVolume != NULL)
    {
        NtClose(hVolume);
        hVolume = NULL;
    }
    RefreshAfterResize(ulDiskNumber, ullStartSector, bVolumeFocus);

done:
    if (hVolume != NULL)
        NtClose(hVolume);

    return EXIT_SUCCESS;
}
