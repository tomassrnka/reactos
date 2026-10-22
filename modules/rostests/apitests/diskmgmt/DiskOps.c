/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Live test of the writer of Disk Management on a scratch disk
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The test changes a disk, so it runs only when the environment variable DISKMGMT_TEST_DISK names the number
 * of an empty scratch disk (no partitions) that does not hold the running system. Without it the test only
 * says that it skipped. It is the same sequence on ReactOS and on Windows.
 */

#include <apitest.h>
#include <wchar.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <strsafe.h>

#include "diskops.h"

#define MiB (1024ULL * 1024)

static ULONG g_Disk;
static ULONG g_Disk2;   /* a second scratch disk (DISKMGMT_TEST_DISK2), 0 when there is none: the swapped-disk tests need it */
static BOOL g_HaveDisk2;
static BOOL g_Keep;     /* leave the partitions and the files, so that the disk image can be checked offline */

static PDL_LAYOUT
ReadLayout(DWORD *Error)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));

    *Error = DmReadDiskLayout(g_Disk, Layout);
    return Layout;
}

/* Deletes the partition the way the program does: with the length it saw. */
static DWORD
Del(ULONGLONG Offset, PCWSTR Guid, BOOL Force)
{
    DWORD Error;
    PDL_LAYOUT Layout = ReadLayout(&Error);
    PDL_ENTRY Entry = Error == ERROR_SUCCESS ? DlFindEntry(Layout, Offset) : NULL;
    ULONGLONG Length = Entry ? Entry->Length : 0;

    HeapFree(GetProcessHeap(), 0, Layout);
    return DmDeletePartition(g_Disk, Offset, Length, Guid, Force, NULL);
}

static ULONG
CountEntries(DM_STYLE *Style)
{
    DWORD Error;
    PDL_LAYOUT Layout = ReadLayout(&Error);
    ULONG Count = 0;

    if (Error == ERROR_SUCCESS)
    {
        Count = Layout->Count;
        if (Style)
            *Style = Layout->Style;
    }
    else if (Style)
    {
        *Style = DM_STYLE_RAW;
    }
    HeapFree(GetProcessHeap(), 0, Layout);
    return Count;
}

static WCHAR
FreeLetter(VOID)
{
    DWORD Used = GetLogicalDrives();
    WCHAR Letter;

    for (Letter = L'Z'; Letter >= L'D'; Letter--)
        if (!(Used & (1UL << (Letter - L'A'))))
            return Letter;
    return 0;
}

static BOOL
HasPath(PCWSTR Guid, PCWSTR Wanted)
{
    WCHAR Paths[512];
    DWORD Length = 0;
    PCWSTR Path;

    ZeroMemory(Paths, sizeof(Paths));
    if (!GetVolumePathNamesForVolumeNameW(Guid, Paths, ARRAYSIZE(Paths) - 1, &Length))
        return FALSE;
    for (Path = Paths; *Path; Path += wcslen(Path) + 1)
        if (_wcsicmp(Path, Wanted) == 0)
            return TRUE;
    return FALSE;
}

static VOID
PathsOf(PCWSTR Guid, PWSTR Paths, DWORD Count)
{
    DWORD Length = 0;

    ZeroMemory(Paths, Count * sizeof(WCHAR));
    GetVolumePathNamesForVolumeNameW(Guid, Paths, Count - 1, &Length);
}

/* the program's own view of the disk, read again as after every action */
static PDM_REGION
ModelRegion(PDM_MODEL *Model, ULONGLONG Offset)
{
    PDM_DISK Disk;

    *Model = DmCollect();
    if (*Model == NULL)
        return NULL;
    return DmFindRegion(*Model, DM_DEV_DISK, g_Disk, Offset, &Disk);
}

static BOOL
WriteReadFile(PCWSTR Root, PCSTR Text)  /* Root is the volume name, so that the file can only land on that volume */
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD Done;
    CHAR Back[64];
    BOOL Ok = FALSE;

    /* only a volume name is a root that can be trusted: an empty or broken one would put the file in the current directory */
    if (wcsncmp(Root, L"\\\\?\\Volume{", 11) != 0)
        return FALSE;
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%shello.txt", Root);
    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    WriteFile(File, Text, (DWORD)strlen(Text), &Done, NULL);
    CloseHandle(File);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    ZeroMemory(Back, sizeof(Back));
    if (ReadFile(File, Back, sizeof(Back) - 1, &Done, NULL) && Done == strlen(Text) && strcmp(Back, Text) == 0)
        Ok = TRUE;
    CloseHandle(File);
    if (!g_Keep)
        DeleteFileW(Path);
    return Ok;
}

static BOOL
Format(PCWSTR Guid, PCWSTR Fs, PCWSTR Label, BOOL Quick)
{
    DM_FORMAT Fmt;
    DWORD Error;

    ZeroMemory(&Fmt, sizeof(Fmt));
    StringCchCopyW(Fmt.Root, ARRAYSIZE(Fmt.Root), Guid);
    StringCchCopyW(Fmt.FileSystem, ARRAYSIZE(Fmt.FileSystem), Fs);
    StringCchCopyW(Fmt.Label, ARRAYSIZE(Fmt.Label), Label);
    Fmt.Quick = Quick;
    Error = DmFormatVolume(&Fmt);
    if (Error == DM_ERR_VOLUME_IN_USE)
    {
        /* a program, such as the shell looking at the new drive letter, holds the volume: as the user would, allow the dismount */
        Fmt.Force = TRUE;
        Error = DmFormatVolume(&Fmt);
    }
    if (Error != ERROR_SUCCESS)
        trace("format %ls failed: 0x%lx %ls\n", Fs, Error, Fmt.Output);
    return Error == ERROR_SUCCESS;
}

static BOOL
VolumeInfo(PCWSTR Guid, PWSTR Label, PWSTR Fs)
{
    return GetVolumeInformationW(Guid, Label, 64, NULL, NULL, NULL, Fs, 32);
}

static DWORD
Create(ULONGLONG Offset, BOOL InExtended, ULONGLONG LengthMiB, DM_CREATED *Created)
{
    return DmCreatePartition(g_Disk, Offset, InExtended, LengthMiB * MiB, NULL, NULL, Created);
}

/* ---- the safety of the writer: the disk the user saw, the table the plan was made on, a write that did not work ---- */

static VOID
GetId(PDM_DISK_ID Id)
{
    DWORD Error;
    PDL_LAYOUT Layout = ReadLayout(&Error);

    ZeroMemory(Id, sizeof(*Id));
    ok_int(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        Id->Style = Layout->Style;
        Id->Length = Layout->DiskLength;
        Id->SectorSize = Layout->SectorSize;
        Id->Signature = Layout->Signature;
        Id->DiskId = Layout->DiskId;
    }
    HeapFree(GetProcessHeap(), 0, Layout);
}

#define RAW_HEAD_BYTES (34 * 512)

/* the first sectors of the disk as they are on it, not as the cache has them */
static BOOL
ReadRawHead(PBYTE Buffer)
{
    WCHAR Path[64];
    HANDLE Device;
    DWORD Done = 0;
    BOOL Ok;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"\\\\.\\PhysicalDrive%lu", g_Disk);
    Device = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = ReadFile(Device, Buffer, RAW_HEAD_BYTES, &Done, NULL) && Done == RAW_HEAD_BYTES;
    CloseHandle(Device);
    return Ok;
}

static LONG g_Writes;
static BOOL g_SkipRestore;

/* the first write puts down a table that is not the one asked for; the write back is made or not as the test says */
static BOOL
CorruptingHook(ULONG Point, ULONG Disk, PVOID Data)
{
    if (Point != DM_FAULT_LAYOUT_WRITE)
        return FALSE;
    if (InterlockedIncrement(&g_Writes) == 1)
    {
        PDL_LAYOUT Layout = Data;

        if (Layout->Count)
            Layout->Entry[0].Length -= MiB;
        return FALSE;
    }
    return g_SkipRestore;
}

static BOOL
TrashingHook(ULONG Point, ULONG Disk, PVOID Data)
{
    if (Point == DM_FAULT_DISK_CREATE)
    {
        WCHAR Path[64];
        HANDLE Device;
        PBYTE Sector = VirtualAlloc(NULL, 512, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD Done;

        StringCchPrintfW(Path, ARRAYSIZE(Path), L"\\\\.\\PhysicalDrive%lu", Disk);
        Device = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                             FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
        if (Sector && Device != INVALID_HANDLE_VALUE)
        {
            memset(Sector, 0xA5, 512);
            WriteFile(Device, Sector, 512, &Done, NULL);
            SetFilePointer(Device, 512, NULL, FILE_BEGIN);
            WriteFile(Device, Sector, 512, &Done, NULL);
        }
        if (Device != INVALID_HANDLE_VALUE)
            CloseHandle(Device);
        if (Sector)
            VirtualFree(Sector, 0, MEM_RELEASE);
        return TRUE;
    }
    return FALSE;
}

/* The disk is an empty MBR disk of the test geometry; it is one again when this returns. */
/* Sectors of any disk as they are on it; Offset counts from the start, or from the end when Tail is set. */
static BOOL
ReadRawOf(ULONG Disk, BOOL Tail, PBYTE Buffer, ULONG Bytes)
{
    WCHAR Path[64];
    HANDLE Device;
    DWORD Done = 0;
    GET_LENGTH_INFORMATION Length;
    LARGE_INTEGER Position;
    BOOL Ok = FALSE;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"\\\\.\\PhysicalDrive%lu", Disk);
    Device = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;
    Position.QuadPart = 0;
    if (!Tail || DeviceIoControl(Device, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &Length, sizeof(Length), &Done, NULL))
    {
        if (Tail)
            Position.QuadPart = Length.Length.QuadPart - Bytes;
        Ok = SetFilePointerEx(Device, Position, NULL, FILE_BEGIN) && ReadFile(Device, Buffer, Bytes, &Done, NULL) && Done == Bytes;
    }
    CloseHandle(Device);
    return Ok;
}

#define RAW_TAIL_BYTES (33 * 512)

static BOOL g_SwapNow;          /* every handle for g_Disk goes to g_Disk2 from now on */
static BOOL g_SwapFirstRaw;     /* only the first raw handle for g_Disk goes to g_Disk2 */
static LONG g_RawOpens;

/* The creation of a table fails and from then on the number of the disk leads to the other disk (or the first raw open already
 * leads there): what an operation does with a number it used before is seen on the other disk. */
static BOOL
SwapHook(ULONG Point, ULONG Disk, PVOID Data)
{
    if (Point == DM_FAULT_DISK_CREATE)
    {
        g_SwapNow = !g_SwapFirstRaw;
        return TRUE;
    }
    if ((Point == DM_FAULT_OPEN_DISK || Point == DM_FAULT_OPEN_RAW) && Disk == g_Disk && *(PULONG)Data == g_Disk)
    {
        if (g_SwapFirstRaw)
        {
            if (Point == DM_FAULT_OPEN_RAW && InterlockedIncrement(&g_RawOpens) == 1)
                *(PULONG)Data = g_Disk2;
        }
        else if (g_SwapNow)
        {
            *(PULONG)Data = g_Disk2;
        }
    }
    return FALSE;
}

/* a driver that reports success for a table it did not write */
static BOOL
SilentHook(ULONG Point, ULONG Disk, PVOID Data)
{
    return Point == DM_FAULT_DISK_CREATE_SILENT;
}

static VOID
Test_Initialize(const DM_DISK_ID *Right)
{
    DM_DISK_ID Now;

    /* the signature that was put into the table is read back: a table that is still the old one is no success, even when it is
     * empty and of the style that was asked for */
    DmSetFaultHook(SilentHook);
    ok_int(DmInitializeDisk(g_Disk, DM_STYLE_MBR, Right), DM_ERR_VERIFY_FAILED);
    DmSetFaultHook(NULL);
    GetId(&Now);
    ok(memcmp(&Now, Right, sizeof(*Right)) == 0, "the disk is not the same disk after the table that was not written\n");

    if (!g_HaveDisk2)
    {
        skip("DISKMGMT_TEST_DISK2 names no second scratch disk: the swapped-disk tests are not run\n");
        return;
    }
    {
        PBYTE A1 = VirtualAlloc(NULL, RAW_HEAD_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        PBYTE A2 = VirtualAlloc(NULL, RAW_HEAD_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        PBYTE BH1 = VirtualAlloc(NULL, RAW_HEAD_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        PBYTE BH2 = VirtualAlloc(NULL, RAW_HEAD_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        PBYTE BT1 = VirtualAlloc(NULL, RAW_TAIL_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        PBYTE BT2 = VirtualAlloc(NULL, RAW_TAIL_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        if (!A1 || !A2 || !BH1 || !BH2 || !BT1 || !BT2 ||
            !ReadRawOf(g_Disk, FALSE, A1, RAW_HEAD_BYTES) || !ReadRawOf(g_Disk2, FALSE, BH1, RAW_HEAD_BYTES) ||
            !ReadRawOf(g_Disk2, TRUE, BT1, RAW_TAIL_BYTES))
        {
            skip("the sectors of the two disks cannot be read\n");
        }
        else
        {
            /* the creation fails and the number then leads to the other disk: the sectors that were kept go back to the disk
             * they came from, and the other disk is not written */
            g_SwapNow = FALSE;
            g_SwapFirstRaw = FALSE;
            DmSetFaultHook(SwapHook);
            ok_int(DmInitializeDisk(g_Disk, DM_STYLE_GPT, Right), ERROR_WRITE_FAULT);
            DmSetFaultHook(NULL);
            g_SwapNow = FALSE;
            ok(ReadRawOf(g_Disk, FALSE, A2, RAW_HEAD_BYTES) && memcmp(A1, A2, RAW_HEAD_BYTES) == 0,
               "the first sectors of the disk are not as they were\n");
            ok(ReadRawOf(g_Disk2, FALSE, BH2, RAW_HEAD_BYTES) && memcmp(BH1, BH2, RAW_HEAD_BYTES) == 0,
               "the rollback wrote the first sectors of the other disk\n");
            ok(ReadRawOf(g_Disk2, TRUE, BT2, RAW_TAIL_BYTES) && memcmp(BT1, BT2, RAW_TAIL_BYTES) == 0,
               "the rollback wrote the last sectors of the other disk\n");

            /* the number leads to the other disk already when the handle is opened: nothing is written to either disk */
            g_SwapFirstRaw = TRUE;
            g_RawOpens = 0;
            DmSetFaultHook(SwapHook);
            ok_int(DmInitializeDisk(g_Disk, DM_STYLE_GPT, Right), DM_ERR_CHANGED);
            DmSetFaultHook(NULL);
            g_SwapFirstRaw = FALSE;
            ok(ReadRawOf(g_Disk, FALSE, A2, RAW_HEAD_BYTES) && memcmp(A1, A2, RAW_HEAD_BYTES) == 0,
               "the first sectors of the disk changed after the refused operation\n");
            ok(ReadRawOf(g_Disk2, FALSE, BH2, RAW_HEAD_BYTES) && memcmp(BH1, BH2, RAW_HEAD_BYTES) == 0 &&
               ReadRawOf(g_Disk2, TRUE, BT2, RAW_TAIL_BYTES) && memcmp(BT1, BT2, RAW_TAIL_BYTES) == 0,
               "the other disk changed after the refused operation\n");
        }
        if (A1) VirtualFree(A1, 0, MEM_RELEASE);
        if (A2) VirtualFree(A2, 0, MEM_RELEASE);
        if (BH1) VirtualFree(BH1, 0, MEM_RELEASE);
        if (BH2) VirtualFree(BH2, 0, MEM_RELEASE);
        if (BT1) VirtualFree(BT1, 0, MEM_RELEASE);
        if (BT2) VirtualFree(BT2, 0, MEM_RELEASE);
    }
}

static VOID
Test_Safety(VOID)
{
    DM_DISK_ID Right, Wrong;
    DM_CREATED P, Junk;
    DL_FREE Free;
    BOOL NeedsExtended;
    PDL_LAYOUT Stale = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    PDL_LAYOUT Empty, WithP, Now;
    PBYTE Head1 = VirtualAlloc(NULL, RAW_HEAD_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    PBYTE Head2 = VirtualAlloc(NULL, RAW_HEAD_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WCHAR Free1, Root[8];
    DWORD Error;

    GetId(&Right);
    Wrong = Right;
    Wrong.Signature ^= 0x5A5A5A5A;
    ok_int(DmVerifyDiskId(g_Disk, &Right), ERROR_SUCCESS);
    ok_int(DmVerifyDiskId(g_Disk, NULL), ERROR_SUCCESS);
    ok_int(DmVerifyDiskId(g_Disk, &Wrong), DM_ERR_CHANGED);
    Wrong = Right;
    Wrong.Length += 512;
    ok_int(DmVerifyDiskId(g_Disk, &Wrong), DM_ERR_CHANGED);
    Wrong = Right;
    Wrong.Style = DM_STYLE_GPT;
    ok_int(DmVerifyDiskId(g_Disk, &Wrong), DM_ERR_CHANGED);

    /* another disk behind the number: nothing is written, whatever the operation */
    Wrong = Right;
    Wrong.Signature ^= 0x5A5A5A5A;
    ok_int(DmCreatePartition(g_Disk, 1 * MiB, FALSE, 50 * MiB, NULL, &Wrong, &Junk), DM_ERR_CHANGED);
    ok_int(DmMarkActive(g_Disk, 1 * MiB, &Wrong), DM_ERR_CHANGED);
    ok_int(DmDeletePartition(g_Disk, 1 * MiB, 50 * MiB, NULL, FALSE, &Wrong), DM_ERR_CHANGED);
    ok_int(DmInitializeDisk(g_Disk, DM_STYLE_GPT, &Wrong), DM_ERR_CHANGED);
    ok_int(DmConvertDisk(g_Disk, DM_STYLE_GPT, &Wrong), DM_ERR_CHANGED);
    {
        DM_STYLE Style;

        ok_int(CountEntries(&Style), 0);
        ok_int(Style, DM_STYLE_MBR);
    }
    GetId(&Wrong);
    ok(memcmp(&Wrong, &Right, sizeof(Right)) == 0, "the disk is not the same disk after the refused operations\n");

    /* the table that a plan was made on is the one that is written to: a table that changed in between is refused */
    Empty = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    ok_int(DmPlanPartition(g_Disk, 1 * MiB, FALSE, &Free, &NeedsExtended, Stale), ERROR_SUCCESS);
    ok_int(DmReadDiskLayout(g_Disk, Empty), ERROR_SUCCESS);
    ok_int(DmCreatePartition(g_Disk, 1 * MiB, FALSE, 50 * MiB, Stale, &Right, &P), ERROR_SUCCESS);
    ok_int(CountEntries(NULL), 1);
    ok_int(DmCreatePartition(g_Disk, 200 * MiB, FALSE, 50 * MiB, Stale, &Right, &Junk), DM_ERR_CHANGED);
    ok_int(CountEntries(NULL), 1);
    WithP = ReadLayout(&Error);
    ok_int(Error, ERROR_SUCCESS);

    /* a layout made on the old table is not written over the new one (the disk is not touched) */
    ok_int(DmWriteDiskLayout(g_Disk, Empty, Empty), DM_ERR_CHANGED);
    ok_int(CountEntries(NULL), 1);
    ok_int(DmWriteDiskLayout(g_Disk, WithP, WithP), ERROR_SUCCESS);

    /* a write that does not give the disk the table: the old table is written back and the disk is read to see it */
    g_Writes = 0;
    g_SkipRestore = FALSE;
    DmSetFaultHook(CorruptingHook);
    ok_int(DmWriteDiskLayout(g_Disk, WithP, WithP), DM_ERR_VERIFY_FAILED);
    DmSetFaultHook(NULL);
    Now = ReadLayout(&Error);
    ok_int(Error, ERROR_SUCCESS);
    ok(Error == ERROR_SUCCESS && Now->Count == 1 && Now->Entry[0].Length == WithP->Entry[0].Length, "the old table is not back\n");
    HeapFree(GetProcessHeap(), 0, Now);

    /* the write back that the driver accepts and does not do is found out, not believed */
    g_Writes = 0;
    g_SkipRestore = TRUE;
    DmSetFaultHook(CorruptingHook);
    ok_int(DmWriteDiskLayout(g_Disk, WithP, WithP), DM_ERR_RESTORE_FAILED);
    DmSetFaultHook(NULL);
    Now = ReadLayout(&Error);
    ok(Error == ERROR_SUCCESS && Now->Count == 1 && Now->Entry[0].Length == WithP->Entry[0].Length - MiB,
       "the test did not leave the wrong table on the disk\n");
    HeapFree(GetProcessHeap(), 0, Now);
    ok_int(DmWriteDiskLayout(g_Disk, NULL, WithP), ERROR_SUCCESS);
    ok_int(CountEntries(NULL), 1);
    /* the rewrites took the volume away and brought one back: it is looked up again */
    ok_int(DmFindVolume(g_Disk, P.Offset, 15000, P.VolumeGuid, ARRAYSIZE(P.VolumeGuid)), ERROR_SUCCESS);

    /* a drive letter or a path is taken away only from the volume it leads to; one that cannot be resolved is left alone */
    Free1 = FreeLetter();
    if (Free1 && P.VolumeGuid[0])
    {
        StringCchPrintfW(Root, ARRAYSIZE(Root), L"%c:\\", Free1);
        ok_int(DmRemoveMountPathOf(P.VolumeGuid, Root), DM_ERR_CHANGED);
        /* a format of a root that names no volume is refused before anything is written */
        {
            DM_FORMAT Fmt;

            ZeroMemory(&Fmt, sizeof(Fmt));
            StringCchCopyW(Fmt.Root, ARRAYSIZE(Fmt.Root), Root);
            StringCchCopyW(Fmt.FileSystem, ARRAYSIZE(Fmt.FileSystem), L"FAT32");
            ok_int(DmFormatVolume(&Fmt), ERROR_PATH_NOT_FOUND);
        }
        /* a volume that another handle has locked is refused unless the user said it may be dismounted */
        {
            WCHAR Device[MAX_PATH];
            HANDLE Held;
            DM_FORMAT Fmt;
            DWORD Returned;

            StringCchCopyW(Device, ARRAYSIZE(Device), P.VolumeGuid);
            Device[wcslen(Device) - 1] = 0;
            Held = CreateFileW(Device, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            ok(Held != INVALID_HANDLE_VALUE, "the volume cannot be opened: %lu\n", GetLastError());
            if (Held != INVALID_HANDLE_VALUE)
            {
                ok(DeviceIoControl(Held, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Returned, NULL), "the volume cannot be locked: %lu\n", GetLastError());
                ZeroMemory(&Fmt, sizeof(Fmt));
                StringCchCopyW(Fmt.Root, ARRAYSIZE(Fmt.Root), P.VolumeGuid);
                StringCchCopyW(Fmt.FileSystem, ARRAYSIZE(Fmt.FileSystem), L"FAT32");
                /* ReactOS refuses the open of a volume that is locked, Windows opens it and the lock fails */
                Returned = DmFormatVolume(&Fmt);
                ok(Returned == DM_ERR_VOLUME_IN_USE || Returned == ERROR_ACCESS_DENIED || Returned == ERROR_SHARING_VIOLATION,
                   "a locked volume was formatted or the refusal is unknown: 0x%lx\n", Returned);
                DeviceIoControl(Held, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &Returned, NULL);
                CloseHandle(Held);
            }
        }
    }

    /* a table that is made and does not come out right: the sectors that were there are put back */
    ok_int(DmDeletePartition(g_Disk, P.Offset, P.Length, P.VolumeGuid[0] ? P.VolumeGuid : NULL, FALSE, &Right), ERROR_SUCCESS);
    ok_int(CountEntries(NULL), 0);
    if (Head1 && Head2 && ReadRawHead(Head1))
    {
        DM_STYLE Style;

        DmSetFaultHook(TrashingHook);
        ok(DmInitializeDisk(g_Disk, DM_STYLE_GPT, &Right) == ERROR_WRITE_FAULT, "the failed creation of a table is not reported\n");
        ok(DmConvertDisk(g_Disk, DM_STYLE_GPT, &Right) == ERROR_WRITE_FAULT, "the failed conversion is not reported\n");
        DmSetFaultHook(NULL);
        ok(ReadRawHead(Head2) && memcmp(Head1, Head2, RAW_HEAD_BYTES) == 0, "the first sectors of the disk are not as they were\n");
        ok_int(CountEntries(&Style), 0);
        ok_int(Style, DM_STYLE_MBR);
        GetId(&Wrong);
        ok(memcmp(&Wrong, &Right, sizeof(Right)) == 0, "the disk is not the same disk after the failed creation\n");
        Test_Initialize(&Right);
    }
    else
    {
        skip("the first sectors of the disk cannot be read\n");
    }

    HeapFree(GetProcessHeap(), 0, Stale);
    HeapFree(GetProcessHeap(), 0, Empty);
    HeapFree(GetProcessHeap(), 0, WithP);
    if (Head1)
        VirtualFree(Head1, 0, MEM_RELEASE);
    if (Head2)
        VirtualFree(Head2, 0, MEM_RELEASE);
}

static VOID
Test_Mbr(VOID)
{
    DM_FS_LIST Fs;
    DM_CREATED P1, P2, P3, P5, P6;
    DL_FREE Free;
    DL_LAYOUT *Plan = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    BOOL NeedsExtended;
    DWORD Error;
    WCHAR Letter[4], Root[8], Label[64], Name[32], Paths[512];
    WCHAR L1;
    PDM_MODEL Model;
    PDM_REGION Region;
    DM_STYLE Style;
    PDL_LAYOUT Layout;
    ULONG i;

    ok_int(DmInitializeDisk(g_Disk, DM_STYLE_MBR, NULL), ERROR_SUCCESS);
    ok_int(CountEntries(&Style), 0);
    ok_int(Style, DM_STYLE_MBR);
    Test_Safety();

    /* the program sees the disk as an initialized MBR disk with all of it free */
    Region = ModelRegion(&Model, 1 * MiB);
    ok(Region != NULL && Region->Kind == DM_KIND_UNALLOCATED, "model: no unallocated block at 1 MB\n");
    DmFreeModel(Model);

    /* the file systems the system can format */
    ok(DmQueryFileSystems(&Fs), "no file system to format with\n");
    trace("file systems:");
    for (i = 0; i < Fs.Count; i++)
        trace(" %ls", Fs.Name[i]);
    trace("\n");

    /* first volume: 100 MB at 1 MB */
    Error = DmPlanPartition(g_Disk, 1 * MiB, FALSE, &Free, &NeedsExtended, Plan);
    ok_int(Error, ERROR_SUCCESS);
    ok(Free.DataOffset == 1 * MiB, "data offset %I64u\n", Free.DataOffset);
    ok(!NeedsExtended, "first volume would be extended\n");
    Error = Create(1 * MiB, FALSE, 100, &P1);
    ok_int(Error, ERROR_SUCCESS);
    ok(P1.Offset == 1 * MiB && P1.Length == 100 * MiB, "created %I64u/%I64u\n", P1.Offset, P1.Length);
    ok(P1.VolumeGuid[0] != 0, "no volume for the new partition\n");
    if (P1.VolumeGuid[0] == 0)
    {
        /* every later step needs the volume: stop before anything is formatted or written */
        HeapFree(GetProcessHeap(), 0, Plan);
        return;
    }
    ok_int(CountEntries(NULL), 1);

    /* a new volume is not formatted */
    if (VolumeInfo(P1.VolumeGuid, Label, Name))
        trace("new volume has file system '%ls'\n", Name);

    /* drive letter */
    L1 = FreeLetter();
    ok(L1 != 0, "no free drive letter\n");
    PathsOf(P1.VolumeGuid, Paths, ARRAYSIZE(Paths));
    ok_int(DmChangeDriveLetter(P1.VolumeGuid, Paths, L1), ERROR_SUCCESS);
    Letter[0] = L1; Letter[1] = L':'; Letter[2] = 0;
    StringCchPrintfW(Root, ARRAYSIZE(Root), L"%c:\\", L1);
    ok(HasPath(P1.VolumeGuid, Root), "the volume has no path %ls\n", Root);
    ok_int(GetDriveTypeW(Root), DRIVE_FIXED);

    /* format FAT32, quick, with a label; Windows sets the type byte */
    ok(Format(P1.VolumeGuid, L"FAT32", L"T1FAT32", TRUE), "FAT32 format failed\n");
    ok(VolumeInfo(P1.VolumeGuid, Label, Name), "no volume information after the format\n");
    ok(_wcsicmp(Name, L"FAT32") == 0, "file system %ls\n", Name);
    ok(wcscmp(Label, L"T1FAT32") == 0, "label %ls\n", Label);
    ok_int(DmFixPartitionType(g_Disk, 1 * MiB, P1.VolumeGuid), ERROR_SUCCESS);
    Layout = ReadLayout(&Error);
    ok_int(Error, ERROR_SUCCESS);
    ok(Layout->Count == 1 && Layout->Entry[0].MbrType == 0x0B, "type 0x%02x\n", Layout->Count ? Layout->Entry[0].MbrType : 0);
    HeapFree(GetProcessHeap(), 0, Layout);

    /* data goes in and comes out */
    ok(WriteReadFile(P1.VolumeGuid, "disk management writer test"), "writing and reading a file on %ls failed\n", P1.VolumeGuid);

    /* the label */
    ok_int(DmSetVolumeLabel(P1.VolumeGuid, L"RENAMED"), ERROR_SUCCESS);
    ok(VolumeInfo(P1.VolumeGuid, Label, Name) && wcscmp(Label, L"RENAMED") == 0, "label after rename: %ls\n", Label);
    ok_int(DmSetVolumeLabel(P1.VolumeGuid, L""), ERROR_SUCCESS);
    ok(VolumeInfo(P1.VolumeGuid, Label, Name) && Label[0] == 0, "label after clearing: %ls\n", Label);

    /* the program's view */
    Region = ModelRegion(&Model, 1 * MiB);
    ok(Region != NULL && Region->Kind == DM_KIND_PRIMARY, "model: no primary partition\n");
    if (Region)
    {
        ok(_wcsicmp(Region->Letter, Letter) == 0, "model letter '%ls'\n", Region->Letter);
        ok(_wcsicmp(Region->FileSystem, L"FAT32") == 0, "model file system '%ls'\n", Region->FileSystem);
    }
    DmFreeModel(Model);

    /* active */
    ok_int(DmMarkActive(g_Disk, 1 * MiB, NULL), ERROR_SUCCESS);
    Layout = ReadLayout(&Error);
    ok(Layout->Count == 1 && Layout->Entry[0].Active, "partition not active\n");
    HeapFree(GetProcessHeap(), 0, Layout);

    /* second volume, every file system on it in turn */
    Error = Create(101 * MiB, FALSE, 100, &P2);
    ok_int(Error, ERROR_SUCCESS);
    ok(P2.Offset == 101 * MiB, "second partition at %I64u\n", P2.Offset);
    ok(P2.VolumeGuid[0] != 0, "no volume for the second partition\n");
    if (P2.VolumeGuid[0])
    {
        WCHAR L2 = FreeLetter(), Root2[8];

        PathsOf(P2.VolumeGuid, Paths, ARRAYSIZE(Paths));
        ok_int(DmChangeDriveLetter(P2.VolumeGuid, Paths, L2), ERROR_SUCCESS);
        StringCchPrintfW(Root2, ARRAYSIZE(Root2), L"%c:\\", L2);
        for (i = 0; i < Fs.Count; i++)
        {
            BOOL Done = Format(P2.VolumeGuid, Fs.Name[i], L"FSTEST", TRUE);
            WCHAR Detected[32] = L"", Lbl[64] = L"";

            trace("format %ls: %s\n", Fs.Name[i], Done ? "ok" : "FAILED");
            if (!Done)
                continue;
            if (VolumeInfo(P2.VolumeGuid, Lbl, Detected))
            {
                trace("  detected '%ls', label '%ls'\n", Detected, Lbl);
                if (_wcsicmp(Fs.Name[i], L"FAT") == 0 || _wcsicmp(Fs.Name[i], L"FAT32") == 0 || _wcsicmp(Fs.Name[i], L"NTFS") == 0)
                {
                    ok(_wcsicmp(Detected, Fs.Name[i]) == 0, "%ls formatted, volume says %ls\n", Fs.Name[i], Detected);
                    ok(WriteReadFile(P2.VolumeGuid, "data on each file system"), "file on %ls (%ls) failed\n", P2.VolumeGuid, Fs.Name[i]);
                }
            }
            else
            {
                trace("  volume information failed: %lu\n", GetLastError());
            }
        }
        ok(DmFixPartitionType(g_Disk, 101 * MiB, P2.VolumeGuid) == ERROR_SUCCESS, "type fix failed\n");
    }

    /* a third primary, then the fourth volume becomes a logical drive in a new extended partition */
    Error = Create(201 * MiB, FALSE, 100, &P3);
    ok_int(Error, ERROR_SUCCESS);
    Error = DmPlanPartition(g_Disk, 301 * MiB, FALSE, &Free, &NeedsExtended, Plan);
    ok_int(Error, ERROR_SUCCESS);
    ok(NeedsExtended, "fourth volume does not ask for an extended partition\n");
    /* diskpart: the extended partition takes the whole free space, 3794 MB at 301 MB here, and the drive 200 MB at 302 MB */
    Error = Create(301 * MiB, FALSE, 200, &P5);
    ok_int(Error, ERROR_SUCCESS);
    ok(P5.MadeExtended && P5.Logical, "extended %d logical %d\n", P5.MadeExtended, P5.Logical);
    ok(P5.Offset == 302 * MiB && P5.Length == 200 * MiB, "logical at %I64u length %I64u\n", P5.Offset, P5.Length);
    ok(P5.VolumeGuid[0] != 0, "no volume for the logical drive\n");
    Layout = ReadLayout(&Error);
    ok_int(Error, ERROR_SUCCESS);
    ok_int(Layout->Count, 5);
    {
        PDL_ENTRY Ext = DlFindExtended(Layout);

        ok(Ext != NULL, "no extended partition\n");
        ok(Ext && Ext->Offset == 301 * MiB && Ext->Length == 3794 * MiB, "extended at %I64u length %I64u\n",
           Ext ? Ext->Offset / MiB : 0, Ext ? Ext->Length / MiB : 0);
    }
    HeapFree(GetProcessHeap(), 0, Layout);
    if (P5.VolumeGuid[0])
    {
        WCHAR L5 = FreeLetter(), Root5[8];

        PathsOf(P5.VolumeGuid, Paths, ARRAYSIZE(Paths));
        ok_int(DmChangeDriveLetter(P5.VolumeGuid, Paths, L5), ERROR_SUCCESS);
        StringCchPrintfW(Root5, ARRAYSIZE(Root5), L"%c:\\", L5);
        ok(Format(P5.VolumeGuid, L"FAT32", L"LOGICAL", TRUE), "format of the logical drive failed\n");
        ok(WriteReadFile(P5.VolumeGuid, "logical drive"), "file on the logical drive failed\n");
        ok_int(DmFixPartitionType(g_Disk, P5.Offset, P5.VolumeGuid), ERROR_SUCCESS);
    }

    /* a second logical drive in the free space of the extended partition: 100 MB at 503 MB */
    Error = DmPlanPartition(g_Disk, 502 * MiB, TRUE, &Free, &NeedsExtended, Plan);
    ok_int(Error, ERROR_SUCCESS);
    ok(!NeedsExtended && Free.InExtended, "free space in the extended partition not found\n");
    ok(Free.DataOffset == 503 * MiB && Free.MaxLength == (4095 - 503) * MiB, "free in extended: %I64u %I64u\n",
       Free.DataOffset / MiB, Free.MaxLength / MiB);
    Error = Create(502 * MiB, TRUE, 100, &P6);
    ok_int(Error, ERROR_SUCCESS);
    ok(P6.Logical && !P6.MadeExtended && P6.Offset == 503 * MiB, "second logical drive at %I64u\n", P6.Offset / MiB);
    Layout = ReadLayout(&Error);
    ok_int(Layout->Count, 6);
    HeapFree(GetProcessHeap(), 0, Layout);

    if (g_Keep)
    {
        HeapFree(GetProcessHeap(), 0, Plan);
        return;
    }

    /* a delete that is not about the partition the user confirmed changes nothing */
    PathsOf(P1.VolumeGuid, Paths, ARRAYSIZE(Paths));
    ok_int(DmDeletePartition(g_Disk, P1.Offset, P1.Length + MiB, P1.VolumeGuid, FALSE, NULL), DM_ERR_CHANGED);
    ok_int(DmDeletePartition(g_Disk, P1.Offset, P1.Length, L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\", FALSE, NULL),
           DM_ERR_CHANGED);
    ok_int(DmDeletePartition(g_Disk, P1.Offset, P1.Length, NULL, FALSE, NULL), DM_ERR_CHANGED);
    ok_int(CountEntries(NULL), 6);
    ok_int(DmDeletePartition(g_Disk, 777 * MiB, P1.Length, NULL, FALSE, NULL), DM_ERR_LAYOUT_BASE + DL_E_NOT_FOUND);
    ok_int(CountEntries(NULL), 6);

    /* delete everything again: logical drives, extended, primaries; the paths go with them */
    if (P6.VolumeGuid[0])
    {
        PathsOf(P6.VolumeGuid, Paths, ARRAYSIZE(Paths));
        ok_int(Del(P6.Offset, P6.VolumeGuid, FALSE), ERROR_SUCCESS);
    }
    ok_int(Del(301 * MiB, NULL, FALSE), DM_ERR_LAYOUT_BASE + DL_E_EXTENDED_IN_USE);
    if (P5.VolumeGuid[0])
    {
        PathsOf(P5.VolumeGuid, Paths, ARRAYSIZE(Paths));
        ok_int(Del(P5.Offset, P5.VolumeGuid, FALSE), ERROR_SUCCESS);
    }
    ok_int(Del(301 * MiB, NULL, FALSE), ERROR_SUCCESS);
    ok_int(CountEntries(NULL), 3);
    PathsOf(P1.VolumeGuid, Paths, ARRAYSIZE(Paths));
    ok_int(Del(1 * MiB, P1.VolumeGuid, FALSE), ERROR_SUCCESS);
    ok(GetDriveTypeW(Root) == DRIVE_NO_ROOT_DIR, "drive type of the deleted volume's letter: %u\n", GetDriveTypeW(Root));
    PathsOf(P2.VolumeGuid, Paths, ARRAYSIZE(Paths));
    ok_int(Del(101 * MiB, P2.VolumeGuid, FALSE), ERROR_SUCCESS);
    PathsOf(P3.VolumeGuid, Paths, ARRAYSIZE(Paths));
    ok_int(Del(201 * MiB, P3.VolumeGuid, FALSE), ERROR_SUCCESS);
    ok_int(CountEntries(NULL), 0);
    HeapFree(GetProcessHeap(), 0, Plan);
}

static BOOL
VolumeTotal(PCWSTR Guid, PULONGLONG Bytes)
{
    ULARGE_INTEGER Avail, Total, Free;

    if (!GetDiskFreeSpaceExW(Guid, &Avail, &Total, &Free))
        return FALSE;
    *Bytes = Total.QuadPart;
    return TRUE;
}

static BOOL
KeepFile(PCWSTR Root, BOOL Write)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    DWORD Done = 0;
    CHAR Back[32] = "";
    BOOL Ok = FALSE;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%skeep.txt", Root);
    File = CreateFileW(Path, Write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, NULL, Write ? CREATE_ALWAYS : OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        trace("open %ls: %lu\n", Path, GetLastError());
        return FALSE;
    }
    if (Write)
        Ok = WriteFile(File, "resize me", 9, &Done, NULL) && Done == 9;
    else
        Ok = ReadFile(File, Back, sizeof(Back) - 1, &Done, NULL) && Done == 9 && strcmp(Back, "resize me") == 0;
    if (!Ok)
        trace("file: done %lu '%s' err %lu\n", Done, Back, GetLastError());
    CloseHandle(File);
    return Ok;
}

/* Growing and shrinking go through requests that a driver may not have yet; the test skips then. */
static VOID
Test_Resize(VOID)
{
    DM_CREATED P1;
    DM_DISK_ID Id, Other;
    ULONGLONG Max = 0, Total = 0, Before = 0, After = 0;
    PDL_LAYOUT Layout;
    PDL_ENTRY Entry;
    DWORD Error;

    ok_int(Create(1 * MiB, FALSE, 200, &P1), ERROR_SUCCESS);
    if (!P1.VolumeGuid[0])
        return;
    ok(Format(P1.VolumeGuid, L"NTFS", L"RSZ", TRUE), "NTFS format failed\n");
    /* another disk behind the number: neither the file system nor the table is touched, whatever the driver can do */
    GetId(&Id);
    Other = Id;
    Other.Signature ^= 0x5A5A5A5A;
    ok_int(DmExtendVolume(g_Disk, P1.Offset, P1.Length, P1.VolumeGuid, 100 * MiB, &Other), DM_ERR_CHANGED);
    ok_int(DmShrinkVolume(g_Disk, P1.Offset, P1.Length, P1.VolumeGuid, 100 * MiB, &Other), DM_ERR_CHANGED);
    if (DmProbeExtend(g_Disk, P1.VolumeGuid) == DM_ERR_NOT_SUPPORTED || DmProbeShrink(P1.VolumeGuid) == DM_ERR_NOT_SUPPORTED)
    {
        skip("the system cannot grow partitions, or extend or shrink NTFS volumes, yet\n");
        Del(P1.Offset, P1.VolumeGuid, FALSE);
        return;
    }
    ok(KeepFile(P1.VolumeGuid, TRUE), "no file on the new volume\n");
    ok(VolumeTotal(P1.VolumeGuid, &Before), "no size of the new volume\n");

    /* the room behind the volume is what the wizard offers, 1 MiB less than the free gap */
    ok_int(DmQueryExtend(g_Disk, P1.Offset, &Max), ERROR_SUCCESS);
    ok(Max == 3893 * MiB, "room to grow %I64u MiB\n", Max / MiB);
    ok_int(DmExtendVolume(g_Disk, P1.Offset, P1.Length, P1.VolumeGuid, 100 * MiB, &Id), ERROR_SUCCESS);
    Layout = ReadLayout(&Error);
    Entry = Error == ERROR_SUCCESS ? DlFindEntry(Layout, P1.Offset) : NULL;
    ok(Entry && Entry->Length == 300 * MiB, "partition after growing: %I64u MiB\n", Entry ? Entry->Length / MiB : 0);
    HeapFree(GetProcessHeap(), 0, Layout);
    ok(VolumeTotal(P1.VolumeGuid, &After) && After > Before && After >= 290 * MiB, "volume after growing: %I64u MiB\n", After / MiB);
    ok(KeepFile(P1.VolumeGuid, FALSE), "the file is gone after growing\n");

    ok_int(DmQueryShrink(g_Disk, P1.Offset, P1.VolumeGuid, &Total, &Max), ERROR_SUCCESS);
    ok(Total == 300 * MiB && Max >= 100 * MiB, "shrink query: total %I64u MiB, can give %I64u MiB\n", Total / MiB, Max / MiB);
    ok_int(DmShrinkVolume(g_Disk, P1.Offset, 300 * MiB, P1.VolumeGuid, 100 * MiB, &Id), ERROR_SUCCESS);
    /* the table is written again to cut the partition, and the volume manager may make the volume anew */
    ok_int(DmFindVolume(g_Disk, P1.Offset, 15000, P1.VolumeGuid, ARRAYSIZE(P1.VolumeGuid)), ERROR_SUCCESS);
    Layout = ReadLayout(&Error);
    Entry = Error == ERROR_SUCCESS ? DlFindEntry(Layout, P1.Offset) : NULL;
    ok(Entry && Entry->Length == 200 * MiB, "partition after shrinking: %I64u MiB\n", Entry ? Entry->Length / MiB : 0);
    HeapFree(GetProcessHeap(), 0, Layout);
    ok(VolumeTotal(P1.VolumeGuid, &After) && After <= 200 * MiB && After >= 190 * MiB, "volume after shrinking: %I64u MiB\n", After / MiB);
    /* the volume is made anew after the table was written, and may take a moment to take requests */
    {
        ULONG Try;
        BOOL Read = FALSE;

        for (Try = 0; Try < 10 && !Read; Try++)
        {
            Read = KeepFile(P1.VolumeGuid, FALSE);
            if (!Read)
                Sleep(1000);
        }
        ok(Read, "the file is gone after shrinking\n");
    }
    ok_int(Del(P1.Offset, P1.VolumeGuid, FALSE), ERROR_SUCCESS);
}

static VOID
Test_Gpt(VOID)
{
    DM_CREATED P1, P2;
    DL_LAYOUT *Plan = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    DL_FREE Free;
    BOOL NeedsExtended;
    DWORD Error;
    DM_STYLE Style;
    PDL_LAYOUT Layout;
    WCHAR Label[64], Name[32], Paths[512], Root[8];
    WCHAR L1;

    /* convert the empty disk to GPT, or initialize it when the run has not made it MBR first */
    if (g_Keep)
        ok_int(DmInitializeDisk(g_Disk, DM_STYLE_GPT, NULL), ERROR_SUCCESS);
    else
        ok_int(DmConvertDisk(g_Disk, DM_STYLE_GPT, NULL), ERROR_SUCCESS);
    ok_int(CountEntries(&Style), 0);
    ok_int(Style, DM_STYLE_GPT);
    /* a second conversion to the same style is refused */
    ok(DmConvertDisk(g_Disk, DM_STYLE_GPT, NULL) != ERROR_SUCCESS, "converted to the style it has\n");

    Error = DmPlanPartition(g_Disk, 1 * MiB, FALSE, &Free, &NeedsExtended, Plan);
    ok_int(Error, ERROR_SUCCESS);
    ok(!NeedsExtended, "GPT asks for an extended partition\n");
    trace("GPT free: data offset %I64u MB, max %I64u MB\n", Free.DataOffset / MiB, Free.MaxLength / MiB);
    Error = Create(1 * MiB, FALSE, 200, &P1);
    ok_int(Error, ERROR_SUCCESS);
    ok(P1.MadeReserved, "no reserved partition\n");
    ok(P1.VolumeGuid[0] != 0, "no volume for the GPT partition\n");
    if (P1.VolumeGuid[0] == 0)
    {
        HeapFree(GetProcessHeap(), 0, Plan);
        return;
    }
    Layout = ReadLayout(&Error);
    ok_int(Error, ERROR_SUCCESS);
    ok_int(Layout->Count, 2);
    ok(Layout->Count == 2 && Layout->Entry[0].Offset == 17408 && memcmp(&Layout->Entry[0].GptType, &DlGuidReserved, sizeof(GUID)) == 0,
       "reserved partition missing\n");
    HeapFree(GetProcessHeap(), 0, Layout);

    /* the reserved partition has no drive letter, even where the system makes a volume of it */
    {
        WCHAR ReservedGuid[MAX_PATH];

        if (DmFindVolume(g_Disk, 17408, 0, ReservedGuid, ARRAYSIZE(ReservedGuid)) == ERROR_SUCCESS)
        {
            PathsOf(ReservedGuid, Paths, ARRAYSIZE(Paths));
            ok(Paths[0] == 0, "the reserved partition has a path: %ls\n", Paths);
        }
    }

    /* a disk with partitions cannot be converted */
    ok_int(DmConvertDisk(g_Disk, DM_STYLE_MBR, NULL), DM_ERR_NOT_EMPTY);

    if (P1.VolumeGuid[0])
    {
        L1 = FreeLetter();
        PathsOf(P1.VolumeGuid, Paths, ARRAYSIZE(Paths));
        ok_int(DmChangeDriveLetter(P1.VolumeGuid, Paths, L1), ERROR_SUCCESS);
        StringCchPrintfW(Root, ARRAYSIZE(Root), L"%c:\\", L1);
        ok(Format(P1.VolumeGuid, L"NTFS", L"GPTNTFS", TRUE), "NTFS format failed on GPT\n");
        ok(VolumeInfo(P1.VolumeGuid, Label, Name) && _wcsicmp(Name, L"NTFS") == 0, "file system %ls\n", Name);
        ok(WriteReadFile(P1.VolumeGuid, "gpt volume"), "file on the GPT volume failed\n");
    }
    Error = Create(P1.Offset + P1.Length, FALSE, 100, &P2);
    ok_int(Error, ERROR_SUCCESS);

    /* what a plan was made on includes the attributes and the name of a GPT partition: a change of them behind its back is
     * not written over with the old ones */
    {
        PDL_LAYOUT Base = ReadLayout(&Error), Changed = HeapAlloc(GetProcessHeap(), 0, sizeof(DL_LAYOUT)), Back;
        PDL_ENTRY Entry;
        ULONG Kind;

        ok_int(Error, ERROR_SUCCESS);
        if (Error == ERROR_SUCCESS && Changed && (Entry = DlFindEntry(Base, P1.Offset)) != NULL)
        {
            for (Kind = 0; Kind < 2; Kind++)
            {
                PDL_ENTRY New;

                memcpy(Changed, Base, sizeof(DL_LAYOUT));
                New = DlFindEntry(Changed, P1.Offset);
                if (Kind == 0)
                    New->GptAttributes ^= 4;    /* legacy BIOS bootable: harmless here */
                else
                    StringCchCopyW(New->GptName, ARRAYSIZE(New->GptName), L"Renamed behind the plan");
                ok_int(DmWriteDiskLayout(g_Disk, Base, Changed), ERROR_SUCCESS);
                ok_int(DmWriteDiskLayout(g_Disk, Base, Base), DM_ERR_CHANGED);
                Back = ReadLayout(&Error);
                New = Error == ERROR_SUCCESS ? DlFindEntry(Back, P1.Offset) : NULL;
                ok(New && ((Kind == 0 && New->GptAttributes == (Entry->GptAttributes ^ 4)) ||
                           (Kind == 1 && wcsncmp(New->GptName, L"Renamed behind the plan", 23) == 0)),
                   "the change of %s was written over\n", Kind == 0 ? "the attributes" : "the name");
                HeapFree(GetProcessHeap(), 0, Back);
                /* put the partition as it was, from what the disk holds now */
                Back = ReadLayout(&Error);
                if (Error == ERROR_SUCCESS)
                    ok_int(DmWriteDiskLayout(g_Disk, Back, Base), ERROR_SUCCESS);
                HeapFree(GetProcessHeap(), 0, Back);
            }
            /* the rewrites took the volumes away and brought them back */
            ok_int(DmFindVolume(g_Disk, P1.Offset, 15000, P1.VolumeGuid, ARRAYSIZE(P1.VolumeGuid)), ERROR_SUCCESS);
            ok_int(DmFindVolume(g_Disk, P2.Offset, 15000, P2.VolumeGuid, ARRAYSIZE(P2.VolumeGuid)), ERROR_SUCCESS);
        }
        HeapFree(GetProcessHeap(), 0, Changed);
        HeapFree(GetProcessHeap(), 0, Base);
    }

    if (g_Keep)
    {
        HeapFree(GetProcessHeap(), 0, Plan);
        return;
    }

    /* delete them; the hidden reserved partition stays, and the disk may be converted back */
    if (P2.VolumeGuid[0])
        ok_int(Del(P2.Offset, P2.VolumeGuid, FALSE), ERROR_SUCCESS);
    PathsOf(P1.VolumeGuid, Paths, ARRAYSIZE(Paths));
    ok_int(Del(P1.Offset, P1.VolumeGuid, FALSE), ERROR_SUCCESS);
    ok_int(CountEntries(NULL), 1);
    ok_int(DmConvertDisk(g_Disk, DM_STYLE_MBR, NULL), ERROR_SUCCESS);
    ok_int(CountEntries(&Style), 0);
    ok_int(Style, DM_STYLE_MBR);
    HeapFree(GetProcessHeap(), 0, Plan);
}

static BOOL
DiskIsScratch(VOID)
{
    WCHAR Windows[MAX_PATH], Device[8];
    HANDLE Handle;
    STORAGE_DEVICE_NUMBER Number;
    DWORD Returned;

    /* only a disk that answers as empty is scratch; an unreadable table is no proof */
    if (DmCheckScratchDisk(g_Disk) != ERROR_SUCCESS)
        return FALSE;
    if (GetWindowsDirectoryW(Windows, ARRAYSIZE(Windows)) >= 2 && Windows[1] == L':')
    {
        StringCchPrintfW(Device, ARRAYSIZE(Device), L"\\\\.\\%c:", Windows[0]);
        Handle = CreateFileW(Device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (Handle != INVALID_HANDLE_VALUE)
        {
            BOOL OnSystemDisk = DeviceIoControl(Handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &Number, sizeof(Number),
                                                &Returned, NULL) && Number.DeviceNumber == g_Disk;

            CloseHandle(Handle);
            if (OnSystemDisk)
                return FALSE;
        }
    }
    return TRUE;
}

/* a plain decimal number that fits the variable; anything else selects no disk */
static BOOL
ParseDisk(PCWSTR Text, ULONG *Disk)
{
    ULONGLONG Value = 0;
    PCWSTR p;

    if (*Text == 0)
        return FALSE;
    for (p = Text; *p; p++)
    {
        if (*p < L'0' || *p > L'9')
            return FALSE;
        Value = Value * 10 + (*p - L'0');
        if (Value > 1024)
            return FALSE;
    }
    *Disk = (ULONG)Value;
    return TRUE;
}

/* the expectations of the test are the numbers of a blank 4 GiB disk with 512-byte sectors, as measured on Windows */
static BOOL
DiskHasTestGeometry(VOID)
{
    WCHAR Path[64];
    HANDLE Device;
    union { DISK_GEOMETRY_EX Geo; BYTE Raw[1024]; } Geometry;
    DWORD Returned;
    BOOL Ok = FALSE;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"\\\\.\\PhysicalDrive%lu", g_Disk);
    Device = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;
    ZeroMemory(&Geometry, sizeof(Geometry));
    if (DeviceIoControl(Device, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &Geometry, sizeof(Geometry), &Returned, NULL))
        Ok = Geometry.Geo.DiskSize.QuadPart == 4096 * MiB && Geometry.Geo.Geometry.BytesPerSector == 512;
    CloseHandle(Device);
    return Ok;
}

/* read only: of all the partitions of all the disks, exactly one is the system partition the kernel recorded */
static VOID
Test_BootPartition(VOID)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    ULONG Disk, i, Found = 0, Readable = 0;

    for (Disk = 0; Disk < 16; Disk++)
    {
        if (DmReadDiskLayout(Disk, Layout) != ERROR_SUCCESS)
            continue;
        Readable++;
        for (i = 0; i < Layout->Count; i++)
            if (DmIsBootPartition(Disk, Layout->Entry[i].Offset))
                Found++;
        ok(!DmIsBootPartition(Disk, 1), "a start where no partition is, is the boot partition\n");
    }
    if (Readable)
        ok(Found == 1, "%lu partitions say they hold the boot files\n", Found);
    HeapFree(GetProcessHeap(), 0, Layout);
}

START_TEST(DiskOps)
{
    WCHAR Value[16];

    Test_BootPartition();

    DWORD Got = GetEnvironmentVariableW(L"DISKMGMT_TEST_DISK", Value, ARRAYSIZE(Value));

    if (Got == 0)
    {
        skip("DISKMGMT_TEST_DISK is not set: the live test of the writer changes a disk and is not run\n");
        return;
    }
    if (Got >= ARRAYSIZE(Value) || !ParseDisk(Value, &g_Disk))
    {
        skip("DISKMGMT_TEST_DISK is not a disk number\n");
        return;
    }
    if (!DiskHasTestGeometry())
    {
        skip("disk %lu is not a blank 4 GiB disk with 512-byte sectors, the numbers of the test are those of such a disk\n", g_Disk);
        return;
    }
    if (!DiskIsScratch())
    {
        skip("disk %lu holds partitions or the system, the live test needs an empty scratch disk\n", g_Disk);
        return;
    }
    if (GetEnvironmentVariableW(L"DISKMGMT_TEST_DISK2", Value, ARRAYSIZE(Value)) != 0 && ParseDisk(Value, &g_Disk2) &&
        g_Disk2 != g_Disk)
    {
        ULONG First = g_Disk;

        /* the second disk must be as empty as the first */
        g_Disk = g_Disk2;
        g_HaveDisk2 = DiskIsScratch();
        g_Disk = First;
        if (!g_HaveDisk2)
            skip("disk %lu is not an empty scratch disk, the swapped-disk tests are not run\n", g_Disk2);
    }
    g_Keep = GetEnvironmentVariableW(L"DISKMGMT_TEST_KEEP", Value, ARRAYSIZE(Value)) != 0;
    if (GetEnvironmentVariableW(L"DISKMGMT_TEST_STYLE", Value, ARRAYSIZE(Value)) != 0 && _wcsicmp(Value, L"GPT") == 0)
    {
        Test_Gpt();
        return;
    }
    Test_Mbr();
    if (!g_Keep)
    {
        Test_Resize();
        Test_Gpt();
    }
}
