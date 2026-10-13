/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Reads the disks, partitions and volumes of the running system into the model
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winnls.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <ntddscsi.h>
#include <ntddcdrm.h>
#include <ntddmmc.h>
#include <strsafe.h>

#include "diskmodel.h"

#define DM_MAX_PHYSICAL_DRIVES 128

static HANDLE
DmOpenDevice(PCWSTR Path, DWORD Access)
{
    return CreateFileW(Path, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL
DmIoctl(HANDLE Device, DWORD Code, PVOID In, DWORD InSize, PVOID Out, DWORD OutSize, PDWORD Returned)
{
    DWORD Dummy;

    return DeviceIoControl(Device, Code, In, InSize, Out, OutSize, Returned ? Returned : &Dummy, NULL);
}

static VOID
DmTrimCopy(PWSTR Dest, SIZE_T Count, const BYTE *Base, DWORD Size, ULONG Offset)
{
    CHAR Narrow[128];
    WCHAR Wide[128];
    SIZE_T Len, Start = 0, n = 0;

    Dest[0] = 0;
    if (Offset == 0 || Offset >= Size)
        return;
    while (Offset + n < Size && n < sizeof(Narrow) - 1 && Base[Offset + n] != 0)
    {
        Narrow[n] = (CHAR)Base[Offset + n];
        n++;
    }
    Narrow[n] = 0;
    if (MultiByteToWideChar(CP_ACP, 0, Narrow, -1, Wide, ARRAYSIZE(Wide)) == 0)
        return;
    Wide[ARRAYSIZE(Wide) - 1] = 0;
    Len = wcslen(Wide);
    while (Len > 0 && Wide[Len - 1] == L' ')
        Wide[--Len] = 0;
    while (Wide[Start] == L' ')
        Start++;
    StringCchCopyW(Dest, Count, Wide + Start);
}

/* Vendor, product and bus from the storage descriptor. */
static VOID
DmQueryStorage(HANDLE Device, PDM_DISK_INPUT Input, BOOL *Removable)
{
    STORAGE_PROPERTY_QUERY Query;
    union { STORAGE_DEVICE_DESCRIPTOR Desc; BYTE Raw[1024]; } Buffer;
    SCSI_ADDRESS Address;
    DWORD Size;

    ZeroMemory(&Query, sizeof(Query));
    Query.PropertyId = StorageDeviceProperty;
    Query.QueryType = PropertyStandardQuery;
    ZeroMemory(&Buffer, sizeof(Buffer));
    if (DmIoctl(Device, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query), &Buffer, sizeof(Buffer), &Size) &&
        Size >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        WCHAR Vendor[64], Product[64];

        Input->BusType = Buffer.Desc.BusType;
        if (Buffer.Desc.RemovableMedia)
            *Removable = TRUE;
        DmTrimCopy(Vendor, ARRAYSIZE(Vendor), Buffer.Raw, min(Size, (DWORD)sizeof(Buffer)), Buffer.Desc.VendorIdOffset);
        DmTrimCopy(Product, ARRAYSIZE(Product), Buffer.Raw, min(Size, (DWORD)sizeof(Buffer)), Buffer.Desc.ProductIdOffset);

        if (Input->BusType == BusTypeAta || Input->BusType == BusTypeAtapi || Input->BusType == BusTypeSata)
            StringCchPrintfW(Input->DeviceName, ARRAYSIZE(Input->DeviceName), L"%s ATA Device", Product[0] ? Product : Vendor);
        else if (Vendor[0] || Product[0])
            StringCchPrintfW(Input->DeviceName, ARRAYSIZE(Input->DeviceName), L"%s%s%s SCSI Disk Device",
                             Vendor, (Vendor[0] && Product[0]) ? L" " : L"", Product);
    }

    ZeroMemory(&Address, sizeof(Address));
    Address.Length = sizeof(Address);
    if (DmIoctl(Device, IOCTL_SCSI_GET_ADDRESS, NULL, 0, &Address, sizeof(Address), NULL))
    {
        Input->ScsiPath = Address.PathId;
        Input->ScsiTarget = Address.TargetId;
        Input->ScsiLun = Address.Lun;
    }
}

static PDRIVE_LAYOUT_INFORMATION_EX
DmReadLayout(HANDLE Device, PULONG Error)
{
    DWORD Size = 8192, Returned;

    *Error = 0;
    while (Size <= 1024 * 1024)
    {
        PDRIVE_LAYOUT_INFORMATION_EX Layout = HeapAlloc(GetProcessHeap(), 0, Size);

        if (Layout == NULL)
        {
            *Error = ERROR_NOT_ENOUGH_MEMORY;
            return NULL;
        }
        if (DmIoctl(Device, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, Layout, Size, &Returned))
        {
            if (Returned >= FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) &&
                Layout->PartitionCount <= (Returned - FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) /
                                           sizeof(PARTITION_INFORMATION_EX))
            {
                return Layout;
            }
            HeapFree(GetProcessHeap(), 0, Layout);
            *Error = ERROR_INVALID_DATA;
            return NULL;
        }
        *Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Layout);
        if (*Error != ERROR_INSUFFICIENT_BUFFER && *Error != ERROR_MORE_DATA)
            return NULL;
        Size *= 2;
    }
    *Error = ERROR_INSUFFICIENT_BUFFER;
    return NULL;
}

static VOID
DmCollectDisks(PDM_MODEL Model)
{
    ULONG Number;

    for (Number = 0; Number < DM_MAX_PHYSICAL_DRIVES; Number++)
    {
        WCHAR Path[64];
        HANDLE Device;
        DM_DISK_INPUT Input;
        union { DISK_GEOMETRY_EX Geo; BYTE Raw[1024]; } Geometry;
        PDRIVE_LAYOUT_INFORMATION_EX Layout = NULL;
        PDM_DISK Disk;
        BOOL Removable = FALSE;
        DWORD Error;

        StringCchPrintfW(Path, ARRAYSIZE(Path), L"\\\\.\\PhysicalDrive%u", Number);
        Device = DmOpenDevice(Path, 0);
        if (Device == INVALID_HANDLE_VALUE)
        {
            Error = GetLastError();
            if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND)
                continue;
            ZeroMemory(&Input, sizeof(Input));
            Input.Device = DM_DEV_DISK;
            Input.Number = Number;
            Input.HasMedia = TRUE;
            Input.LayoutError = Error;
            Input.BusType = 0xFF;
            Input.ScsiPath = Input.ScsiTarget = Input.ScsiLun = -1;
            if (DmBuildDisk(&Input, &Disk) && !DmAddDisk(Model, Disk))
                DmFreeDisk(Disk);
            continue;
        }
        ZeroMemory(&Input, sizeof(Input));
        Input.Number = Number;
        Input.HasMedia = TRUE;
        Input.BusType = 0xFF;
        Input.BytesPerSector = 512;
        Input.ScsiPath = Input.ScsiTarget = Input.ScsiLun = -1;
        DmQueryStorage(Device, &Input, &Removable);

        ZeroMemory(&Geometry, sizeof(Geometry));
        if (DmIoctl(Device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &Geometry, sizeof(Geometry), NULL))
        {
            Input.DiskLength = (ULONGLONG)Geometry.Geo.DiskSize.QuadPart;
            Input.BytesPerSector = Geometry.Geo.Geometry.BytesPerSector;
            if (Geometry.Geo.Geometry.MediaType == RemovableMedia)
                Removable = TRUE;
        }
        else
        {
            Error = GetLastError();
            if (Removable)
                Input.HasMedia = FALSE;
            else
                Input.LayoutError = Error ? Error : ERROR_GEN_FAILURE;
        }

        if (Input.HasMedia && Input.LayoutError == 0)
        {
            Layout = DmReadLayout(Device, &Error);
            if (Layout == NULL)
            {
                if (Removable && (Error == ERROR_NOT_READY || Error == ERROR_NO_MEDIA_IN_DRIVE))
                    Input.HasMedia = FALSE;
                Input.LayoutError = Error ? Error : ERROR_GEN_FAILURE;
            }
        }
        Input.Layout = Layout;
        Input.Removable = Removable;
        Input.Device = Removable ? DM_DEV_REMOVABLE : DM_DEV_DISK;

        if (DmBuildDisk(&Input, &Disk) && !DmAddDisk(Model, Disk))
            DmFreeDisk(Disk);
        if (Layout)
            HeapFree(GetProcessHeap(), 0, Layout);
        CloseHandle(Device);
    }
}

/* Optical drives: "DVD" when the disc in the drive is a DVD, otherwise "CD-ROM" (Windows names an empty drive "CD-ROM"). */
static VOID
DmOpticalKind(PCWSTR DrivePath, BOOL HasMedia, ULONGLONG MediaBytes, PWSTR Kind, SIZE_T Count)
{
    BOOL Dvd = FALSE, Known = FALSE;

    if (HasMedia)
    {
        HANDLE Device = DmOpenDevice(DrivePath, GENERIC_READ);

        if (Device != INVALID_HANDLE_VALUE)
        {
            GET_CONFIGURATION_IOCTL_INPUT Input;
            BYTE Buffer[512];
            DWORD Returned = 0;

            ZeroMemory(&Input, sizeof(Input));
            Input.Feature = FeatureProfileList;
            Input.RequestType = SCSI_GET_CONFIGURATION_REQUEST_TYPE_ALL;
            ZeroMemory(Buffer, sizeof(Buffer));
            if (DmIoctl(Device, IOCTL_CDROM_GET_CONFIGURATION, &Input, sizeof(Input), Buffer, sizeof(Buffer), &Returned) &&
                Returned >= sizeof(GET_CONFIGURATION_HEADER))
            {
                USHORT Current = ((USHORT)Buffer[6] << 8) | Buffer[7];

                if (Current != 0)
                {
                    Known = TRUE;
                    Dvd = (Current >= 0x10 && Current < 0x40);
                }
            }
            CloseHandle(Device);
        }
        if (!Known && MediaBytes > 1024 * DM_MIB)
            Dvd = TRUE;
    }
    StringCchCopyW(Kind, Count, Dvd ? L"DVD" : L"CD-ROM");
}

static BOOL
DmDeviceNumber(PCWSTR Path, PSTORAGE_DEVICE_NUMBER Number)
{
    HANDLE Device = DmOpenDevice(Path, 0);
    BOOL Ok;

    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = DmIoctl(Device, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, Number, sizeof(*Number), NULL);
    CloseHandle(Device);
    return Ok;
}

static VOID
DmFillVolumeInfo(PDM_VOLUME_INFO Info, PCWSTR RootPath, PCWSTR DevicePath)
{
    STORAGE_DEVICE_NUMBER Number;
    ULARGE_INTEGER FreeToCaller, Total, TotalFree;

    Info->DriveType = GetDriveTypeW(RootPath);
    if (DmDeviceNumber(DevicePath, &Number))
    {
        Info->HaveDeviceNumber = TRUE;
        Info->DeviceType = Number.DeviceType;
        Info->DeviceNumber = Number.DeviceNumber;
        Info->PartitionNumber = Number.PartitionNumber;
    }

    if (GetVolumeInformationW(RootPath, Info->Label, ARRAYSIZE(Info->Label), NULL, NULL, NULL,
                              Info->FileSystem, ARRAYSIZE(Info->FileSystem)))
        Info->HasFsInfo = TRUE;
    else
        Info->Label[0] = Info->FileSystem[0] = 0;

    if (GetDiskFreeSpaceExW(RootPath, &FreeToCaller, &Total, &TotalFree))
    {
        Info->HasSpace = TRUE;
        Info->Total = Total.QuadPart;
        /* the total honours quotas, the free space does not */
        Info->Free = min(TotalFree.QuadPart, Total.QuadPart);
    }
    Info->HasMedia = Info->HasFsInfo || Info->HasSpace;
}

static BOOL
DmLetterSeen(const PWSTR *Seen, ULONG Count, PCWSTR Letter)
{
    ULONG i;

    for (i = 0; i < Count; i++)
        if (_wcsicmp(Seen[i], Letter) == 0)
            return TRUE;
    return FALSE;
}

static VOID
DmCollectVolumes(PDM_MODEL Model)
{
    WCHAR Guid[MAX_PATH];
    HANDLE Find;
    WCHAR SeenStore[26][4];
    PWSTR Seen[26];
    ULONG SeenCount = 0;
    DWORD Drives, i;

    Find = FindFirstVolumeW(Guid, ARRAYSIZE(Guid));
    if (Find != INVALID_HANDLE_VALUE)
    {
        do
        {
            PDM_VOLUME_INFO Info = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DM_VOLUME_INFO));
            WCHAR Device[MAX_PATH];
            PWSTR Paths = NULL;
            DWORD Length = 0;
            SIZE_T Len;

            if (Info == NULL)
                break;
            StringCchCopyW(Info->Guid, ARRAYSIZE(Info->Guid), Guid);
            StringCchCopyW(Device, ARRAYSIZE(Device), Guid);
            Len = wcslen(Device);
            if (Len > 0 && Device[Len - 1] == L'\\')
                Device[Len - 1] = 0;

            GetVolumePathNamesForVolumeNameW(Guid, NULL, 0, &Length);
            if (Length > 1)
                Paths = HeapAlloc(GetProcessHeap(), 0, (Length + 1) * sizeof(WCHAR));
            if (Paths && GetVolumePathNamesForVolumeNameW(Guid, Paths, Length, &Length))
            {
                Paths[Length] = 0;
                PCWSTR Path;
                SIZE_T Used = 0;

                for (Path = Paths; *Path; Path += wcslen(Path) + 1)
                {
                    SIZE_T PathLen = wcslen(Path);

                    if (PathLen == 3 && Path[1] == L':' && Path[2] == L'\\' && Info->Letter[0] == 0)
                    {
                        Info->Letter[0] = (WCHAR)towupper(Path[0]);
                        Info->Letter[1] = L':';
                        Info->Letter[2] = 0;
                    }
                    if (Used + PathLen + 2 <= ARRAYSIZE(Info->MountPaths))
                    {
                        memcpy(Info->MountPaths + Used, Path, (PathLen + 1) * sizeof(WCHAR));
                        Used += PathLen + 1;
                        Info->MountPaths[Used] = 0;
                    }
                }
            }
            if (Paths)
                HeapFree(GetProcessHeap(), 0, Paths);
            DmFillVolumeInfo(Info, Guid, Device);
            if (Info->DriveType == DRIVE_CDROM)
            {
                WCHAR LetterPath[8];

                StringCchPrintfW(LetterPath, ARRAYSIZE(LetterPath), L"\\\\.\\%s", Info->Letter);
                DmOpticalKind(Info->Letter[0] ? LetterPath : Device, Info->HasMedia, Info->Total,
                              Info->MediaKind, ARRAYSIZE(Info->MediaKind));
            }

            if (DmAttachVolume(Model, Info) && Info->Letter[0] && SeenCount < ARRAYSIZE(Seen))
            {
                StringCchCopyW(SeenStore[SeenCount], 4, Info->Letter);
                Seen[SeenCount] = SeenStore[SeenCount];
                SeenCount++;
            }
            HeapFree(GetProcessHeap(), 0, Info);
        } while (FindNextVolumeW(Find, Guid, ARRAYSIZE(Guid)));
        FindVolumeClose(Find);
    }

    /* optical and removable drives the volume list did not give (no medium) */
    Drives = GetLogicalDrives();
    for (i = 0; i < 26; i++)
    {
        WCHAR Letter[4], Root[4], Device[8];
        PDM_VOLUME_INFO Info;
        UINT Type;

        if (!(Drives & (1UL << i)))
            continue;
        Letter[0] = (WCHAR)(L'A' + i);
        Letter[1] = L':';
        Letter[2] = 0;
        if (DmLetterSeen(Seen, SeenCount, Letter))
            continue;
        Root[0] = Letter[0]; Root[1] = L':'; Root[2] = L'\\'; Root[3] = 0;
        Type = GetDriveTypeW(Root);
        if (Type != DRIVE_CDROM && Type != DRIVE_REMOVABLE)
            continue;

        Info = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DM_VOLUME_INFO));
        if (Info == NULL)
            break;
        StringCchCopyW(Info->Letter, ARRAYSIZE(Info->Letter), Letter);
        StringCchPrintfW(Info->MountPaths, ARRAYSIZE(Info->MountPaths), L"%s", Root);
        StringCchPrintfW(Device, ARRAYSIZE(Device), L"\\\\.\\%s", Letter);
        DmFillVolumeInfo(Info, Root, Device);
        if (Type == DRIVE_CDROM)
            DmOpticalKind(Device, Info->HasMedia, Info->Total, Info->MediaKind, ARRAYSIZE(Info->MediaKind));
        DmAttachVolume(Model, Info);
        HeapFree(GetProcessHeap(), 0, Info);
    }
}

static VOID
DmReadSystemInfo(PDM_SYSINFO Info)
{
    WCHAR Windows[MAX_PATH], Device[8];
    STORAGE_DEVICE_NUMBER Number;
    HKEY Key;

    ZeroMemory(Info, sizeof(*Info));
    if (GetWindowsDirectoryW(Windows, ARRAYSIZE(Windows)) >= 2 && Windows[1] == L':')
    {
        StringCchPrintfW(Device, ARRAYSIZE(Device), L"\\\\.\\%c:", Windows[0]);
        if (DmDeviceNumber(Device, &Number))
        {
            Info->HaveBootDevice = TRUE;
            Info->BootDisk = Number.DeviceNumber;
            Info->BootPartition = Number.PartitionNumber;
            Info->BootIsCdrom = (Number.DeviceType == FILE_DEVICE_CD_ROM);
        }
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                      0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        WCHAR Buffer[1024];
        DWORD Size = sizeof(Buffer) - 2 * sizeof(WCHAR), Type;

        ZeroMemory(Buffer, sizeof(Buffer));
        if (RegQueryValueExW(Key, L"PagingFiles", NULL, &Type, (LPBYTE)Buffer, &Size) == ERROR_SUCCESS &&
            (Type == REG_MULTI_SZ || Type == REG_SZ))
        {
            PCWSTR Entry;

            for (Entry = Buffer; *Entry; Entry += wcslen(Entry) + 1)
            {
                PCWSTR Name = Entry;

                if (wcsncmp(Name, L"\\??\\", 4) == 0)
                    Name += 4;
                if (Name[0] == L'?' && Name[1] == L':')
                    Info->PagefileOnBoot = TRUE;
                else if (Name[1] == L':' && ((Name[0] >= L'A' && Name[0] <= L'Z') || (Name[0] >= L'a' && Name[0] <= L'z')))
                    Info->PagefileLetters |= 1UL << (towupper(Name[0]) - L'A');
            }
        }
        RegCloseKey(Key);
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CrashControl",
                      0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        DWORD Value = 0, Size = sizeof(Value), Type;

        if (RegQueryValueExW(Key, L"CrashDumpEnabled", NULL, &Type, (LPBYTE)&Value, &Size) == ERROR_SUCCESS &&
            Type == REG_DWORD && Value != 0)
        {
            Info->CrashDumpEnabled = TRUE;
        }
        RegCloseKey(Key);
    }
}

PDM_MODEL
DmCollect(VOID)
{
    PDM_MODEL Model = DmCreateModel();
    DM_SYSINFO Info;
    UINT OldMode;

    if (Model == NULL)
        return NULL;

    OldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    DmCollectDisks(Model);
    DmCollectVolumes(Model);
    DmReadSystemInfo(&Info);
    DmFinishModel(Model, &Info);
    SetErrorMode(OldMode);
    return Model;
}
