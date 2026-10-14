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
    return DmDeletePartition(g_Disk, Offset, Length, Guid, Force);
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
    return DmCreatePartition(g_Disk, Offset, InExtended, LengthMiB * MiB, Created);
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

    ok_int(DmInitializeDisk(g_Disk, DM_STYLE_MBR), ERROR_SUCCESS);
    ok_int(CountEntries(&Style), 0);
    ok_int(Style, DM_STYLE_MBR);

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
    ok_int(DmMarkActive(g_Disk, 1 * MiB), ERROR_SUCCESS);
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
    ok_int(DmDeletePartition(g_Disk, P1.Offset, P1.Length + MiB, P1.VolumeGuid, FALSE), DM_ERR_CHANGED);
    ok_int(DmDeletePartition(g_Disk, P1.Offset, P1.Length, L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\", FALSE),
           DM_ERR_CHANGED);
    ok_int(DmDeletePartition(g_Disk, P1.Offset, P1.Length, NULL, FALSE), DM_ERR_CHANGED);
    ok_int(CountEntries(NULL), 6);
    ok_int(DmDeletePartition(g_Disk, 777 * MiB, P1.Length, NULL, FALSE), DM_ERR_LAYOUT_BASE + DL_E_NOT_FOUND);
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
    ULONGLONG Max = 0, Total = 0, Before = 0, After = 0;
    PDL_LAYOUT Layout;
    PDL_ENTRY Entry;
    DWORD Error;

    ok_int(Create(1 * MiB, FALSE, 200, &P1), ERROR_SUCCESS);
    if (!P1.VolumeGuid[0])
        return;
    ok(Format(P1.VolumeGuid, L"NTFS", L"RSZ", TRUE), "NTFS format failed\n");
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
    ok_int(DmExtendVolume(g_Disk, P1.Offset, P1.Length, P1.VolumeGuid, 100 * MiB), ERROR_SUCCESS);
    Layout = ReadLayout(&Error);
    Entry = Error == ERROR_SUCCESS ? DlFindEntry(Layout, P1.Offset) : NULL;
    ok(Entry && Entry->Length == 300 * MiB, "partition after growing: %I64u MiB\n", Entry ? Entry->Length / MiB : 0);
    HeapFree(GetProcessHeap(), 0, Layout);
    ok(VolumeTotal(P1.VolumeGuid, &After) && After > Before && After >= 290 * MiB, "volume after growing: %I64u MiB\n", After / MiB);
    ok(KeepFile(P1.VolumeGuid, FALSE), "the file is gone after growing\n");

    ok_int(DmQueryShrink(g_Disk, P1.Offset, P1.VolumeGuid, &Total, &Max), ERROR_SUCCESS);
    ok(Total == 300 * MiB && Max >= 100 * MiB, "shrink query: total %I64u MiB, can give %I64u MiB\n", Total / MiB, Max / MiB);
    ok_int(DmShrinkVolume(g_Disk, P1.Offset, 300 * MiB, P1.VolumeGuid, 100 * MiB), ERROR_SUCCESS);
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
        ok_int(DmInitializeDisk(g_Disk, DM_STYLE_GPT), ERROR_SUCCESS);
    else
        ok_int(DmConvertDisk(g_Disk, DM_STYLE_GPT), ERROR_SUCCESS);
    ok_int(CountEntries(&Style), 0);
    ok_int(Style, DM_STYLE_GPT);
    /* a second conversion to the same style is refused */
    ok(DmConvertDisk(g_Disk, DM_STYLE_GPT) != ERROR_SUCCESS, "converted to the style it has\n");

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
    ok_int(DmConvertDisk(g_Disk, DM_STYLE_MBR), DM_ERR_NOT_EMPTY);

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
    ok_int(DmConvertDisk(g_Disk, DM_STYLE_MBR), ERROR_SUCCESS);
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
