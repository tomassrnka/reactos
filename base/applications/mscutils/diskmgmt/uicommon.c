/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared helpers of the dialogs and wizards
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "uicommon.h"

/* ------------------------------------------------------------------- the model */

BOOL
DiskWritable(const DM_DISK *Disk)
{
    return Disk->Device != DM_DEV_CDROM && Disk->HasMedia && !Disk->Dynamic &&
           (wcscmp(Disk->StatusText, L"Online") == 0 || wcscmp(Disk->StatusText, L"Not Initialized") == 0);
}

BOOL
RegionIsPartition(const DM_REGION *Region)
{
    return Region && (Region->Kind == DM_KIND_PRIMARY || Region->Kind == DM_KIND_LOGICAL);
}

BOOL
RegionProtected(const DM_REGION *Region)
{
    return Region->IsBoot || Region->IsSystem || Region->IsPageFile || Region->IsCrashDump;
}

BOOL
DiskHasPartitions(const DM_DISK *Disk)
{
    ULONG i;

    if (Disk->HasExtended)
        return TRUE;
    for (i = 0; i < Disk->RegionCount; i++)
        if (Disk->Regions[i].Kind == DM_KIND_PRIMARY || Disk->Regions[i].Kind == DM_KIND_LOGICAL)
            return TRUE;
    return FALSE;
}

/* Windows shows a disk without a signature as not initialized; ReactOS reads such a disk as an empty MBR disk. */
BOOL
DiskNeedsInit(const DM_DISK *Disk)
{
    if (Disk->Device == DM_DEV_CDROM || !Disk->HasMedia || Disk->Dynamic)
        return FALSE;
    if (Disk->Style == DM_STYLE_RAW)
        return wcscmp(Disk->StatusText, L"Not Initialized") == 0;
    return Disk->Style == DM_STYLE_MBR && Disk->Signature == 0 && !DiskHasPartitions(Disk);
}

VOID
FillTarget(PTARGET Target, const DM_DISK *Disk, const DM_REGION *Region)
{
    ZeroMemory(Target, sizeof(*Target));
    Target->Disk = Disk->Number;
    Target->Device = Disk->Device;
    Target->Style = Disk->Style;
    Target->Removable = (Disk->Device == DM_DEV_REMOVABLE);
    if (Region == NULL)
        return;
    Target->Offset = Region->Offset;
    Target->Length = Region->Length;
    Target->InExtended = Region->InExtended;
    Target->Protected = RegionProtected(Region);
    Target->IsEfi = Region->IsEfi;
    if (Region->HasVolume)
    {
        StringCchCopyW(Target->Guid, ARRAYSIZE(Target->Guid), Region->VolumeGuid);
        StringCchCopyW(Target->Letter, ARRAYSIZE(Target->Letter), Region->Letter);
        memcpy(Target->Paths, Region->MountPaths, sizeof(Target->Paths));
        StringCchCopyW(Target->Label, ARRAYSIZE(Target->Label), Region->Label);
        StringCchCopyW(Target->FileSystem, ARRAYSIZE(Target->FileSystem), Region->FileSystem);
    }
}

/* "J: (New Volume)" as Windows words it in the titles of its dialogs */
PCWSTR
VolumeDisplayName(const TARGET *Target, PWSTR Buffer, SIZE_T Count)
{
    if (Target->Label[0] && Target->Letter[0])
        StringCchPrintfW(Buffer, Count, L"%s (%s)", Target->Letter, Target->Label);
    else if (Target->Letter[0])
        StringCchCopyW(Buffer, Count, Target->Letter);
    else if (Target->Label[0])
        StringCchCopyW(Buffer, Count, Target->Label);
    else
        StringCchPrintfW(Buffer, Count, L"Disk %u", Target->Disk);
    return Buffer;
}

/* -------------------------------------------------------------------- messages */

static PCWSTR
SystemMessage(DWORD Error, PWSTR Buffer, DWORD Count)
{
    DWORD Len = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error, 0, Buffer, Count, NULL);

    while (Len > 0 && (Buffer[Len - 1] == L'\r' || Buffer[Len - 1] == L'\n' || Buffer[Len - 1] == L' '))
        Buffer[--Len] = 0;
    if (Len == 0)
        StringCchPrintfW(Buffer, Count, L"Error %lu", Error);
    return Buffer;
}

VOID
ShowError(HWND hOwner, DWORD Error, PCWSTR Extra)
{
    WCHAR Title[64], Format[256], Detail[256], Text[640];
    UINT Id = 0;

    LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
    switch (Error)
    {
        case DM_ERR_NO_PROVIDER:    Id = IDS_ERR_NO_PROVIDER; break;
        case DM_ERR_VOLUME_IN_USE:  Id = IDS_ERR_IN_USE; break;
        case DM_ERR_NOT_EMPTY:      Id = IDS_ERR_NOT_EMPTY; break;
        case DM_ERR_NO_VOLUME:      Id = IDS_ERR_NO_VOLUME; break;
        case DM_ERR_NOT_SUPPORTED:  Id = IDS_ERR_NOT_SUPPORTED; break;
        case DM_ERR_PROTECTED:      Id = IDS_ERR_PROTECTED; break;
        case DM_ERR_CHANGED:        Id = IDS_ERR_CHANGED; break;
        case DM_ERR_UNREADABLE:     Id = IDS_ERR_UNREADABLE; break;
        case DM_ERR_TYPE_NOT_UPDATED: Id = IDS_ERR_TYPE_NOT_UPDATED; break;
        case DM_ERR_VERIFY_FAILED:  Id = IDS_ERR_VERIFY_FAILED; break;
        case DM_ERR_RESTORE_FAILED: Id = IDS_ERR_RESTORE_FAILED; break;
        case DM_ERR_EXTEND_PARTIAL: Id = IDS_ERR_EXTEND_PARTIAL; break;
        case DM_ERR_SHRINK_PARTIAL: Id = IDS_ERR_SHRINK_PARTIAL; break;
        case DM_ERR_FORMAT_FAILED:  break;
        default:
            if (Error >= DM_ERR_LAYOUT_BASE && Error < DM_ERR_LAYOUT_BASE + 0x100)
                Id = IDS_ERR_LAYOUT;
    }
    if (Error == DM_ERR_FORMAT_FAILED)
    {
        WCHAR Tail[200];

        LoadStringBuffer(IDS_ERR_FORMAT, Format, ARRAYSIZE(Format));
        if (Extra && Extra[0])
        {
            WCHAR Part[32];

            LoadStringBuffer(IDS_ERR_FORMAT_CODE, Part, ARRAYSIZE(Part));
            StringCchPrintfW(Tail, ARRAYSIZE(Tail), Part, Extra);
        }
        else
        {
            Tail[0] = 0;
        }
        StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Tail);
    }
    else
    {
        LoadStringBuffer(IDS_ERR_GENERIC, Format, ARRAYSIZE(Format));
        if (Id)
            LoadStringBuffer(Id, Detail, ARRAYSIZE(Detail));
        else
            SystemMessage(Error, Detail, ARRAYSIZE(Detail));
        StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Detail);
    }
    MessageBoxW(hOwner, Text, Title, MB_OK | MB_ICONERROR);
}

/* a question with a warning icon; the answer is no unless the user says yes */
BOOL
ConfirmBox(HWND hOwner, UINT CaptionId, UINT TextId, PCWSTR Argument)
{
    WCHAR Title[96], Format[400], Text[512];

    LoadStringBuffer(CaptionId, Title, ARRAYSIZE(Title));
    LoadStringBuffer(TextId, Format, ARRAYSIZE(Format));
    if (Argument)
        StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Argument);
    else
        StringCchCopyW(Text, ARRAYSIZE(Text), Format);
    return MessageBoxW(hOwner, Text, Title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

VOID
FormatMessageBox(HWND hOwner, UINT TextId, UINT Icon, PCWSTR Argument)
{
    WCHAR Title[64], Format[400], Text[512];

    LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
    LoadStringBuffer(TextId, Format, ARRAYSIZE(Format));
    if (Argument)
        StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Argument);
    else
        StringCchCopyW(Text, ARRAYSIZE(Text), Format);
    MessageBoxW(hOwner, Text, Title, MB_OK | Icon);
}

/* ------------------------------------------------------------ busy and progress */

typedef struct _WORK
{
    DWORD (*Func)(PVOID);
    PVOID Arg;
    DWORD Result;
    HWND hDlg;
    HANDLE hThread;
    BOOL Bar;
    WCHAR Text[160];
} WORK, *PWORK;

static PWORK g_Work;

static DWORD WINAPI
WorkThread(PVOID Param)
{
    PWORK Work = Param;

    Work->Result = Work->Func(Work->Arg);
    PostMessageW(Work->hDlg, WM_DM_DONE, 0, 0);
    return 0;
}

static INT_PTR CALLBACK
ProgressProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PWORK Work = (PWORK)lParam;
            HANDLE hThread;

            Work->hDlg = hDlg;
            SetDlgItemTextW(hDlg, IDC_PR_TEXT, Work->Text);
            SendDlgItemMessageW(hDlg, IDC_PR_BAR, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            hThread = CreateThread(NULL, 0, WorkThread, Work, 0, NULL);
            if (hThread == NULL)
            {
                Work->Result = GetLastError();
                EndDialog(hDlg, 0);
            }
            else
            {
                /* RunBusy waits for the worker before it lets go of the job */
                Work->hThread = hThread;
            }
            return TRUE;
        }
        case WM_DM_PROGRESS:
            SendDlgItemMessageW(hDlg, IDC_PR_BAR, PBM_SETPOS, wParam, 0);
            return TRUE;
        case WM_DM_DONE:
            EndDialog(hDlg, 1);
            return TRUE;
        case WM_CLOSE:
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL)
                return TRUE;
            break;
    }
    return FALSE;
}

static VOID
ReportProgress(ULONG Percent)
{
    if (g_Work && g_Work->hDlg)
        PostMessageW(g_Work->hDlg, WM_DM_PROGRESS, Percent > 100 ? 100 : Percent, 0);
}

/* runs Func on a thread behind a dialog the user cannot close; returns what Func returned */
DWORD
RunBusy(HWND hOwner, PCWSTR Text, BOOL Bar, DWORD (*Func)(PVOID), PVOID Arg, VOID (**Progress)(ULONG))
{
    static WORK Work;

    ZeroMemory(&Work, sizeof(Work));
    Work.Func = Func;
    Work.Arg = Arg;
    Work.Bar = Bar;
    if (Text)
        StringCchCopyW(Work.Text, ARRAYSIZE(Work.Text), Text);
    else
        LoadStringBuffer(IDS_WORKING, Work.Text, ARRAYSIZE(Work.Text));
    g_Work = &Work;
    if (Progress)
        *Progress = ReportProgress;
    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PROGRESS), hOwner, ProgressProc, (LPARAM)&Work) == -1 &&
        Work.hThread == NULL)
        Work.Result = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    if (Work.hThread)
    {
        /* the dialog can end before the worker does (owner destroyed, quit message) */
        WaitForSingleObject(Work.hThread, INFINITE);
        CloseHandle(Work.hThread);
        Work.hThread = NULL;
    }
    g_Work = NULL;
    return Work.Result;
}

/* the partition type follows the file system, as in Windows; a failure is told apart from a failed format */
VOID
TypeFix(HWND hOwner, ULONG Disk, ULONGLONG Offset, PCWSTR Guid)
{
    if (DmFixPartitionType(Disk, Offset, Guid) != ERROR_SUCCESS)
        ShowError(hOwner, DM_ERR_TYPE_NOT_UPDATED, NULL);
}

typedef struct _FORMAT_JOB
{
    DM_FORMAT Format;
} FORMAT_JOB, *PFORMAT_JOB;

static VOID
FormatProgress(PVOID Context, ULONG Percent)
{
    UNREFERENCED_PARAMETER(Context);
    ReportProgress(Percent);
}

static DWORD
FormatThread(PVOID Arg)
{
    return DmFormatVolume(&((PFORMAT_JOB)Arg)->Format);
}

DWORD
RunFormat(HWND hOwner, PCWSTR Guid, PCWSTR FileSystem, PCWSTR Label, BOOL Quick, ULONG Cluster, BOOL Removable)
{
    FORMAT_JOB Job;
    WCHAR Format[64], Text[160];
    DWORD Error;

    ZeroMemory(&Job, sizeof(Job));
    StringCchCopyW(Job.Format.Root, ARRAYSIZE(Job.Format.Root), Guid);
    StringCchCopyW(Job.Format.FileSystem, ARRAYSIZE(Job.Format.FileSystem), FileSystem);
    StringCchCopyW(Job.Format.Label, ARRAYSIZE(Job.Format.Label), Label);
    Job.Format.Quick = Quick;
    Job.Format.ClusterSize = Cluster;
    Job.Format.Removable = Removable;
    Job.Format.Progress = FormatProgress;
    LoadStringBuffer(IDS_FORMATTING, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Text, ARRAYSIZE(Text), Format, FileSystem);
    Error = RunBusy(hOwner, Text, TRUE, FormatThread, &Job, NULL);
    if (Error == DM_ERR_VOLUME_IN_USE && ConfirmBox(hOwner, IDS_CONFIRM_TITLE, IDS_CONFIRM_FORCE, NULL))
    {
        /* the user allowed the volume to be dismounted */
        Job.Format.Force = TRUE;
        Error = RunBusy(hOwner, Text, TRUE, FormatThread, &Job, NULL);
    }
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, Job.Format.Output);
    return Error;
}

/* -------------------------------------------------- letters, file systems, paths */

/* A whole number of decimal digits that fits in the range; "1,000", "12ab" or an empty text is not one. */
BOOL
ParseNumber(PCWSTR Text, ULONGLONG Min, ULONGLONG Max, PULONGLONG Value)
{
    ULONGLONG Number = 0;
    PCWSTR p;

    if (Text[0] == 0)
        return FALSE;
    for (p = Text; *p; p++)
    {
        if (*p < L'0' || *p > L'9' || Number > (~0ULL - (ULONGLONG)(*p - L'0')) / 10)
            return FALSE;
        Number = Number * 10 + (ULONGLONG)(*p - L'0');
    }
    if (Number < Min || Number > Max)
        return FALSE;
    *Value = Number;
    return TRUE;
}

/* the letters nobody uses (C to Z), the current one included */
VOID
FillLetterCombo(HWND hCombo, WCHAR Current)
{
    DWORD Used = GetLogicalDrives();
    WCHAR Letter;
    int Select = -1, First = -1, Index;

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    for (Letter = L'C'; Letter <= L'Z'; Letter++)
    {
        WCHAR Text[4];

        if ((Used & (1UL << (Letter - L'A'))) && Letter != Current)
            continue;
        Text[0] = Letter; Text[1] = 0;
        Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Text);
        SendMessageW(hCombo, CB_SETITEMDATA, Index, Letter);
        if (First < 0)
            First = Index;
        if (Current && Letter == Current)
            Select = Index;
    }
    if (SendMessageW(hCombo, CB_GETCOUNT, 0, 0) == 0)
        return;
    if (Select < 0)
        Select = First >= 0 ? First : 0;
    SendMessageW(hCombo, CB_SETCURSEL, Select, 0);
}

WCHAR
ComboLetter(HWND hDlg, UINT Id)
{
    int Index = (int)SendDlgItemMessageW(hDlg, Id, CB_GETCURSEL, 0, 0);

    if (Index < 0)
        return 0;
    return (WCHAR)SendDlgItemMessageW(hDlg, Id, CB_GETITEMDATA, Index, 0);
}

/* Windows offers NTFS, FAT and FAT32 by the size of the volume; whatever else is installed follows. */
static BOOL
FsFits(PCWSTR Fs, ULONGLONG Bytes)
{
    if (_wcsicmp(Fs, L"FAT") == 0)
        return Bytes <= 4 * DM_GIB;
    if (_wcsicmp(Fs, L"FAT32") == 0)
        return Bytes >= 33 * DM_MIB;
    return TRUE;
}

VOID
FillFsCombo(HWND hCombo, const DM_FS_LIST *Fs, PCWSTR Current, ULONGLONG VolumeBytes)
{
    static const WCHAR *Order[] = { L"NTFS", L"FAT", L"FAT32" };
    ULONG i, k;
    int Select = -1, Index;
    BOOL Done[DM_MAX_FS] = { FALSE };

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    for (k = 0; k < ARRAYSIZE(Order); k++)
    {
        for (i = 0; i < Fs->Count; i++)
        {
            if (_wcsicmp(Fs->Name[i], Order[k]) != 0 || !FsFits(Fs->Name[i], VolumeBytes))
                continue;
            Done[i] = TRUE;
            Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Fs->Name[i]);
            if (Current && Current[0] && _wcsicmp(Current, Fs->Name[i]) == 0)
                Select = Index;
        }
    }
    for (i = 0; i < Fs->Count; i++)
    {
        if (Done[i] || !FsFits(Fs->Name[i], VolumeBytes) || _wcsicmp(Fs->Name[i], L"NTFS") == 0 ||
            _wcsicmp(Fs->Name[i], L"FAT") == 0 || _wcsicmp(Fs->Name[i], L"FAT32") == 0)
            continue;
        Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Fs->Name[i]);
        if (Current && Current[0] && _wcsicmp(Current, Fs->Name[i]) == 0)
            Select = Index;
    }
    if (SendMessageW(hCombo, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(hCombo, CB_SETCURSEL, Select >= 0 ? Select : 0, 0);
}

/* The allocation unit sizes a file system can use on a volume of this size (the limits of the cluster counts). */
VOID
FillClusterCombo(HWND hCombo, PCWSTR FileSystem, ULONGLONG VolumeBytes)
{
    WCHAR Text[32];
    ULONG Size;
    int Index;

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    LoadStringBuffer(IDS_FM_CLUSTER_DEFAULT, Text, ARRAYSIZE(Text));
    Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Text);
    SendMessageW(hCombo, CB_SETITEMDATA, Index, 0);
    for (Size = 512; Size <= 65536; Size *= 2)
    {
        ULONGLONG Clusters = VolumeBytes / Size;
        BOOL Ok;

        if (_wcsicmp(FileSystem, L"NTFS") == 0)
            Ok = (Size <= VolumeBytes);
        else if (_wcsicmp(FileSystem, L"FAT") == 0)
            Ok = (Clusters >= 4085 && Clusters < 65525);
        else if (_wcsicmp(FileSystem, L"FAT32") == 0)
            Ok = (Clusters >= 65525 && Size <= 32768);
        else
            Ok = FALSE;
        if (!Ok)
            continue;
        if (Size >= 16384)
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%luK", Size / 1024);
        else
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", Size);
        Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Text);
        SendMessageW(hCombo, CB_SETITEMDATA, Index, Size);
    }
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

ULONG
ComboCluster(HWND hDlg, UINT Id)
{
    int Index = (int)SendDlgItemMessageW(hDlg, Id, CB_GETCURSEL, 0, 0);

    return Index < 0 ? 0 : (ULONG)SendDlgItemMessageW(hDlg, Id, CB_GETITEMDATA, Index, 0);
}

VOID
CurrentPaths(PCWSTR Guid, PWSTR Buffer, DWORD Count)
{
    DWORD Length = 0;

    Buffer[0] = Buffer[1] = 0;
    if (!GetVolumePathNamesForVolumeNameW(Guid, Buffer, Count - 1, &Length))
        Buffer[0] = Buffer[1] = 0;
    else
        Buffer[min(Length, Count - 1)] = 0;
}

BOOL
IsLetterPath(PCWSTR Path)
{
    return wcslen(Path) == 3 && Path[1] == L':' && Path[2] == L'\\';
}

/* only a volume that keeps reparse points can hold a mount point */

DWORD
ApplyPathChoice(PCWSTR Guid, PATH_CHOICE Choice, WCHAR Letter, PCWSTR Folder)
{
    WCHAR Paths[512];
    DWORD Error = ERROR_SUCCESS;

    CurrentPaths(Guid, Paths, ARRAYSIZE(Paths));
    switch (Choice)
    {
        case PATH_LETTER:
            Error = DmChangeDriveLetter(Guid, Paths, Letter);
            break;
        case PATH_FOLDER:
            /* the system may have given a letter on its own: the user chose a folder only */
            Error = DmChangeDriveLetter(Guid, Paths, 0);
            if (Error == ERROR_SUCCESS)
                Error = DmAddMountPath(Guid, Folder);
            break;
        case PATH_NONE:
            Error = DmChangeDriveLetter(Guid, Paths, 0);
            break;
    }
    return Error;
}
