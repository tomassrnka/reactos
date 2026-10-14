/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The writer: changes disks, partitions and volumes (no user interface)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Only documented interfaces: the drive layout control codes, the volume control codes, the mount point
 * functions of kernel32 and the format functions of fmifs, so that the program is the same on Windows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winioctl.h>
#include <winreg.h>
#include <ntddstor.h>
#include <rpc.h>
#include <strsafe.h>

#include "diskops.h"

#define DM_VOLUME_POLL_MS   250
#define DM_MAX_MOUNT_PATHS  64

typedef BOOLEAN (NTAPI *DM_FMIFS_CALLBACK)(ULONG Command, ULONG SubAction, PVOID ActionInfo);
typedef BOOLEAN (NTAPI *DM_QUERY_FS_FN)(DWORD Index, PWCHAR FileSystem, UCHAR *Major, UCHAR *Minor, BOOLEAN *Latest);
typedef VOID (NTAPI *DM_FORMATEX_FN)(PWCHAR DriveRoot, ULONG MediaFlag, PWCHAR Format, PWCHAR Label, BOOLEAN Quick,
                                      ULONG ClusterSize, DM_FMIFS_CALLBACK Callback);

/* the callback of fmifs carries no context, and one format runs at a time */
static PDM_FORMAT g_Format;
static BOOLEAN g_FormatDone;
static BOOLEAN g_FormatOk;
static CRITICAL_SECTION g_FormatLock;
static LONG g_FormatLockInit;

#define FMIFS_PROGRESS      0
#define FMIFS_DONE          11
#define FMIFS_OUTPUT        14
#define FMIFS_MEDIA_REMOVABLE 11
#define FMIFS_MEDIA_HARDDISK  12

static DWORD
DmFromLayout(DL_STATUS Status)
{
    return Status == DL_OK ? ERROR_SUCCESS : (DM_ERR_LAYOUT_BASE + (DWORD)Status);
}

static BOOL
DmIoctl(HANDLE Device, DWORD Code, PVOID In, DWORD InSize, PVOID Out, DWORD OutSize, PDWORD Returned)
{
    DWORD Dummy;

    return DeviceIoControl(Device, Code, In, InSize, Out, OutSize, Returned ? Returned : &Dummy, NULL);
}

/* ------------------------------------------------------------------ file systems */

BOOL
DmQueryFileSystems(PDM_FS_LIST List)
{
    HMODULE Module;
    DM_QUERY_FS_FN Query;
    DWORD Index;

    ZeroMemory(List, sizeof(*List));
    Module = LoadLibraryW(L"fmifs.dll");
    if (Module == NULL)
        return FALSE;
    Query = (DM_QUERY_FS_FN)GetProcAddress(Module, "QueryAvailableFileSystemFormat");
    if (Query)
    {
        for (Index = 0; Index < 64 && List->Count < DM_MAX_FS; Index++)
        {
            WCHAR Name[64];
            UCHAR Major, Minor;
            BOOLEAN Latest;
            ULONG i;
            BOOL Seen = FALSE;

            ZeroMemory(Name, sizeof(Name));
            if (!Query(Index, Name, &Major, &Minor, &Latest))
                break;
            Name[ARRAYSIZE(Name) - 1] = 0;
            if (Name[0] == 0 || wcslen(Name) >= DM_FS_NAME)
                continue;
            for (i = 0; i < List->Count; i++)
                if (_wcsicmp(List->Name[i], Name) == 0)
                    Seen = TRUE;
            if (!Seen)
                StringCchCopyW(List->Name[List->Count++], DM_FS_NAME, Name);
        }
    }
    FreeLibrary(Module);
    return List->Count != 0;
}

/* ------------------------------------------------------------------------ disks */

static HANDLE
DmOpenDiskHandle(ULONG Disk, DWORD Access)
{
    WCHAR Path[64];

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"\\\\.\\PhysicalDrive%u", Disk);
    return CreateFileW(Path, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static VOID
DmNewGuid(GUID *Guid)
{
    UuidCreate(Guid);
}

DWORD
DmReadDiskLayout(ULONG Disk, PDL_LAYOUT Layout)
{
    HANDLE Device = DmOpenDiskHandle(Disk, GENERIC_READ);
    union { DISK_GEOMETRY_EX Geo; BYTE Raw[1024]; } Geometry;
    DWORD Size = 8192, Returned, Error = ERROR_SUCCESS;

    if (Device == INVALID_HANDLE_VALUE)
        return GetLastError();
    ZeroMemory(&Geometry, sizeof(Geometry));
    if (!DmIoctl(Device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &Geometry, sizeof(Geometry), NULL))
    {
        Error = GetLastError();
        CloseHandle(Device);
        return Error ? Error : ERROR_GEN_FAILURE;
    }
    while (Size <= 1024 * 1024)
    {
        PDRIVE_LAYOUT_INFORMATION_EX Driver = HeapAlloc(GetProcessHeap(), 0, Size);

        if (Driver == NULL)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        if (DmIoctl(Device, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, Driver, Size, &Returned))
        {
            if (Returned >= FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) &&
                Driver->PartitionStyle == PARTITION_STYLE_RAW)
            {
                /* a disk without a table, as the driver words it */
                Error = ERROR_UNRECOGNIZED_MEDIA;
            }
            else if (Returned >= FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) &&
                Driver->PartitionCount <= (Returned - FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) /
                                           sizeof(PARTITION_INFORMATION_EX) &&
                DlFromDriver(Driver, (ULONGLONG)Geometry.Geo.DiskSize.QuadPart, Geometry.Geo.Geometry.BytesPerSector, Layout))
            {
                Error = ERROR_SUCCESS;
            }
            else
            {
                /* a table this program cannot represent is not an empty disk */
                Error = DM_ERR_UNREADABLE;
            }
            HeapFree(GetProcessHeap(), 0, Driver);
            break;
        }
        Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Driver);
        if (Error != ERROR_INSUFFICIENT_BUFFER && Error != ERROR_MORE_DATA)
            break;
        Size *= 2;
    }
    CloseHandle(Device);
    return Error;
}

static DWORD
DmWriteLayoutOnce(ULONG Disk, const DL_LAYOUT *Layout)
{
    HANDLE Device;
    PDRIVE_LAYOUT_INFORMATION_EX Driver, Result;
    ULONG Size = 0;
    DWORD Error = ERROR_SUCCESS;

    if (!DlIsEncodable(Layout))
        return DM_ERR_LAYOUT_BASE + DL_E_UNREPRESENTABLE;
    Driver = DlToDriver(Layout, &Size);
    if (Driver == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Result = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (Result == NULL)
    {
        HeapFree(GetProcessHeap(), 0, Driver);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    Device = DmOpenDiskHandle(Disk, GENERIC_READ | GENERIC_WRITE);
    if (Device == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
    }
    else
    {
        if (!DmIoctl(Device, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, Driver, Size, Result, Size, NULL))
            Error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
        else
            DmIoctl(Device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, NULL);
        CloseHandle(Device);
    }
    HeapFree(GetProcessHeap(), 0, Result);
    HeapFree(GetProcessHeap(), 0, Driver);
    return Error;
}

/* TRUE when every partition of Wanted is on the disk with the same start, length and kind, and the disk has no other */
static BOOL
DmLayoutsAgree(const DL_LAYOUT *Wanted, const DL_LAYOUT *Found)
{
    ULONG i, j;

    if (Wanted->Count != Found->Count || Wanted->Style != Found->Style)
        return FALSE;
    for (i = 0; i < Wanted->Count; i++)
    {
        const DL_ENTRY *W = &Wanted->Entry[i];

        for (j = 0; j < Found->Count; j++)
        {
            const DL_ENTRY *F = &Found->Entry[j];

            if (F->Offset != W->Offset || F->Length != W->Length)
                continue;
            if (Wanted->Style == DM_STYLE_MBR)
            {
                if (F->MbrType == W->MbrType && (F->Active ? TRUE : FALSE) == (W->Active ? TRUE : FALSE) &&
                    F->Logical == W->Logical && F->Container == W->Container)
                    break;
            }
            else if (memcmp(&F->GptType, &W->GptType, sizeof(GUID)) == 0 && memcmp(&F->GptId, &W->GptId, sizeof(GUID)) == 0)
            {
                break;
            }
        }
        if (j == Found->Count)
            return FALSE;
    }
    return TRUE;
}

/* Writes the table. The tables of the disk are written one sector after the other, so a failure half way can leave the
 * chain of the logical drives cut: the table that was there is kept, written back after a failure, and the result is read
 * again to see that the disk has what was asked for. */
DWORD
DmWriteDiskLayout(ULONG Disk, const DL_LAYOUT *Layout)
{
    PDL_LAYOUT Before = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    PDL_LAYOUT After = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    DWORD Error;

    if (Before == NULL || After == NULL)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Done;
    }
    /* without the table that is there now nothing could be put back, so nothing is written */
    Error = DmReadDiskLayout(Disk, Before);
    if (Error != ERROR_SUCCESS)
        goto Done;
    Error = DmWriteLayoutOnce(Disk, Layout);
    if (Error == ERROR_SUCCESS)
    {
        ULONG Try;
        BOOL Agree = FALSE;

        /* a driver may report success for a write that failed, so the disk is read again; it may take a moment to show the table */
        for (Try = 0; Try < 5 && !Agree; Try++)
        {
            if (Try)
                Sleep(200);
            Agree = (DmReadDiskLayout(Disk, After) == ERROR_SUCCESS && DmLayoutsAgree(Layout, After));
        }
        if (!Agree)
            Error = DM_ERR_VERIFY_FAILED;
    }
    if (Error != ERROR_SUCCESS && Error != DM_ERR_LAYOUT_BASE + DL_E_UNREPRESENTABLE)
    {
        /* the table may be half written: put the old table back, and say so when that fails too */
        if (DmWriteLayoutOnce(Disk, Before) != ERROR_SUCCESS)
            Error = DM_ERR_RESTORE_FAILED;
    }
Done:
    if (Before)
        HeapFree(GetProcessHeap(), 0, Before);
    if (After)
        HeapFree(GetProcessHeap(), 0, After);
    return Error;
}

/* ERROR_SUCCESS when the disk is empty: raw media, or a table that lists no partition (the reserved partition of a GPT
 * disk does not count). A table that cannot be read is no proof of an empty disk, so every other error is returned. */
static DWORD
DmCheckDiskEmpty(ULONG Disk)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    DWORD Error;
    ULONG i;

    if (Layout == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = DmReadDiskLayout(Disk, Layout);
    if (Error == ERROR_SUCCESS)
    {
        for (i = 0; i < Layout->Count; i++)
        {
            if (Layout->Style == DM_STYLE_MBR || memcmp(&Layout->Entry[i].GptType, &DlGuidReserved, sizeof(GUID)) != 0)
                Error = DM_ERR_NOT_EMPTY;
        }
    }
    else if (Error == ERROR_UNRECOGNIZED_MEDIA)
    {
        Error = ERROR_SUCCESS;
    }
    HeapFree(GetProcessHeap(), 0, Layout);
    return Error;
}

DWORD
DmCheckScratchDisk(ULONG Disk)
{
    return DmCheckDiskEmpty(Disk);
}

/* Writes a new, empty table. It never replaces a table that lists partitions. */
DWORD
DmInitializeDisk(ULONG Disk, DM_STYLE Style)
{
    HANDLE Device;
    CREATE_DISK Create;
    GUID Guid;
    DWORD Error = ERROR_SUCCESS;

    if (Style != DM_STYLE_MBR && Style != DM_STYLE_GPT)
        return ERROR_INVALID_PARAMETER;
    Error = DmCheckDiskEmpty(Disk);
    if (Error != ERROR_SUCCESS)
        return Error;
    ZeroMemory(&Create, sizeof(Create));
    DmNewGuid(&Guid);
    if (Style == DM_STYLE_MBR)
    {
        Create.PartitionStyle = PARTITION_STYLE_MBR;
        Create.Mbr.Signature = Guid.Data1 ? Guid.Data1 : 1;
    }
    else
    {
        Create.PartitionStyle = PARTITION_STYLE_GPT;
        Create.Gpt.DiskId = Guid;
        Create.Gpt.MaxPartitionCount = 128;
    }
    Device = DmOpenDiskHandle(Disk, GENERIC_READ | GENERIC_WRITE);
    if (Device == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!DmIoctl(Device, IOCTL_DISK_CREATE_DISK, &Create, sizeof(Create), NULL, 0, NULL))
        Error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    else
        DmIoctl(Device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, NULL);
    CloseHandle(Device);
    return Error;
}

DWORD
DmConvertDisk(ULONG Disk, DM_STYLE Style)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    DWORD Error;

    if (Layout == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = DmReadDiskLayout(Disk, Layout);
    if (Error == ERROR_SUCCESS && Layout->Style == Style)
        Error = ERROR_INVALID_PARAMETER;
    HeapFree(GetProcessHeap(), 0, Layout);
    if (Error != ERROR_SUCCESS)
        return Error;
    /* the table must list no partition but a reserved one */
    return DmInitializeDisk(Disk, Style);
}

typedef DWORD (WINAPI *DM_CM_LOCATE_FN)(PDWORD Dev, PWSTR Id, ULONG Flags);
typedef DWORD (WINAPI *DM_CM_REENUM_FN)(DWORD Dev, ULONG Flags);

DWORD
DmRescanDisks(VOID)
{
    ULONG Disk;
    HMODULE Module;

    /* the bus drivers look for new devices, then every disk reads its table again */
    Module = LoadLibraryW(L"cfgmgr32.dll");
    if (Module == NULL)
        Module = LoadLibraryW(L"setupapi.dll");
    if (Module)
    {
        DM_CM_LOCATE_FN Locate = (DM_CM_LOCATE_FN)GetProcAddress(Module, "CM_Locate_DevNodeW");
        DM_CM_REENUM_FN Reenum = (DM_CM_REENUM_FN)GetProcAddress(Module, "CM_Reenumerate_DevNode");
        DWORD Root;

        if (Locate && Reenum && Locate(&Root, NULL, 0) == 0)
            Reenum(Root, 0x00000001 /* CM_REENUMERATE_SYNCHRONOUS */);
        FreeLibrary(Module);
    }
    for (Disk = 0; Disk < 128; Disk++)
    {
        HANDLE Device = DmOpenDiskHandle(Disk, GENERIC_READ | GENERIC_WRITE);

        if (Device == INVALID_HANDLE_VALUE)
            continue;
        DmIoctl(Device, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, NULL);
        CloseHandle(Device);
    }
    return ERROR_SUCCESS;
}

/* ----------------------------------------------------------------------- volumes */

static BOOL
DmTrimGuid(PCWSTR Guid, PWSTR Device, SIZE_T Count)
{
    SIZE_T Len;

    if (FAILED(StringCchCopyW(Device, Count, Guid)))
        return FALSE;
    Len = wcslen(Device);
    if (Len > 0 && Device[Len - 1] == L'\\')
        Device[Len - 1] = 0;
    return TRUE;
}

static BOOL
DmVolumeOnDisk(PCWSTR Guid, ULONG Disk, ULONGLONG Offset)
{
    WCHAR Device[MAX_PATH];
    HANDLE Handle;
    union { VOLUME_DISK_EXTENTS Extents; BYTE Raw[512]; } Buffer;
    DWORD Returned;
    BOOL Found = FALSE;
    ULONG i;

    if (!DmTrimGuid(Guid, Device, ARRAYSIZE(Device)))
        return FALSE;
    Handle = CreateFileW(Device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    ZeroMemory(&Buffer, sizeof(Buffer));
    if (DmIoctl(Handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &Buffer, sizeof(Buffer), &Returned))
    {
        for (i = 0; i < Buffer.Extents.NumberOfDiskExtents && i < 4; i++)
        {
            if (Buffer.Extents.Extents[i].DiskNumber == Disk &&
                (ULONGLONG)Buffer.Extents.Extents[i].StartingOffset.QuadPart == Offset)
                Found = TRUE;
        }
    }
    CloseHandle(Handle);
    return Found;
}

DWORD
DmFindVolume(ULONG Disk, ULONGLONG Offset, ULONG TimeoutMs, PWSTR VolumeGuid, SIZE_T Count)
{
    DWORD Start = GetTickCount();

    if (Count > 0)
        VolumeGuid[0] = 0;
    for (;;)
    {
        WCHAR Guid[MAX_PATH];
        HANDLE Find = FindFirstVolumeW(Guid, ARRAYSIZE(Guid));

        if (Find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (DmVolumeOnDisk(Guid, Disk, Offset))
                {
                    FindVolumeClose(Find);
                    return SUCCEEDED(StringCchCopyW(VolumeGuid, Count, Guid)) ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
                }
            } while (FindNextVolumeW(Find, Guid, ARRAYSIZE(Guid)));
            FindVolumeClose(Find);
        }
        if (GetTickCount() - Start >= TimeoutMs)
            return DM_ERR_NO_VOLUME;
        Sleep(DM_VOLUME_POLL_MS);
    }
}

/* The volume of the Windows directory must never be formatted or removed. */
static BOOL
DmIsSystemVolume(PCWSTR VolumeGuid)
{
    WCHAR Windows[MAX_PATH], Root[4], Guid[MAX_PATH];

    if (GetWindowsDirectoryW(Windows, ARRAYSIZE(Windows)) < 2 || Windows[1] != L':')
        return FALSE;
    Root[0] = Windows[0]; Root[1] = L':'; Root[2] = L'\\'; Root[3] = 0;
    if (!GetVolumeNameForVolumeMountPointW(Root, Guid, ARRAYSIZE(Guid)))
        return FALSE;
    return _wcsicmp(Guid, VolumeGuid) == 0;
}

/* Locks and dismounts a volume, then lets go of it: the handle is closed before the table is written. */
static DWORD
DmLockDismount(PCWSTR VolumeGuid, BOOL Force)
{
    WCHAR Device[MAX_PATH];
    HANDLE Handle;
    DWORD Error = ERROR_SUCCESS;
    BOOL Locked;

    if (!DmTrimGuid(VolumeGuid, Device, ARRAYSIZE(Device)))
        return ERROR_BAD_PATHNAME;
    Handle = CreateFileW(Device, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    Locked = DmIoctl(Handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    if (!Locked && !Force)
    {
        CloseHandle(Handle);
        return DM_ERR_VOLUME_IN_USE;
    }
    if (!DmIoctl(Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL))
        Error = GetLastError();
    if (Locked)
        DmIoctl(Handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    CloseHandle(Handle);
    return Error;
}

static PCWSTR
DmNextPath(PCWSTR Paths)
{
    return (Paths && *Paths) ? Paths + wcslen(Paths) + 1 : NULL;
}

DWORD
DmAddMountPath(PCWSTR VolumeGuid, PCWSTR Path)
{
    return SetVolumeMountPointW(Path, VolumeGuid) ? ERROR_SUCCESS : (GetLastError() ? GetLastError() : DM_ERR_NOT_SUPPORTED);
}

/* The volume a path leads to now, or an empty string when the system cannot say. */
static VOID
DmPathTarget(PCWSTR Path, PWSTR Guid, SIZE_T Count)
{
    Guid[0] = 0;
    if (!GetVolumeNameForVolumeMountPointW(Path, Guid, (DWORD)Count))
        Guid[0] = 0;
}

/* Takes a path away from a volume, but only while the path still leads to that volume: another program may have given the
 * letter to another volume since the user looked. */
DWORD
DmRemoveMountPathOf(PCWSTR VolumeGuid, PCWSTR Path)
{
    WCHAR Target[MAX_PATH];

    DmPathTarget(Path, Target, ARRAYSIZE(Target));
    if (Target[0] && _wcsicmp(Target, VolumeGuid) != 0)
        return DM_ERR_CHANGED;
    return DmRemoveMountPath(Path);
}

DWORD
DmRemoveMountPath(PCWSTR Path)
{
    return DeleteVolumeMountPointW(Path) ? ERROR_SUCCESS : (GetLastError() ? GetLastError() : DM_ERR_NOT_SUPPORTED);
}

static BOOL
DmIsLetterPath(PCWSTR Path)
{
    return wcslen(Path) == 3 && Path[1] == L':' && Path[2] == L'\\';
}

DWORD
DmChangeDriveLetter(PCWSTR VolumeGuid, PCWSTR MountPaths, WCHAR NewLetter)
{
    PCWSTR Path;
    WCHAR Old[4][4];
    ULONG OldCount = 0, Removed, i;
    WCHAR Target[4];
    DWORD Error = ERROR_SUCCESS;

    if (NewLetter)
    {
        Target[0] = NewLetter; Target[1] = L':'; Target[2] = L'\\'; Target[3] = 0;
    }
    for (Path = MountPaths; Path && *Path; Path = DmNextPath(Path))
    {
        if (DmIsLetterPath(Path))
        {
            if (NewLetter && _wcsnicmp(Path, Target, 3) == 0)
                return ERROR_SUCCESS;       /* the volume has the letter already */
            if (OldCount < ARRAYSIZE(Old))
                StringCchCopyW(Old[OldCount++], 4, Path);
        }
    }
    for (Removed = 0; Removed < OldCount; Removed++)
    {
        Error = DmRemoveMountPathOf(VolumeGuid, Old[Removed]);
        if (Error != ERROR_SUCCESS)
            goto Restore;
    }
    if (NewLetter)
    {
        Error = DmAddMountPath(VolumeGuid, Target);
        if (Error != ERROR_SUCCESS)
            goto Restore;
    }
    return ERROR_SUCCESS;
Restore:
    /* put back the letters that were taken away */
    for (i = 0; i < Removed; i++)
        DmAddMountPath(VolumeGuid, Old[i]);
    return Error;
}

/* Replaces one path of a volume by another, and puts the old one back when the new one cannot be made. */
DWORD
DmReplaceMountPath(PCWSTR VolumeGuid, PCWSTR OldPath, PCWSTR NewPath)
{
    DWORD Error;

    if (_wcsicmp(OldPath, NewPath) == 0)
        return ERROR_SUCCESS;
    Error = DmRemoveMountPathOf(VolumeGuid, OldPath);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = DmAddMountPath(VolumeGuid, NewPath);
    if (Error != ERROR_SUCCESS)
        DmAddMountPath(VolumeGuid, OldPath);
    return Error;
}

DWORD
DmSetVolumeLabel(PCWSTR VolumeGuid, PCWSTR Label)
{
    return SetVolumeLabelW(VolumeGuid, (Label && Label[0]) ? Label : NULL) ? ERROR_SUCCESS : GetLastError();
}

/* ------------------------------------------------------------------------ format */

static BOOLEAN NTAPI
DmFormatCallback(ULONG Command, ULONG SubAction, PVOID ActionInfo)
{
    PDM_FORMAT Format = g_Format;

    UNREFERENCED_PARAMETER(SubAction);
    if (Format == NULL)
        return TRUE;
    switch (Command)
    {
        case FMIFS_PROGRESS:
            if (Format->Progress && ActionInfo)
                Format->Progress(Format->Context, *(PDWORD)ActionInfo);
            break;
        case FMIFS_DONE:
            g_FormatDone = TRUE;
            g_FormatOk = ActionInfo ? *(PBOOLEAN)ActionInfo : FALSE;
            break;
        case FMIFS_OUTPUT:
            if (ActionInfo && Format->Output[0] == 0)
            {
                typedef struct { ULONG Lines; PCHAR Output; } DM_TEXTOUTPUT;
                DM_TEXTOUTPUT *Text = ActionInfo;

                if (Text->Output)
                {
                    CHAR Line[128];
                    SIZE_T n = 0;

                    while (Text->Output[n] && Text->Output[n] != '\n' && Text->Output[n] != '\r' && n < sizeof(Line) - 1)
                    {
                        Line[n] = Text->Output[n];
                        n++;
                    }
                    Line[n] = 0;
                    MultiByteToWideChar(CP_ACP, 0, Line, -1, Format->Output, ARRAYSIZE(Format->Output));
                }
            }
            break;
        default:
            break;
    }
    return TRUE;
}

static DWORD
DmFormatRun(PDM_FORMAT Format, PCWSTR RootPath)
{
    HMODULE Module;
    DM_FORMATEX_FN FormatEx;
    WCHAR Root[MAX_PATH], Fs[DM_FS_NAME], Label[64];
    DWORD Error;

    Module = LoadLibraryW(L"fmifs.dll");
    if (Module == NULL)
        return DM_ERR_NO_PROVIDER;
    FormatEx = (DM_FORMATEX_FN)GetProcAddress(Module, "FormatEx");
    if (FormatEx == NULL)
    {
        FreeLibrary(Module);
        return DM_ERR_NO_PROVIDER;
    }

    if (InterlockedCompareExchange(&g_FormatLockInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_FormatLock);
        InterlockedExchange(&g_FormatLockInit, 2);
    }
    while (g_FormatLockInit != 2)
        Sleep(1);
    EnterCriticalSection(&g_FormatLock);
    StringCchCopyW(Root, ARRAYSIZE(Root), RootPath);
    StringCchCopyW(Fs, ARRAYSIZE(Fs), Format->FileSystem);
    StringCchCopyW(Label, ARRAYSIZE(Label), Format->Label);
    Format->Output[0] = 0;
    g_Format = Format;
    g_FormatDone = FALSE;
    g_FormatOk = FALSE;
    FormatEx(Root, Format->Removable ? FMIFS_MEDIA_REMOVABLE : FMIFS_MEDIA_HARDDISK, Fs, Label,
             Format->Quick ? TRUE : FALSE, Format->ClusterSize, DmFormatCallback);
    g_Format = NULL;
    Error = (g_FormatDone && g_FormatOk) ? ERROR_SUCCESS : DM_ERR_FORMAT_FAILED;
    LeaveCriticalSection(&g_FormatLock);
    FreeLibrary(Module);
    return Error;
}

/* The best path of a volume to hand to the formatter: a drive letter, else a folder. A path that does not fit or does not
 * lead back to the volume is never used, since a cut-off folder path can name an enclosing volume. */
static BOOL
DmPathLeadsTo(PCWSTR Path, PCWSTR Guid)
{
    WCHAR Resolved[MAX_PATH];

    return GetVolumeNameForVolumeMountPointW(Path, Resolved, ARRAYSIZE(Resolved)) && _wcsicmp(Resolved, Guid) == 0;
}

static BOOL
DmBestPath(PCWSTR Guid, PWSTR Path, SIZE_T Count)
{
    WCHAR Paths[512];
    DWORD Length = 0;
    PCWSTR Entry;
    BOOL Found = FALSE;

    ZeroMemory(Paths, sizeof(Paths));
    if (!GetVolumePathNamesForVolumeNameW(Guid, Paths, ARRAYSIZE(Paths) - 1, &Length))
        return FALSE;
    for (Entry = Paths; *Entry; Entry += wcslen(Entry) + 1)
    {
        if (!DmPathLeadsTo(Entry, Guid))
            continue;
        if (DmIsLetterPath(Entry))
        {
            if (SUCCEEDED(StringCchCopyW(Path, Count, Entry)))
                return TRUE;
            continue;
        }
        if (!Found && SUCCEEDED(StringCchCopyW(Path, Count, Entry)))
            Found = TRUE;
    }
    return Found;
}

DWORD
DmFormatVolume(PDM_FORMAT Format)
{
    WCHAR Best[MAX_PATH], Guid[MAX_PATH];
    BOOL IsGuid;
    DWORD Error;

    if (Format->Root[0] == 0 || Format->FileSystem[0] == 0)
        return ERROR_INVALID_PARAMETER;
    IsGuid = (wcsncmp(Format->Root, L"\\\\?\\Volume", 10) == 0);
    if (IsGuid)
        StringCchCopyW(Guid, ARRAYSIZE(Guid), Format->Root);
    else if (!(Format->Root[1] == L':' && DmIsLetterPath(Format->Root) &&
               GetVolumeNameForVolumeMountPointW(Format->Root, Guid, ARRAYSIZE(Guid))))
        Guid[0] = 0;
    if (Guid[0] && DmIsSystemVolume(Guid))
        return DM_ERR_PROTECTED;

    /* The providers of file systems do not all stop when they cannot lock the volume: some go on writing. So the volume is
     * locked and let go once here, and a volume that is in use is refused before any provider is called, unless the caller
     * has been told by the user that the volume may be dismounted. */
    if (Guid[0])
    {
        Error = DmLockDismount(Guid, Format->Force);
        if (Error == DM_ERR_VOLUME_IN_USE)
            return Error;
    }

    /* Windows formats only a volume that has a drive letter or a folder; ReactOS takes the volume name too */
    if (IsGuid && DmBestPath(Guid, Best, ARRAYSIZE(Best)))
        return DmFormatRun(Format, Best);
    Error = DmFormatRun(Format, Format->Root);
    if (Error == DM_ERR_FORMAT_FAILED && IsGuid)
    {
        DWORD Used = GetLogicalDrives();
        WCHAR Letter;

        for (Letter = L'Z'; Letter >= L'D'; Letter--)
        {
            WCHAR Root[4] = { Letter, L':', L'\\', 0 };

            if (Used & (1UL << (Letter - L'A')))
                continue;
            if (SetVolumeMountPointW(Root, Guid))
            {
                Error = DmFormatRun(Format, Root);
                DeleteVolumeMountPointW(Root);
            }
            break;
        }
    }
    return Error;
}

/* The file system a volume holds, from its first sector: Windows sets the partition type to match. */
static DWORD
DmProbeFat(PCWSTR VolumeGuid, PBOOL Fat12, PBOOL Fat16)
{
    WCHAR Device[MAX_PATH];
    HANDLE Handle;
    BYTE Sector[4096];
    DWORD Read = 0;

    *Fat12 = *Fat16 = FALSE;
    if (!DmTrimGuid(VolumeGuid, Device, ARRAYSIZE(Device)))
        return ERROR_BAD_PATHNAME;
    Handle = CreateFileW(Device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    /* a volume handle reads whole sectors */
    if (!ReadFile(Handle, Sector, sizeof(Sector), &Read, NULL) || Read < 512)
    {
        DWORD Error = GetLastError();

        CloseHandle(Handle);
        return Error ? Error : ERROR_READ_FAULT;
    }
    CloseHandle(Handle);
    if (memcmp(Sector + 0x36, "FAT12   ", 8) == 0)
        *Fat12 = TRUE;
    else if (memcmp(Sector + 0x36, "FAT16   ", 8) == 0)
        *Fat16 = TRUE;
    return ERROR_SUCCESS;
}

DWORD
DmFixPartitionType(ULONG Disk, ULONGLONG Offset, PCWSTR VolumeGuid)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    WCHAR Fs[32];
    BOOL Fat12 = FALSE, Fat16 = FALSE;
    PDL_ENTRY Entry;
    UCHAR Type;
    DWORD Error;

    if (Layout == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = DmReadDiskLayout(Disk, Layout);
    if (Error != ERROR_SUCCESS || Layout->Style != DM_STYLE_MBR)
        goto Done;
    Entry = DlFindEntry(Layout, Offset);
    if (Entry == NULL || Entry->Container)
    {
        Error = ERROR_NOT_FOUND;
        goto Done;
    }
    if (!GetVolumeInformationW(VolumeGuid, NULL, 0, NULL, NULL, NULL, Fs, ARRAYSIZE(Fs)))
    {
        Error = GetLastError();
        goto Done;
    }
    if (_wcsicmp(Fs, L"FAT") == 0)
    {
        /* without the answer the type would turn from a FAT type to 0x07, so leave it as it is */
        Error = DmProbeFat(VolumeGuid, &Fat12, &Fat16);
        if (Error != ERROR_SUCCESS)
            goto Done;
    }
    Type = DlMbrTypeForFileSystem(Fs, Fat12, Fat16, Entry->Offset, Entry->Length, Entry->MbrType);
    if (Type != Entry->MbrType)
    {
        Entry->MbrType = Type;
        Error = DmWriteDiskLayout(Disk, Layout);
    }
Done:
    HeapFree(GetProcessHeap(), 0, Layout);
    return Error;
}

/* ---------------------------------------------------------------------- partitions */

DWORD
DmPlanPartition(ULONG Disk, ULONGLONG FreeOffset, BOOL InExtended, PDL_FREE Free, PBOOL NeedsExtended, PDL_LAYOUT LayoutOut)
{
    DWORD Error = DmReadDiskLayout(Disk, LayoutOut);

    if (Error != ERROR_SUCCESS)
        return Error;
    if (!DlFindFreeFor(LayoutOut, FreeOffset, InExtended, Free))
        return DM_ERR_LAYOUT_BASE + DL_E_NO_SPACE;
    *NeedsExtended = DlWantsExtendedForNext(LayoutOut, Free);
    if (*NeedsExtended && Free->MaxLength > DL_ALIGN)
    {
        /* the extended partition takes a mebibyte for its boot record */
        Free->MaxLength -= DL_ALIGN;
        if (Free->WizardMax > Free->MaxLength)
            Free->WizardMax = Free->MaxLength;
    }
    return ERROR_SUCCESS;
}

/* The partition manager of ReactOS shows the reserved partition of a GPT disk as a volume without a file system, and the mount
 * manager gives it a drive letter; Windows shows no volume for it. The letter is taken away so that it stays free for the
 * volumes the user makes. */
static VOID
DmHideReserved(ULONG Disk, const DL_LAYOUT *Layout)
{
    ULONG i;

    for (i = 0; i < Layout->Count; i++)
    {
        WCHAR Guid[MAX_PATH], Paths[512];
        DWORD Length = 0;
        PCWSTR Path;

        if (memcmp(&Layout->Entry[i].GptType, &DlGuidReserved, sizeof(GUID)) != 0)
            continue;
        if (DmFindVolume(Disk, Layout->Entry[i].Offset, 1000, Guid, ARRAYSIZE(Guid)) != ERROR_SUCCESS)
            continue;
        ZeroMemory(Paths, sizeof(Paths));
        if (!GetVolumePathNamesForVolumeNameW(Guid, Paths, ARRAYSIZE(Paths) - 1, &Length))
            continue;
        for (Path = Paths; *Path; Path += wcslen(Path) + 1)
            DmRemoveMountPath(Path);
    }
}

DWORD
DmCreatePartition(ULONG Disk, ULONGLONG FreeOffset, BOOL InExtended, ULONGLONG Length, PDM_CREATED Created)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    DL_FREE Free;
    BOOL NeedsExtended;
    DWORD Error;
    DL_STATUS Status;
    GUID Id;
    PDL_ENTRY New = NULL;

    ZeroMemory(Created, sizeof(*Created));
    if (Layout == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = DmPlanPartition(Disk, FreeOffset, InExtended, &Free, &NeedsExtended, Layout);
    if (Error != ERROR_SUCCESS)
        goto Done;
    if (Length == 0 || Length > Free.MaxLength || (Length % DL_ALIGN) != 0)
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Done;
    }

    if (DlGptNeedsReserved(Layout))
    {
        DmNewGuid(&Id);
        Status = DlCreateReserved(Layout, &Id);
        if (Status != DL_OK)
        {
            Error = DmFromLayout(Status);
            goto Done;
        }
        /* Free was planned with the reserved partition in already */
        Created->MadeReserved = TRUE;
    }

    if (Layout->Style == DM_STYLE_MBR && NeedsExtended)
    {
        /* the extended partition takes the whole gap, and the drive the size that was asked for, as Windows does */
        ULONGLONG ExtOffset = DlAlignUp(Free.Offset);
        ULONGLONG ExtEnd = DlAlignDown(Free.Offset + Free.Length);

        Status = DlCreate(Layout, DL_NEW_EXTENDED, ExtOffset, ExtEnd - ExtOffset, 0, NULL, NULL, &New);
        if (Status != DL_OK)
        {
            Error = DmFromLayout(Status);
            goto Done;
        }
        Created->MadeExtended = TRUE;
        Status = DlCreate(Layout, DL_NEW_LOGICAL, ExtOffset + DL_ALIGN, Length, 0, NULL, NULL, &New);
        Created->Logical = TRUE;
    }
    else if (Layout->Style == DM_STYLE_MBR)
    {
        Created->Logical = InExtended;
        Status = DlCreate(Layout, InExtended ? DL_NEW_LOGICAL : DL_NEW_PRIMARY, Free.DataOffset, Length, 0, NULL, NULL, &New);
    }
    else
    {
        DmNewGuid(&Id);
        Status = DlCreate(Layout, DL_NEW_PRIMARY, Free.DataOffset, Length, 0, &DlGuidBasicData, &Id, &New);
        if (Status == DL_OK)
            StringCchCopyW(New->GptName, ARRAYSIZE(New->GptName), L"Basic data partition");
    }
    if (Status != DL_OK)
    {
        Error = DmFromLayout(Status);
        goto Done;
    }
    Created->Offset = New->Offset;
    Created->Length = New->Length;

    Error = DmWriteDiskLayout(Disk, Layout);
    if (Error != ERROR_SUCCESS)
        goto Done;
    /* the volume manager adds the volume a moment after the table is written */
    if (DmFindVolume(Disk, Created->Offset, 15000, Created->VolumeGuid, ARRAYSIZE(Created->VolumeGuid)) != ERROR_SUCCESS)
        Created->VolumeGuid[0] = 0;
    if (Created->MadeReserved)
        DmHideReserved(Disk, Layout);
Done:
    HeapFree(GetProcessHeap(), 0, Layout);
    return Error;
}

/* the number of the disk that holds the Windows directory, or ((ULONG)-1) */
static ULONG
DmSystemDisk(VOID)
{
    WCHAR Windows[MAX_PATH], Root[4], Guid[MAX_PATH], Device[MAX_PATH];
    union { VOLUME_DISK_EXTENTS Extents; BYTE Raw[512]; } Buffer;
    HANDLE Handle;
    DWORD Returned;
    ULONG Disk = ((ULONG)-1);

    if (GetWindowsDirectoryW(Windows, ARRAYSIZE(Windows)) < 2 || Windows[1] != L':')
        return ((ULONG)-1);
    Root[0] = Windows[0]; Root[1] = L':'; Root[2] = L'\\'; Root[3] = 0;
    if (!GetVolumeNameForVolumeMountPointW(Root, Guid, ARRAYSIZE(Guid)) || !DmTrimGuid(Guid, Device, ARRAYSIZE(Device)))
        return ((ULONG)-1);
    Handle = CreateFileW(Device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return ((ULONG)-1);
    ZeroMemory(&Buffer, sizeof(Buffer));
    if (DmIoctl(Handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &Buffer, sizeof(Buffer), &Returned) &&
        Buffer.Extents.NumberOfDiskExtents >= 1)
        Disk = Buffer.Extents.Extents[0].DiskNumber;
    CloseHandle(Handle);
    return Disk;
}

/* The partition the boot files are on: the system partition that the kernel recorded in the registry at start up, as the
 * target of the boot symbolic link, resolved to a disk and a start offset. */
BOOL
DmIsBootPartition(ULONG Disk, ULONGLONG Offset)
{
    HKEY Key;
    WCHAR Value[260], Path[300];
    DWORD Size = sizeof(Value) - sizeof(WCHAR), Type = 0;
    HANDLE Handle;
    STORAGE_DEVICE_NUMBER Number;
    PARTITION_INFORMATION_EX Info;
    BOOL Result = FALSE;

    ZeroMemory(Value, sizeof(Value));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(Key, L"SystemPartition", NULL, &Type, (LPBYTE)Value, &Size) != ERROR_SUCCESS || Type != REG_SZ ||
        Value[0] != L'\\')
    {
        RegCloseKey(Key);
        return FALSE;
    }
    RegCloseKey(Key);
    if (FAILED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"\\\\?\\GLOBALROOT%s", Value)))
        return FALSE;
    Handle = CreateFileW(Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    ZeroMemory(&Number, sizeof(Number));
    ZeroMemory(&Info, sizeof(Info));
    if (DmIoctl(Handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &Number, sizeof(Number), NULL) &&
        Number.DeviceNumber == Disk &&
        DmIoctl(Handle, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &Info, sizeof(Info), NULL) &&
        (ULONGLONG)Info.StartingOffset.QuadPart == Offset)
        Result = TRUE;
    CloseHandle(Handle);
    return Result;
}

/* The partitions the running system depends on. The display flags of the model are not trusted for this: the volume of the
 * Windows directory, the system partition the kernel recorded, and every EFI system partition. */
static BOOL
DmIsProtectedPartition(ULONG Disk, const DL_LAYOUT *Layout, const DL_ENTRY *Entry, PCWSTR CurrentGuid)
{
    if (Layout->Style == DM_STYLE_GPT && memcmp(&Entry->GptType, &DlGuidEfi, sizeof(GUID)) == 0)
        return TRUE;
    if (CurrentGuid[0] && DmIsSystemVolume(CurrentGuid))
        return TRUE;
    if (DmIsBootPartition(Disk, Entry->Offset))
        return TRUE;
    if (Layout->Style == DM_STYLE_MBR && Entry->Active && DmSystemDisk() == Disk)
        return TRUE;
    return FALSE;
}

/* The paths the volume has now: the system is asked, not the list the caller saw. */
static DWORD
DmCurrentPaths(PCWSTR VolumeGuid, PWSTR *Paths)
{
    DWORD Length = 512;

    *Paths = NULL;
    for (;;)
    {
        PWSTR Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)Length + 2) * sizeof(WCHAR));
        DWORD Needed = 0, Error;

        if (Buffer == NULL)
            return ERROR_NOT_ENOUGH_MEMORY;
        if (GetVolumePathNamesForVolumeNameW(VolumeGuid, Buffer, Length, &Needed))
        {
            *Paths = Buffer;
            return ERROR_SUCCESS;
        }
        Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Buffer);
        if (Error != ERROR_MORE_DATA || Needed <= Length || Needed > 65536)
            return Error ? Error : ERROR_GEN_FAILURE;
        Length = Needed;
    }
}

/* Deletes the partition at Offset, which must still be the one the caller saw: Length and the volume are what it had then. */
DWORD
DmDeletePartition(ULONG Disk, ULONGLONG Offset, ULONGLONG Length, PCWSTR VolumeGuid, BOOL Force)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    WCHAR Current[MAX_PATH];
    PWSTR Paths = NULL;
    PCWSTR Todo[DM_MAX_MOUNT_PATHS], Path;
    ULONG TodoCount = 0, Removed, i;
    PDL_ENTRY Entry;
    BOOL Dismounted = FALSE, HaveGuid;
    DWORD Error;

    if (Layout == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = DmReadDiskLayout(Disk, Layout);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Entry = DlFindEntry(Layout, Offset);
    if (Entry == NULL)
    {
        Error = DM_ERR_LAYOUT_BASE + DL_E_NOT_FOUND;
        goto Cleanup;
    }

    /* the partition and its volume must be what was confirmed */
    HaveGuid = (VolumeGuid && VolumeGuid[0]);
    if (Entry->Length != Length)
    {
        Error = DM_ERR_CHANGED;
        goto Cleanup;
    }
    if (DmFindVolume(Disk, Offset, 0, Current, ARRAYSIZE(Current)) != ERROR_SUCCESS)
        Current[0] = 0;
    if (HaveGuid ? (_wcsicmp(Current, VolumeGuid) != 0) : (Current[0] != 0))
    {
        Error = DM_ERR_CHANGED;
        goto Cleanup;
    }
    if (DmIsProtectedPartition(Disk, Layout, Entry, Current))
    {
        Error = DM_ERR_PROTECTED;
        goto Cleanup;
    }

    if (Current[0])
    {
        Error = DmCurrentPaths(Current, &Paths);
        if (Error != ERROR_SUCCESS)
            goto Cleanup;
        /* the paths that lead to this volume; the others are not ours to remove */
        for (Path = Paths; *Path; Path = DmNextPath(Path))
        {
            if (!DmPathLeadsTo(Path, Current))
                continue;
            if (TodoCount == ARRAYSIZE(Todo))
            {
                Error = ERROR_TOO_MANY_NAMES;
                goto Cleanup;
            }
            Todo[TodoCount++] = Path;
        }
        Error = DmLockDismount(Current, Force);
        if (Error != ERROR_SUCCESS)
            goto Cleanup;
        Dismounted = TRUE;
        /* every path of this volume goes, or the partition stays: a mount point that is left behind would name nothing */
        for (Removed = 0; Removed < TodoCount; Removed++)
        {
            Error = DmRemoveMountPath(Todo[Removed]);
            if (Error != ERROR_SUCCESS)
            {
                for (i = 0; i < Removed; i++)
                    DmAddMountPath(Current, Todo[i]);
                goto Cleanup;
            }
        }
    }
    Error = DmFromLayout(DlDelete(Layout, Offset));
    if (Error == ERROR_SUCCESS)
        Error = DmWriteDiskLayout(Disk, Layout);
    if (Error != ERROR_SUCCESS && Dismounted)
    {
        /* the table did not change: give the paths back */
        for (i = 0; i < TodoCount; i++)
            DmAddMountPath(Current, Todo[i]);
    }
Cleanup:
    if (Paths)
        HeapFree(GetProcessHeap(), 0, Paths);
    HeapFree(GetProcessHeap(), 0, Layout);
    return Error;
}

DWORD
DmMarkActive(ULONG Disk, ULONGLONG Offset)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    DWORD Error;

    if (Layout == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = DmReadDiskLayout(Disk, Layout);
    if (Error == ERROR_SUCCESS)
        Error = DmFromLayout(DlSetActive(Layout, Offset, TRUE));
    if (Error == ERROR_SUCCESS)
        Error = DmWriteDiskLayout(Disk, Layout);
    HeapFree(GetProcessHeap(), 0, Layout);
    return Error;
}
