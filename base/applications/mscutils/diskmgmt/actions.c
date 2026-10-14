/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The actions of the window: menus, confirmations and dialogs in front of the writer
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "diskmgmt.h"

#define WM_DM_DONE      (WM_APP + 20)
#define WM_DM_PROGRESS  (WM_APP + 21)

#define DM_MENU_TEXT    96

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

/* what an action needs to know about its target, copied from the model so that it outlives a refresh */
typedef struct _TARGET
{
    ULONG Disk;
    DM_DEVICE Device;
    DM_STYLE Style;
    ULONGLONG Offset;
    ULONGLONG Length;
    BOOL InExtended;
    BOOL Protected;
    BOOL Removable;
    WCHAR Guid[64];
    WCHAR Letter[4];
    WCHAR Paths[256];
    WCHAR Label[64];
    WCHAR FileSystem[32];
} TARGET, *PTARGET;

/* ------------------------------------------------------------------- helpers */

static BOOL
DiskWritable(const DM_DISK *Disk)
{
    return Disk->Device != DM_DEV_CDROM && Disk->HasMedia && !Disk->Dynamic &&
           (wcscmp(Disk->StatusText, L"Online") == 0 || wcscmp(Disk->StatusText, L"Not Initialized") == 0);
}

static BOOL
RegionIsPartition(const DM_REGION *Region)
{
    return Region && (Region->Kind == DM_KIND_PRIMARY || Region->Kind == DM_KIND_LOGICAL);
}

static BOOL
RegionProtected(const DM_REGION *Region)
{
    return Region->IsBoot || Region->IsSystem || Region->IsPageFile || Region->IsCrashDump;
}

static BOOL
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

static BOOL
ExtendedIsEmpty(const DM_DISK *Disk)
{
    ULONG i;

    for (i = 0; i < Disk->RegionCount; i++)
        if (Disk->Regions[i].Kind == DM_KIND_LOGICAL)
            return FALSE;
    return Disk->HasExtended;
}

/* Windows shows a disk without a signature as not initialized; ReactOS reads such a disk as an empty MBR disk. */
static BOOL
DiskNeedsInit(const DM_DISK *Disk)
{
    if (Disk->Style == DM_STYLE_RAW)
        return wcscmp(Disk->StatusText, L"Not Initialized") == 0;
    return Disk->Style == DM_STYLE_MBR && Disk->Signature == 0 && !DiskHasPartitions(Disk);
}

static VOID
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
    if (Region->HasVolume)
    {
        StringCchCopyW(Target->Guid, ARRAYSIZE(Target->Guid), Region->VolumeGuid);
        StringCchCopyW(Target->Letter, ARRAYSIZE(Target->Letter), Region->Letter);
        memcpy(Target->Paths, Region->MountPaths, sizeof(Target->Paths));
        StringCchCopyW(Target->Label, ARRAYSIZE(Target->Label), Region->Label);
        StringCchCopyW(Target->FileSystem, ARRAYSIZE(Target->FileSystem), Region->FileSystem);
    }
}

BOOL
ActionsEnabled(UINT Id, const DM_DISK *Disk, const DM_REGION *Region)
{
    BOOL Writable;

    if (Id == ID_RESCAN)
        return TRUE;
    if (Disk == NULL)
        return FALSE;
    Writable = DiskWritable(Disk);

    switch (Id)
    {
        case ID_OPEN:
        case ID_EXPLORE:
            return Region && Region->HasVolume && Region->HasFsInfo && (Region->Letter[0] || Region->MountPaths[0]);
        case ID_MARK_ACTIVE:
            return Writable && Disk->Style == DM_STYLE_MBR && Region && Region->Kind == DM_KIND_PRIMARY &&
                   !Region->Active && !Region->IsDynamic;
        case ID_CHANGE_LETTER:
            return Region && RegionIsPartition(Region) && Region->HasVolume && Region->VolumeGuid[0] != 0;
        case ID_FORMAT:
            return Writable && RegionIsPartition(Region) && Region->HasVolume && Region->VolumeGuid[0] != 0 &&
                   !RegionProtected(Region) && !Region->IsDynamic && !Region->IsEfi && !DmIsBootPartition(Disk->Number, Region->Offset);
        case ID_DELETE_VOLUME:
            return Writable && RegionIsPartition(Region) && !RegionProtected(Region) && !Region->IsDynamic && !Region->IsEfi && !DmIsBootPartition(Disk->Number, Region->Offset);
        case ID_DELETE_PARTITION:
            return Writable && Region && Region->Kind == DM_KIND_FREE && Disk->Style == DM_STYLE_MBR && ExtendedIsEmpty(Disk);
        case ID_NEW_VOLUME:
            return Writable && Region && (Region->Kind == DM_KIND_UNALLOCATED || Region->Kind == DM_KIND_FREE) &&
                   (Disk->Style == DM_STYLE_MBR || Disk->Style == DM_STYLE_GPT);
        case ID_INIT_DISK:
            return Writable && Region == NULL && DiskNeedsInit(Disk);
        case ID_CONVERT_GPT:
            return Writable && Region == NULL && Disk->Style == DM_STYLE_MBR && !DiskHasPartitions(Disk);
        case ID_CONVERT_MBR:
            return Writable && Region == NULL && Disk->Style == DM_STYLE_GPT && !DiskHasPartitions(Disk);
    }
    return FALSE;
}

static VOID
AddItem(HMENU hMenu, UINT Id, UINT TextId, const DM_DISK *Disk, const DM_REGION *Region)
{
    WCHAR Text[DM_MENU_TEXT];

    LoadStringBuffer(TextId, Text, ARRAYSIZE(Text));
    AppendMenuW(hMenu, MF_STRING | (ActionsEnabled(Id, Disk, Region) ? MF_ENABLED : MF_GRAYED), Id, Text);
}

static VOID
AddSeparator(HMENU hMenu)
{
    int Count = GetMenuItemCount(hMenu);

    if (Count > 0 && !(GetMenuState(hMenu, Count - 1, MF_BYPOSITION) & MF_SEPARATOR))
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
}

VOID
ActionsBuildMenu(HMENU hMenu, const DM_DISK *Disk, const DM_REGION *Region, BOOL WithProperties)
{
    while (GetMenuItemCount(hMenu) > 0)
        DeleteMenu(hMenu, 0, MF_BYPOSITION);
    if (Disk == NULL)
    {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, L"(none)");
        return;
    }

    if (Region == NULL)
    {
        AddItem(hMenu, ID_INIT_DISK, IDS_ACT_INIT, Disk, Region);
        AddItem(hMenu, ID_CONVERT_GPT, IDS_ACT_GPT, Disk, Region);
        AddItem(hMenu, ID_CONVERT_MBR, IDS_ACT_MBR, Disk, Region);
    }
    else if (RegionIsPartition(Region))
    {
        AddItem(hMenu, ID_OPEN, IDS_ACT_OPEN, Disk, Region);
        AddItem(hMenu, ID_EXPLORE, IDS_ACT_EXPLORE, Disk, Region);
        AddSeparator(hMenu);
        if (Disk->Style == DM_STYLE_MBR)
            AddItem(hMenu, ID_MARK_ACTIVE, IDS_ACT_ACTIVE, Disk, Region);
        AddItem(hMenu, ID_CHANGE_LETTER, IDS_ACT_LETTER, Disk, Region);
        AddItem(hMenu, ID_FORMAT, IDS_ACT_FORMAT, Disk, Region);
        AddSeparator(hMenu);
        AddItem(hMenu, ID_DELETE_VOLUME, IDS_ACT_DELETE_VOLUME, Disk, Region);
    }
    else
    {
        AddItem(hMenu, ID_NEW_VOLUME, IDS_ACT_NEW, Disk, Region);
        if (Region->Kind == DM_KIND_FREE)
            AddItem(hMenu, ID_DELETE_PARTITION, IDS_ACT_DELETE_PARTITION, Disk, Region);
    }
    if (WithProperties)
    {
        WCHAR Text[DM_MENU_TEXT];
        BOOL Enabled = (Region == NULL) || RegionIsPartition(Region);

        AddSeparator(hMenu);
        LoadStringBuffer(IDS_ACT_PROPERTIES, Text, ARRAYSIZE(Text));
        AppendMenuW(hMenu, MF_STRING | (Enabled ? MF_ENABLED : MF_GRAYED), ID_PROPERTIES, Text);
    }
}

/* --------------------------------------------------------------------- errors */

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

static VOID
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

static BOOL
Confirm(HWND hOwner, UINT TextId, PCWSTR Argument)
{
    WCHAR Title[64], Format[400], Text[512];

    LoadStringBuffer(IDS_CONFIRM_TITLE, Title, ARRAYSIZE(Title));
    LoadStringBuffer(TextId, Format, ARRAYSIZE(Format));
    if (Argument)
        StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Argument);
    else
        StringCchCopyW(Text, ARRAYSIZE(Text), Format);
    return MessageBoxW(hOwner, Text, Title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

/* ----------------------------------------------------------- busy and progress */

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

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Work);
            Work->hDlg = hDlg;
            SetDlgItemTextW(hDlg, IDC_PR_TEXT, Work->Text);
            SendDlgItemMessageW(hDlg, IDC_PR_BAR, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            if (!Work->Bar)
                SendDlgItemMessageW(hDlg, IDC_PR_BAR, PBM_SETMARQUEE, TRUE, 50);
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

/* runs Func on a thread behind a dialog the user cannot close; returns what Func returned */
static DWORD
RunBusy(HWND hOwner, PCWSTR Text, BOOL Bar, DWORD (*Func)(PVOID), PVOID Arg, PWORK *OutWork)
{
    static WORK Work;

    ZeroMemory(&Work, sizeof(Work));
    Work.Func = Func;
    Work.Arg = Arg;
    Work.Bar = Bar;
    StringCchCopyW(Work.Text, ARRAYSIZE(Work.Text), Text ? Text : L"");
    if (Text == NULL)
        LoadStringBuffer(IDS_WORKING, Work.Text, ARRAYSIZE(Work.Text));
    if (OutWork)
        *OutWork = &Work;
    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PROGRESS), hOwner, ProgressProc, (LPARAM)&Work) == -1 && Work.hThread == NULL)
        Work.Result = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    if (Work.hThread)
    {
        /* the dialog can end before the worker does (owner destroyed, quit message) */
        WaitForSingleObject(Work.hThread, INFINITE);
        CloseHandle(Work.hThread);
        Work.hThread = NULL;
    }
    return Work.Result;
}

static PWORK g_Work;

typedef struct _FORMAT_JOB
{
    DM_FORMAT Format;
} FORMAT_JOB, *PFORMAT_JOB;

static VOID
FormatProgress(PVOID Context, ULONG Percent)
{
    UNREFERENCED_PARAMETER(Context);
    if (g_Work && g_Work->hDlg)
        PostMessageW(g_Work->hDlg, WM_DM_PROGRESS, Percent > 100 ? 100 : Percent, 0);
}

/* the partition type follows the file system, as in Windows; a failure is told apart from a failed format */
static VOID
TypeFix(HWND hOwner, ULONG Disk, ULONGLONG Offset, PCWSTR Guid)
{
    DWORD Error = DmFixPartitionType(Disk, Offset, Guid);

    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, DM_ERR_TYPE_NOT_UPDATED, NULL);
}

static DWORD
FormatThread(PVOID Arg)
{
    return DmFormatVolume(&((PFORMAT_JOB)Arg)->Format);
}

static DWORD
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
    g_Work = NULL;
    /* the callback needs the dialog window: RunBusy publishes it through g_Work before the thread starts */
    Error = RunBusy(hOwner, Text, TRUE, FormatThread, &Job, &g_Work);
    if (Error == DM_ERR_VOLUME_IN_USE && Confirm(hOwner, IDS_CONFIRM_FORCE, NULL))
    {
        /* the user allowed the volume to be dismounted */
        Job.Format.Force = TRUE;
        Error = RunBusy(hOwner, Text, TRUE, FormatThread, &Job, &g_Work);
    }
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, Job.Format.Output);
    g_Work = NULL;
    return Error;
}

/* ------------------------------------------------------------------ letters */

static DWORD
UsedLetters(VOID)
{
    return GetLogicalDrives();
}

static VOID
FillLetterCombo(HWND hCombo, BOOL WithNone, WCHAR Current)
{
    DWORD Used = UsedLetters();
    WCHAR Letter;
    int Select = -1, First = -1, Index;

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    if (WithNone)
    {
        WCHAR Text[64];

        LoadStringBuffer(IDS_NV_NO_LETTER, Text, ARRAYSIZE(Text));
        Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Text);
        SendMessageW(hCombo, CB_SETITEMDATA, Index, 0);
    }
    for (Letter = L'C'; Letter <= L'Z'; Letter++)
    {
        WCHAR Text[4];

        if ((Used & (1UL << (Letter - L'A'))) && Letter != Current)
            continue;
        Text[0] = Letter; Text[1] = L':'; Text[2] = 0;
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

static WCHAR
ComboLetter(HWND hDlg, UINT Id)
{
    int Index = (int)SendDlgItemMessageW(hDlg, Id, CB_GETCURSEL, 0, 0);

    if (Index < 0)
        return 0;
    return (WCHAR)SendDlgItemMessageW(hDlg, Id, CB_GETITEMDATA, Index, 0);
}

/* ------------------------------------------------------- initialize dialog */

typedef struct _INIT_DATA
{
    ULONG Disk;
    DM_STYLE Style;
} INIT_DATA, *PINIT_DATA;

static INT_PTR CALLBACK
InitProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PINIT_DATA Data = (PINIT_DATA)lParam;
            WCHAR Format[160], Text[200];

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            LoadStringBuffer(IDS_ID_TEXT_FMT, Format, ARRAYSIZE(Format));
            StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Data->Disk);
            SetDlgItemTextW(hDlg, IDC_ID_TEXT, Text);
            CheckRadioButton(hDlg, IDC_ID_MBR, IDC_ID_GPT, IDC_ID_MBR);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
            {
                PINIT_DATA Data = (PINIT_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);

                Data->Style = IsDlgButtonChecked(hDlg, IDC_ID_GPT) == BST_CHECKED ? DM_STYLE_GPT : DM_STYLE_MBR;
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static BOOL
RunInitialize(HWND hOwner, const DM_DISK *Disk)
{
    INIT_DATA Data;
    DWORD Error;

    Data.Disk = Disk->Number;
    Data.Style = DM_STYLE_MBR;
    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_INITDISK), hOwner, InitProc, (LPARAM)&Data) != IDOK)
        return FALSE;
    Error = DmInitializeDisk(Data.Disk, Data.Style);
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    return TRUE;
}

static BOOL
RunConvert(HWND hOwner, const DM_DISK *Disk, DM_STYLE To)
{
    WCHAR Style[32];
    DWORD Error;

    LoadStringBuffer(To == DM_STYLE_GPT ? IDS_STYLE_GPT_SHORT : IDS_STYLE_MBR_SHORT, Style, ARRAYSIZE(Style));
    if (!Confirm(hOwner, IDS_CONFIRM_CONVERT, Style))
        return FALSE;
    Error = DmConvertDisk(Disk->Number, To);
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    return TRUE;
}

/* -------------------------------------------------------------- format dialog */

typedef struct _FORMAT_DATA
{
    DM_FS_LIST Fs;
    WCHAR Title[96];
    WCHAR VolumeText[128];
    WCHAR Label[64];
    WCHAR CurrentFs[32];
    BOOL AllowNone;             /* the new volume dialog may skip the format */
    WCHAR FileSystem[DM_FS_NAME];
    ULONG Cluster;
    BOOL Quick;
    BOOL Skip;
} FORMAT_DATA, *PFORMAT_DATA;

static const ULONG g_ClusterSizes[] = { 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

static VOID
FillFsCombo(HWND hCombo, const DM_FS_LIST *Fs, PCWSTR Current, BOOL AllowNone)
{
    ULONG i;
    int Select = -1, Index;

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    if (AllowNone)
    {
        WCHAR Text[64];

        LoadStringBuffer(IDS_NV_NO_FORMAT, Text, ARRAYSIZE(Text));
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Text);
    }
    for (i = 0; i < Fs->Count; i++)
    {
        Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Fs->Name[i]);
        if (Current && Current[0] && _wcsicmp(Current, Fs->Name[i]) == 0)
            Select = Index;
        else if (Select < 0 && _wcsicmp(Fs->Name[i], L"NTFS") == 0)
            Select = Index;
    }
    if (Select < 0)
        Select = AllowNone && Fs->Count == 0 ? 0 : (AllowNone ? 1 : 0);
    if (SendMessageW(hCombo, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(hCombo, CB_SETCURSEL, Select, 0);
}

static VOID
FillClusterCombo(HWND hCombo)
{
    WCHAR Text[32];
    ULONG i;
    int Index;

    LoadStringBuffer(IDS_FM_CLUSTER_DEFAULT, Text, ARRAYSIZE(Text));
    Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Text);
    SendMessageW(hCombo, CB_SETITEMDATA, Index, 0);
    for (i = 0; i < ARRAYSIZE(g_ClusterSizes); i++)
    {
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", g_ClusterSizes[i]);
        Index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)Text);
        SendMessageW(hCombo, CB_SETITEMDATA, Index, g_ClusterSizes[i]);
    }
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

static INT_PTR CALLBACK
FormatProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PFORMAT_DATA Data = (PFORMAT_DATA)lParam;

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            SetWindowTextW(hDlg, Data->Title);
            SetDlgItemTextW(hDlg, IDC_FM_VOLUME, Data->VolumeText);
            SetDlgItemTextW(hDlg, IDC_FM_LABEL, Data->Label);
            FillFsCombo(GetDlgItem(hDlg, IDC_FM_FS), &Data->Fs, Data->CurrentFs, FALSE);
            FillClusterCombo(GetDlgItem(hDlg, IDC_FM_CLUSTER));
            CheckDlgButton(hDlg, IDC_FM_QUICK, BST_CHECKED);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
            {
                PFORMAT_DATA Data = (PFORMAT_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);
                int Index = (int)SendDlgItemMessageW(hDlg, IDC_FM_FS, CB_GETCURSEL, 0, 0);

                if (Index < 0)
                    return TRUE;
                SendDlgItemMessageW(hDlg, IDC_FM_FS, CB_GETLBTEXT, Index, (LPARAM)Data->FileSystem);
                GetDlgItemTextW(hDlg, IDC_FM_LABEL, Data->Label, ARRAYSIZE(Data->Label));
                Index = (int)SendDlgItemMessageW(hDlg, IDC_FM_CLUSTER, CB_GETCURSEL, 0, 0);
                Data->Cluster = Index < 0 ? 0 : (ULONG)SendDlgItemMessageW(hDlg, IDC_FM_CLUSTER, CB_GETITEMDATA, Index, 0);
                Data->Quick = IsDlgButtonChecked(hDlg, IDC_FM_QUICK) == BST_CHECKED;
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static PCWSTR
VolumeDisplayName(const TARGET *Target, PWSTR Buffer, SIZE_T Count)
{
    if (Target->Label[0] && Target->Letter[0])
        StringCchPrintfW(Buffer, Count, L"%s (%s)", Target->Label, Target->Letter);
    else if (Target->Letter[0])
        StringCchPrintfW(Buffer, Count, L"(%s)", Target->Letter);
    else if (Target->Label[0])
        StringCchCopyW(Buffer, Count, Target->Label);
    else
        StringCchPrintfW(Buffer, Count, L"Disk %u", Target->Disk);
    return Buffer;
}

static BOOL
RunFormatAction(HWND hOwner, const TARGET *Target)
{
    FORMAT_DATA Data;
    WCHAR Name[128], Format[64];
    DWORD Error;

    ZeroMemory(&Data, sizeof(Data));
    if (!DmQueryFileSystems(&Data.Fs))
    {
        ShowError(hOwner, DM_ERR_NO_PROVIDER, NULL);
        return FALSE;
    }
    VolumeDisplayName(Target, Name, ARRAYSIZE(Name));
    LoadStringBuffer(IDS_FM_TITLE, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Data.Title, ARRAYSIZE(Data.Title), Format, Name);
    LoadStringBuffer(IDS_FM_VOLUME_FMT, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Data.VolumeText, ARRAYSIZE(Data.VolumeText), Format, Name);
    StringCchCopyW(Data.Label, ARRAYSIZE(Data.Label), Target->Label);
    StringCchCopyW(Data.CurrentFs, ARRAYSIZE(Data.CurrentFs), Target->FileSystem);
    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_FORMAT), hOwner, FormatProc, (LPARAM)&Data) != IDOK)
        return FALSE;
    if (!Confirm(hOwner, IDS_CONFIRM_FORMAT, NULL))
        return FALSE;

    Error = RunFormat(hOwner, Target->Guid, Data.FileSystem, Data.Label, Data.Quick, Data.Cluster, Target->Removable);
    if (Error == ERROR_SUCCESS && Target->Style == DM_STYLE_MBR)
        TypeFix(hOwner, Target->Disk, Target->Offset, Target->Guid);
    return TRUE;
}

/* ---------------------------------------------------------- new volume dialog */

typedef struct _NEWVOL_DATA
{
    ULONGLONG MaxMB;
    WCHAR Info[160];
    WCHAR Note[240];
    WCHAR Title[96];
    DM_FS_LIST Fs;
    ULONGLONG SizeMB;
    WCHAR Letter;
    WCHAR FileSystem[DM_FS_NAME];       /* empty: leave the volume unformatted */
    WCHAR Label[64];
    BOOL Quick;
} NEWVOL_DATA, *PNEWVOL_DATA;

static INT_PTR CALLBACK
NewVolProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PNEWVOL_DATA Data = (PNEWVOL_DATA)lParam;
            WCHAR Text[32];

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            SetWindowTextW(hDlg, Data->Title);
            SetDlgItemTextW(hDlg, IDC_NV_INFO, Data->Info);
            SetDlgItemTextW(hDlg, IDC_NV_NOTE, Data->Note);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Data->MaxMB);
            SetDlgItemTextW(hDlg, IDC_NV_SIZE, Text);
            FillLetterCombo(GetDlgItem(hDlg, IDC_NV_LETTER), TRUE, 0);
            FillFsCombo(GetDlgItem(hDlg, IDC_NV_FS), &Data->Fs, NULL, TRUE);
            LoadStringBuffer(IDS_NV_LABEL_DEFAULT, Text, ARRAYSIZE(Text));
            SetDlgItemTextW(hDlg, IDC_NV_LABEL, Text);
            CheckDlgButton(hDlg, IDC_NV_QUICK, BST_CHECKED);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_NV_FS && HIWORD(wParam) == CBN_SELCHANGE)
            {
                BOOL Format = SendDlgItemMessageW(hDlg, IDC_NV_FS, CB_GETCURSEL, 0, 0) > 0;

                EnableWindow(GetDlgItem(hDlg, IDC_NV_LABEL), Format);
                EnableWindow(GetDlgItem(hDlg, IDC_NV_QUICK), Format);
                return TRUE;
            }
            if (LOWORD(wParam) == IDOK)
            {
                PNEWVOL_DATA Data = (PNEWVOL_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);
                WCHAR Text[32];
                PWCHAR End;
                unsigned long long Size;
                int Index;

                GetDlgItemTextW(hDlg, IDC_NV_SIZE, Text, ARRAYSIZE(Text));
                Size = _wcstoui64(Text, &End, 10);
                if (Text[0] == 0 || *End != 0 || Size < 1 || Size > Data->MaxMB)
                {
                    WCHAR Format[96], Message[160], Title[64];

                    LoadStringBuffer(IDS_NV_SIZE_RANGE, Format, ARRAYSIZE(Format));
                    StringCchPrintfW(Message, ARRAYSIZE(Message), Format, Data->MaxMB);
                    LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
                    MessageBoxW(hDlg, Message, Title, MB_OK | MB_ICONERROR);
                    return TRUE;
                }
                Data->SizeMB = Size;
                Data->Letter = ComboLetter(hDlg, IDC_NV_LETTER);
                Index = (int)SendDlgItemMessageW(hDlg, IDC_NV_FS, CB_GETCURSEL, 0, 0);
                Data->FileSystem[0] = 0;
                if (Index > 0)
                    SendDlgItemMessageW(hDlg, IDC_NV_FS, CB_GETLBTEXT, Index, (LPARAM)Data->FileSystem);
                GetDlgItemTextW(hDlg, IDC_NV_LABEL, Data->Label, ARRAYSIZE(Data->Label));
                Data->Quick = IsDlgButtonChecked(hDlg, IDC_NV_QUICK) == BST_CHECKED;
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static VOID
CurrentPaths(PCWSTR Guid, PWSTR Buffer, DWORD Count)
{
    DWORD Length = 0;

    Buffer[0] = Buffer[1] = 0;
    if (!GetVolumePathNamesForVolumeNameW(Guid, Buffer, Count - 1, &Length))
        Buffer[0] = Buffer[1] = 0;
    else
        Buffer[min(Length, Count - 1)] = 0;
}

static BOOL
RunNewVolume(HWND hOwner, const TARGET *Target)
{
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    PNEWVOL_DATA Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NEWVOL_DATA));
    DL_FREE Free;
    BOOL NeedsExtended = FALSE, Changed = FALSE;
    DWORD Error;
    DM_CREATED Created;
    WCHAR Format[160], FreeText[32], MaxText[32], Paths[512];

    if (Layout == NULL || Data == NULL)
        goto Done;
    Error = DmPlanPartition(Target->Disk, Target->Offset, Target->InExtended, &Free, &NeedsExtended, Layout);
    if (Error == ERROR_SUCCESS && (Free.MaxLength < DM_MIB || Free.WizardMax < DM_MIB))
        Error = DM_ERR_LAYOUT_BASE + DL_E_NO_SPACE;
    if (Error != ERROR_SUCCESS)
    {
        ShowError(hOwner, Error, NULL);
        goto Done;
    }
    DmQueryFileSystems(&Data->Fs);
    Data->MaxMB = Free.WizardMax / DM_MIB;
    DmFormatSizeMB(Free.Length / DM_MIB, FreeText, ARRAYSIZE(FreeText));
    DmFormatSizeMB(Data->MaxMB, MaxText, ARRAYSIZE(MaxText));
    LoadStringBuffer(IDS_NV_INFO_FMT, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Data->Info, ARRAYSIZE(Data->Info), Format, FreeText, MaxText);
    if (NeedsExtended)
        LoadStringBuffer(IDS_NV_NOTE_EXTENDED, Data->Note, ARRAYSIZE(Data->Note));
    else if (Target->InExtended)
        LoadStringBuffer(IDS_NV_NOTE_LOGICAL, Data->Note, ARRAYSIZE(Data->Note));
    else if (DlGptNeedsReserved(Layout))
        LoadStringBuffer(IDS_NV_NOTE_RESERVED, Data->Note, ARRAYSIZE(Data->Note));
    LoadStringBuffer(IDS_NV_TITLE, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Data->Title, ARRAYSIZE(Data->Title), Format, Target->Disk);
    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_NEWVOL), hOwner, NewVolProc, (LPARAM)Data) != IDOK)
        goto Done;

    Changed = TRUE;
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    Error = DmCreatePartition(Target->Disk, Target->Offset, Target->InExtended, Data->SizeMB * DM_MIB, &Created);
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (Error != ERROR_SUCCESS)
    {
        ShowError(hOwner, Error, NULL);
        goto Done;
    }
    if (Created.VolumeGuid[0] == 0)
    {
        WCHAR Title[64], Text[200];

        LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
        LoadStringBuffer(IDS_NV_CREATED_NO_VOLUME, Text, ARRAYSIZE(Text));
        MessageBoxW(hOwner, Text, Title, MB_OK | MB_ICONWARNING);
        goto Done;
    }

    CurrentPaths(Created.VolumeGuid, Paths, ARRAYSIZE(Paths));
    Error = DmChangeDriveLetter(Created.VolumeGuid, Paths, Data->Letter);
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    if (Data->FileSystem[0])
    {
        Error = RunFormat(hOwner, Created.VolumeGuid, Data->FileSystem, Data->Label, Data->Quick, 0, Target->Removable);
        if (Error == ERROR_SUCCESS && Target->Style == DM_STYLE_MBR)
            TypeFix(hOwner, Target->Disk, Created.Offset, Created.VolumeGuid);
    }
Done:
    if (Layout)
        HeapFree(GetProcessHeap(), 0, Layout);
    if (Data)
        HeapFree(GetProcessHeap(), 0, Data);
    return Changed;
}

/* ------------------------------------------------------------ delete actions */

static BOOL
RunDelete(HWND hOwner, const TARGET *Target, BOOL ExtendedPartition, ULONGLONG ExtOffset, ULONGLONG ExtLength)
{
    DWORD Error;

    if (!Confirm(hOwner, IDS_CONFIRM_DELETE, NULL))
        return FALSE;
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (ExtendedPartition)
    {
        Error = DmDeletePartition(Target->Disk, ExtOffset, ExtLength, NULL, FALSE);
    }
    else
    {
        Error = DmDeletePartition(Target->Disk, Target->Offset, Target->Length, Target->Guid, FALSE);
        if (Error == DM_ERR_VOLUME_IN_USE)
        {
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            if (!Confirm(hOwner, IDS_CONFIRM_FORCE, NULL))
                return FALSE;
            SetCursor(LoadCursorW(NULL, IDC_WAIT));
            Error = DmDeletePartition(Target->Disk, Target->Offset, Target->Length, Target->Guid, TRUE);
        }
    }
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    return TRUE;
}

static BOOL
RunMarkActive(HWND hOwner, const TARGET *Target, const DM_DISK *Disk)
{
    DWORD Error;
    ULONG i;
    BOOL OtherActive = FALSE;

    /* Windows asks only when another partition of the disk is active at the moment */
    for (i = 0; i < Disk->RegionCount; i++)
        if (Disk->Regions[i].Active && Disk->Regions[i].Offset != Target->Offset)
            OtherActive = TRUE;
    if (OtherActive && !Confirm(hOwner, IDS_CONFIRM_ACTIVE, NULL))
        return FALSE;
    Error = DmMarkActive(Target->Disk, Target->Offset);
    if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    return TRUE;
}

static VOID
RunOpen(HWND hOwner, const TARGET *Target, BOOL Explore)
{
    WCHAR Path[MAX_PATH];

    if (Target->Letter[0])
        StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\", Target->Letter);
    else
        StringCchCopyW(Path, ARRAYSIZE(Path), Target->Paths);
    ShellExecuteW(hOwner, Explore ? L"explore" : L"open", Path, NULL, NULL, SW_SHOWNORMAL);
}

/* ------------------------------------------------- change drive letter and paths */

typedef struct _LETTER_DATA
{
    TARGET Target;
    BOOL Changed;
    BOOL LetterOnly;            /* change: only a drive letter may be picked */
    BOOL IsFolder;              /* out */
    WCHAR Letter;               /* out */
    WCHAR Folder[MAX_PATH];     /* out */
    WCHAR Current;              /* the letter being changed */
} LETTER_DATA, *PLETTER_DATA;

static INT_PTR CALLBACK
PickProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PLETTER_DATA Data = (PLETTER_DATA)lParam;

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            FillLetterCombo(GetDlgItem(hDlg, IDC_PL_LETTER), FALSE, Data->Current);
            CheckRadioButton(hDlg, IDC_PL_LETTER_RADIO, IDC_PL_FOLDER_RADIO, IDC_PL_LETTER_RADIO);
            if (Data->LetterOnly)
            {
                EnableWindow(GetDlgItem(hDlg, IDC_PL_FOLDER_RADIO), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_PL_FOLDER), FALSE);
            }
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
            {
                PLETTER_DATA Data = (PLETTER_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);

                if (IsDlgButtonChecked(hDlg, IDC_PL_FOLDER_RADIO) == BST_CHECKED)
                {
                    SIZE_T Len;

                    GetDlgItemTextW(hDlg, IDC_PL_FOLDER, Data->Folder, ARRAYSIZE(Data->Folder));
                    Len = wcslen(Data->Folder);
                    if (Len >= 3 && Data->Folder[Len - 1] != L'\\' && Len + 1 < ARRAYSIZE(Data->Folder))
                    {
                        Data->Folder[Len++] = L'\\';
                        Data->Folder[Len] = 0;
                    }
                    if (Len < 3 || Data->Folder[1] != L':' || Data->Folder[Len - 1] != L'\\')
                    {
                        WCHAR Text[160], Title[64];

                        LoadStringBuffer(IDS_PL_BAD_FOLDER, Text, ARRAYSIZE(Text));
                        LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
                        MessageBoxW(hDlg, Text, Title, MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    Data->IsFolder = TRUE;
                }
                else
                {
                    Data->Letter = ComboLetter(hDlg, IDC_PL_LETTER);
                    Data->IsFolder = FALSE;
                    if (Data->Letter == 0)
                    {
                        WCHAR Text[96], Title[64];

                        LoadStringBuffer(IDS_PL_NO_LETTER_FREE, Text, ARRAYSIZE(Text));
                        LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
                        MessageBoxW(hDlg, Text, Title, MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                }
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static BOOL
IsLetterPath(PCWSTR Path)
{
    return wcslen(Path) == 3 && Path[1] == L':' && Path[2] == L'\\';
}

static VOID
LoadPathList(HWND hDlg, PLETTER_DATA Data)
{
    WCHAR Paths[512];
    PCWSTR Path;
    HWND hList = GetDlgItem(hDlg, IDC_CL_LIST);
    int Count = 0;

    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    CurrentPaths(Data->Target.Guid, Paths, ARRAYSIZE(Paths));
    for (Path = Paths; *Path; Path += wcslen(Path) + 1)
    {
        int Index = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)Path);

        SendMessageW(hList, LB_SETITEMDATA, Index, IsLetterPath(Path) ? 1 : 2);
        Count++;
    }
    if (Count == 0)
    {
        WCHAR Text[64];
        int Index;

        LoadStringBuffer(IDS_CL_NOTHING, Text, ARRAYSIZE(Text));
        Index = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)Text);
        SendMessageW(hList, LB_SETITEMDATA, Index, 0);
    }
    SendMessageW(hList, LB_SETCURSEL, 0, 0);
    SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CL_LIST, LBN_SELCHANGE), (LPARAM)hList);
}

static INT_PTR CALLBACK
LetterProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PLETTER_DATA Data = (PLETTER_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Name[128], Format[96], Text[200];

            Data = (PLETTER_DATA)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            VolumeDisplayName(&Data->Target, Name, ARRAYSIZE(Name));
            LoadStringBuffer(IDS_CL_TITLE, Format, ARRAYSIZE(Format));
            StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Name);
            SetWindowTextW(hDlg, Text);
            SetDlgItemTextW(hDlg, IDC_CL_VOLUME, Name);
            LoadPathList(hDlg, Data);
            return TRUE;
        }
        case WM_COMMAND:
        {
            HWND hList = GetDlgItem(hDlg, IDC_CL_LIST);
            int Sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            LRESULT Kind = Sel >= 0 ? SendMessageW(hList, LB_GETITEMDATA, Sel, 0) : 0;

            switch (LOWORD(wParam))
            {
                case IDC_CL_LIST:
                    if (HIWORD(wParam) == LBN_SELCHANGE)
                    {
                        EnableWindow(GetDlgItem(hDlg, IDC_CL_CHANGE), Kind == 1);
                        EnableWindow(GetDlgItem(hDlg, IDC_CL_REMOVE), Kind != 0);
                    }
                    return TRUE;

                case IDC_CL_ADD:
                case IDC_CL_CHANGE:
                {
                    BOOL Change = (LOWORD(wParam) == IDC_CL_CHANGE);
                    WCHAR Old[MAX_PATH], Paths[512], New[MAX_PATH];
                    DWORD Error;

                    if (Data->Target.Protected)
                    {
                        ShowError(hDlg, DM_ERR_PROTECTED, NULL);
                        return TRUE;
                    }
                    ZeroMemory(&Old, sizeof(Old));
                    Data->LetterOnly = Change;
                    Data->Current = 0;
                    if (Change)
                    {
                        SendMessageW(hList, LB_GETTEXT, Sel, (LPARAM)Old);
                        Data->Current = Old[0];
                    }
                    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PICKLETTER), hDlg, PickProc, (LPARAM)Data) != IDOK)
                        return TRUE;
                    if (Change && !Confirm(hDlg, IDS_CONFIRM_LETTER, NULL))
                        return TRUE;
                    CurrentPaths(Data->Target.Guid, Paths, ARRAYSIZE(Paths));
                    if (Data->IsFolder)
                    {
                        Error = DmAddMountPath(Data->Target.Guid, Data->Folder);
                        /* a file system without reparse points answers the request with "invalid function" */
                        if (Error == ERROR_INVALID_FUNCTION)
                        {
                            WCHAR Text[160], Title[64];

                            LoadStringBuffer(IDS_PL_NO_REPARSE, Text, ARRAYSIZE(Text));
                            LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
                            MessageBoxW(hDlg, Text, Title, MB_OK | MB_ICONERROR);
                            Error = ERROR_SUCCESS;
                        }
                    }
                    else if (Change)
                    {
                        /* only the letter that was picked in the list is replaced; the others stay */
                        WCHAR OldRoot[4];

                        OldRoot[0] = Old[0]; OldRoot[1] = L':'; OldRoot[2] = L'\\'; OldRoot[3] = 0;
                        New[0] = Data->Letter; New[1] = L':'; New[2] = L'\\'; New[3] = 0;
                        Error = DmReplaceMountPath(Data->Target.Guid, OldRoot, New);
                    }
                    else
                    {
                        New[0] = Data->Letter; New[1] = L':'; New[2] = L'\\'; New[3] = 0;
                        Error = DmAddMountPath(Data->Target.Guid, New);
                    }
                    if (Error != ERROR_SUCCESS)
                        ShowError(hDlg, Error, NULL);
                    Data->Changed = TRUE;
                    LoadPathList(hDlg, Data);
                    return TRUE;
                }

                case IDC_CL_REMOVE:
                {
                    PWSTR Path;
                    LRESULT Len;
                    DWORD Error;

                    if (Kind == 0)
                        return TRUE;
                    if (Data->Target.Protected)
                    {
                        ShowError(hDlg, DM_ERR_PROTECTED, NULL);
                        return TRUE;
                    }
                    /* a mount path can be longer than MAX_PATH */
                    Len = SendMessageW(hList, LB_GETTEXTLEN, Sel, 0);
                    if (Len == LB_ERR)
                        return TRUE;
                    Path = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)Len + 1) * sizeof(WCHAR));
                    if (Path == NULL)
                        return TRUE;
                    if (SendMessageW(hList, LB_GETTEXT, Sel, (LPARAM)Path) == LB_ERR)
                    {
                        HeapFree(GetProcessHeap(), 0, Path);
                        return TRUE;
                    }
                    if (!Confirm(hDlg, IDS_CONFIRM_REMOVE_PATH, NULL))
                    {
                        HeapFree(GetProcessHeap(), 0, Path);
                        return TRUE;
                    }
                    Error = DmRemoveMountPathOf(Data->Target.Guid, Path);
                    HeapFree(GetProcessHeap(), 0, Path);
                    if (Error != ERROR_SUCCESS)
                        ShowError(hDlg, Error, NULL);
                    Data->Changed = TRUE;
                    LoadPathList(hDlg, Data);
                    return TRUE;
                }

                case IDOK:
                case IDCANCEL:
                    EndDialog(hDlg, Data->Changed);
                    return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

static BOOL
RunLetters(HWND hOwner, const TARGET *Target)
{
    PLETTER_DATA Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LETTER_DATA));
    BOOL Changed;

    if (Data == NULL)
        return FALSE;
    Data->Target = *Target;
    Changed = (BOOL)DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_LETTER), hOwner, LetterProc, (LPARAM)Data);
    HeapFree(GetProcessHeap(), 0, Data);
    return Changed;
}

/* --------------------------------------------------------------------- entry */

BOOL
ActionsRun(HWND hOwner, UINT Id, const DM_DISK *Disk, const DM_REGION *Region)
{
    TARGET Target;
    ULONGLONG ExtOffset = Disk ? Disk->ExtOffset : 0, ExtLength = Disk ? Disk->ExtLength : 0;
    BOOL Changed = FALSE;

    if (Id == ID_RESCAN)
    {
        SetCursor(LoadCursorW(NULL, IDC_WAIT));
        DmRescanDisks();
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;
    }
    if (Disk == NULL || !ActionsEnabled(Id, Disk, Region))
        return FALSE;
    FillTarget(&Target, Disk, Region);

    switch (Id)
    {
        case ID_OPEN:           RunOpen(hOwner, &Target, FALSE); break;
        case ID_EXPLORE:        RunOpen(hOwner, &Target, TRUE); break;
        case ID_MARK_ACTIVE:    Changed = RunMarkActive(hOwner, &Target, Disk); break;
        case ID_CHANGE_LETTER:  Changed = RunLetters(hOwner, &Target); break;
        case ID_FORMAT:         Changed = RunFormatAction(hOwner, &Target); break;
        case ID_DELETE_VOLUME:  Changed = RunDelete(hOwner, &Target, FALSE, 0, 0); break;
        case ID_DELETE_PARTITION: Changed = RunDelete(hOwner, &Target, TRUE, ExtOffset, ExtLength); break;
        case ID_NEW_VOLUME:     Changed = RunNewVolume(hOwner, &Target); break;
        case ID_INIT_DISK:      Changed = RunInitialize(hOwner, Disk); break;
        case ID_CONVERT_GPT:    Changed = RunConvert(hOwner, Disk, DM_STYLE_GPT); break;
        case ID_CONVERT_MBR:    Changed = RunConvert(hOwner, Disk, DM_STYLE_MBR); break;
    }
    return Changed;
}
