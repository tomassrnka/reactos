/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The dialogs of Disk Management: initialize, format, drive letters and paths, shrink
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Layout and wording follow the dialogs of Disk Management on Windows Server 2008 R2.
 */

#include "uicommon.h"
#include <shlobj.h>

/* ----------------------------------------------------------------- initialize */

typedef struct _INIT_DATA
{
    PDM_MODEL Model;
    const DM_DISK *Only;
    ULONG Numbers[64];
    DM_DISK_ID Ids[64];
    ULONG Count;
    BOOL Changed;
} INIT_DATA, *PINIT_DATA;

static INT_PTR CALLBACK
InitProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PINIT_DATA Data = (PINIT_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            HWND hList = GetDlgItem(hDlg, IDC_ID_LIST);
            LVCOLUMNW Column;
            RECT rc;
            ULONG i;

            Data = (PINIT_DATA)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES);
            GetClientRect(hList, &rc);
            ZeroMemory(&Column, sizeof(Column));
            Column.mask = LVCF_WIDTH;
            Column.cx = rc.right;
            SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&Column);

            for (i = 0; i < Data->Model->DiskCount && Data->Count < ARRAYSIZE(Data->Numbers); i++)
            {
                const DM_DISK *Disk = Data->Model->Disks[i];
                WCHAR Text[32];
                LVITEMW Item;

                if (!DiskNeedsInit(Disk) || (Data->Only && Data->Only != Disk))
                    continue;
                StringCchPrintfW(Text, ARRAYSIZE(Text), L"Disk %u", Disk->Number);
                ZeroMemory(&Item, sizeof(Item));
                Item.mask = LVIF_TEXT;
                Item.iItem = (int)Data->Count;
                Item.pszText = Text;
                SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&Item);
                ListView_SetCheckState(hList, Data->Count, TRUE);
                DiskIdOf(Disk, &Data->Ids[Data->Count]);
                Data->Numbers[Data->Count++] = Disk->Number;
            }
            CheckRadioButton(hDlg, IDC_ID_MBR, IDC_ID_GPT, IDC_ID_MBR);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
            {
                HWND hList = GetDlgItem(hDlg, IDC_ID_LIST);
                DM_STYLE Style = IsDlgButtonChecked(hDlg, IDC_ID_GPT) == BST_CHECKED ? DM_STYLE_GPT : DM_STYLE_MBR;
                ULONG i;

                SetCursor(LoadCursorW(NULL, IDC_WAIT));
                for (i = 0; i < Data->Count; i++)
                {
                    DWORD Error;

                    if (!ListView_GetCheckState(hList, i))
                        continue;
                    Error = DmInitializeDisk(Data->Numbers[i], Style, &Data->Ids[i]);
                    if (Error != ERROR_SUCCESS)
                        ShowError(hDlg, Error, NULL);
                    Data->Changed = TRUE;
                }
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
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

/* Shows the disks that need a partition style; with Only, just that one. */
BOOL
DlgInitialize(HWND hOwner, PDM_MODEL Model, const DM_DISK *Only)
{
    INIT_DATA Data;
    ULONG i;
    BOOL Any = FALSE;

    ZeroMemory(&Data, sizeof(Data));
    Data.Model = Model;
    Data.Only = Only;
    for (i = 0; Model && i < Model->DiskCount; i++)
        if (DiskNeedsInit(Model->Disks[i]) && (!Only || Only == Model->Disks[i]))
            Any = TRUE;
    if (!Any)
        return FALSE;
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_INITDISK), hOwner, InitProc, (LPARAM)&Data);
    return Data.Changed;
}

/* --------------------------------------------------------------------- format */

typedef struct _FORMAT_DATA
{
    TARGET Target;
    DM_FS_LIST Fs;
    ULONGLONG Bytes;
    BOOL Changed;
} FORMAT_DATA, *PFORMAT_DATA;

static VOID
FormatFsChanged(HWND hDlg, PFORMAT_DATA Data)
{
    WCHAR Fs[DM_FS_NAME] = L"";
    int Index = (int)SendDlgItemMessageW(hDlg, IDC_FM_FS, CB_GETCURSEL, 0, 0);

    if (Index >= 0)
        SendDlgItemMessageW(hDlg, IDC_FM_FS, CB_GETLBTEXT, Index, (LPARAM)Fs);
    FillClusterCombo(GetDlgItem(hDlg, IDC_FM_CLUSTER), Fs, Data->Bytes);
    SendDlgItemMessageW(hDlg, IDC_FM_LABEL, EM_LIMITTEXT,
                        (_wcsicmp(Fs, L"FAT") == 0 || _wcsicmp(Fs, L"FAT32") == 0) ? 11 : 32, 0);
}

static INT_PTR CALLBACK
FormatProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PFORMAT_DATA Data = (PFORMAT_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Title[64];

            Data = (PFORMAT_DATA)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            if (Data->Target.Letter[0])
                StringCchPrintfW(Title, ARRAYSIZE(Title), L"Format %s", Data->Target.Letter);
            else
                StringCchCopyW(Title, ARRAYSIZE(Title), L"Format");
            SetWindowTextW(hDlg, Title);
            SetDlgItemTextW(hDlg, IDC_FM_LABEL, Data->Target.Label);
            FillFsCombo(GetDlgItem(hDlg, IDC_FM_FS), &Data->Fs, Data->Target.FileSystem, Data->Bytes);
            FormatFsChanged(hDlg, Data);
            CheckDlgButton(hDlg, IDC_FM_QUICK, BST_CHECKED);
            /* the file systems of ReactOS keep no compression flag */
            EnableWindow(GetDlgItem(hDlg, IDC_FM_COMPRESS), FALSE);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_FM_FS && HIWORD(wParam) == CBN_SELCHANGE)
            {
                FormatFsChanged(hDlg, Data);
                return TRUE;
            }
            if (LOWORD(wParam) == IDOK)
            {
                WCHAR Fs[DM_FS_NAME], Label[64], Caption[64];
                int Index = (int)SendDlgItemMessageW(hDlg, IDC_FM_FS, CB_GETCURSEL, 0, 0);
                ULONG Cluster = ComboCluster(hDlg, IDC_FM_CLUSTER);
                BOOL Quick = IsDlgButtonChecked(hDlg, IDC_FM_QUICK) == BST_CHECKED;
                DWORD Error;

                if (Index < 0)
                    return TRUE;
                SendDlgItemMessageW(hDlg, IDC_FM_FS, CB_GETLBTEXT, Index, (LPARAM)Fs);
                GetDlgItemTextW(hDlg, IDC_FM_LABEL, Label, ARRAYSIZE(Label));
                GetWindowTextW(hDlg, Caption, ARRAYSIZE(Caption));

                {
                    WCHAR Text[512];

                    LoadStringBuffer(IDS_CONFIRM_FORMAT, Text, ARRAYSIZE(Text));
                    /* Windows offers OK and Cancel here, not Yes and No */
                    if (MessageBoxW(hDlg, Text, Caption, MB_OKCANCEL | MB_ICONWARNING) != IDOK)
                        return TRUE;
                }
                Error = RunFormat(hDlg, Data->Target.Guid, Fs, Label, Quick, Cluster, Data->Target.Removable);
                if (Error == ERROR_SUCCESS && Data->Target.Style == DM_STYLE_MBR)
                    TypeFix(hDlg, Data->Target.Disk, Data->Target.Offset, Data->Target.Guid);
                Data->Changed = TRUE;
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

BOOL
DlgFormat(HWND hOwner, const TARGET *Target)
{
    FORMAT_DATA Data;

    ZeroMemory(&Data, sizeof(Data));
    Data.Target = *Target;
    Data.Bytes = Target->Length;
    if (!DmQueryFileSystems(&Data.Fs))
    {
        ShowError(hOwner, DM_ERR_NO_PROVIDER, NULL);
        return FALSE;
    }
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_FORMAT), hOwner, FormatProc, (LPARAM)&Data);
    return Data.Changed;
}

/* ------------------------------------------------- drive letter and paths */

/* the list of paths of a volume holds up to 511 characters, so a path always fits */
#define PATHBUF 520

typedef struct _LETTER_DATA
{
    TARGET Target;
    BOOL Changed;
    BOOL ChangeMode;            /* the dialog changes an existing path */
    BOOL HasLetter;             /* the volume has a drive letter */
    BOOL IsFolder;              /* out: a folder was entered */
    WCHAR Letter;               /* out */
    WCHAR Folder[MAX_PATH];     /* in and out */
    WCHAR Current[PATHBUF];     /* the path being changed, without the last backslash */
    WCHAR Name[128];
} LETTER_DATA, *PLETTER_DATA;

static VOID
PickUpdate(HWND hDlg, PLETTER_DATA Data)
{
    BOOL Folder = IsDlgButtonChecked(hDlg, IDC_PL_FOLDER_RADIO) == BST_CHECKED;

    EnableWindow(GetDlgItem(hDlg, IDC_PL_LETTER), !Folder && IsWindowEnabled(GetDlgItem(hDlg, IDC_PL_LETTER_RADIO)));
    EnableWindow(GetDlgItem(hDlg, IDC_PL_FOLDER), Folder);
    EnableWindow(GetDlgItem(hDlg, IDC_PL_BROWSE), Folder);
    if (Data->ChangeMode)
    {
        BOOL Differs;

        if (Folder)
        {
            WCHAR Text[MAX_PATH];

            GetDlgItemTextW(hDlg, IDC_PL_FOLDER, Text, ARRAYSIZE(Text));
            Differs = Text[0] != 0 && _wcsicmp(Text, Data->Current) != 0;
        }
        else
        {
            Differs = ComboLetter(hDlg, IDC_PL_LETTER) != (WCHAR)towupper(Data->Current[0]);
        }
        EnableWindow(GetDlgItem(hDlg, IDOK), Differs);
    }
}

static INT_PTR CALLBACK
PickProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PLETTER_DATA Data = (PLETTER_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Format[96], Text[200];
            BOOL CurrentIsFolder;

            Data = (PLETTER_DATA)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            CurrentIsFolder = Data->ChangeMode && wcslen(Data->Current) > 2;
            SetWindowTextW(hDlg, Data->ChangeMode ? L"Change Drive Letter or Path" : L"Add Drive Letter or Path");
            LoadStringBuffer(Data->ChangeMode ? IDS_PL_CHANGE_TEXT : IDS_PL_ADD_TEXT, Format, ARRAYSIZE(Format));
            StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Data->Name);
            SetDlgItemTextW(hDlg, IDC_PL_TEXT, Text);
            FillLetterCombo(GetDlgItem(hDlg, IDC_PL_LETTER), (Data->ChangeMode && !CurrentIsFolder) ? (WCHAR)towupper(Data->Current[0]) : 0);
            if (Data->ChangeMode)
            {
                /* a letter is changed to a letter, a folder to a folder */
                EnableWindow(GetDlgItem(hDlg, IDC_PL_LETTER_RADIO), !CurrentIsFolder);
                EnableWindow(GetDlgItem(hDlg, IDC_PL_FOLDER_RADIO), CurrentIsFolder);
                CheckRadioButton(hDlg, IDC_PL_LETTER_RADIO, IDC_PL_FOLDER_RADIO, CurrentIsFolder ? IDC_PL_FOLDER_RADIO : IDC_PL_LETTER_RADIO);
                if (CurrentIsFolder)
                    SetDlgItemTextW(hDlg, IDC_PL_FOLDER, Data->Current);
            }
            else
            {
                /* a volume that has a letter takes more paths, not another letter */
                EnableWindow(GetDlgItem(hDlg, IDC_PL_LETTER_RADIO), !Data->HasLetter);
                CheckRadioButton(hDlg, IDC_PL_LETTER_RADIO, IDC_PL_FOLDER_RADIO, Data->HasLetter ? IDC_PL_FOLDER_RADIO : IDC_PL_LETTER_RADIO);
            }
            PickUpdate(hDlg, Data);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_PL_LETTER_RADIO:
                case IDC_PL_FOLDER_RADIO:
                case IDC_PL_LETTER:
                    PickUpdate(hDlg, Data);
                    return TRUE;

                case IDC_PL_FOLDER:
                    if (HIWORD(wParam) == EN_CHANGE)
                        PickUpdate(hDlg, Data);
                    return TRUE;

                case IDC_PL_BROWSE:
                {
                    BROWSEINFOW Info;
                    PIDLIST_ABSOLUTE Item;
                    WCHAR Path[MAX_PATH], Title[96];

                    ZeroMemory(&Info, sizeof(Info));
                    LoadStringBuffer(IDS_PL_BROWSE, Title, ARRAYSIZE(Title));
                    Info.hwndOwner = hDlg;
                    Info.lpszTitle = Title;
                    Info.ulFlags = BIF_RETURNONLYFSDIRS;
                    Item = SHBrowseForFolderW(&Info);
                    if (Item)
                    {
                        if (SHGetPathFromIDListW(Item, Path))
                            SetDlgItemTextW(hDlg, IDC_PL_FOLDER, Path);
                        CoTaskMemFree(Item);
                    }
                    return TRUE;
                }

                case IDOK:
                    if (IsDlgButtonChecked(hDlg, IDC_PL_FOLDER_RADIO) == BST_CHECKED)
                    {
                        SIZE_T Len;

                        /* a path that does not fit whole must not be cut to another folder */
                        if (GetWindowTextLengthW(GetDlgItem(hDlg, IDC_PL_FOLDER)) >= (int)ARRAYSIZE(Data->Folder) - 2)
                        {
                            FormatMessageBox(hDlg, IDS_PL_BAD_FOLDER, MB_ICONERROR, NULL);
                            return TRUE;
                        }
                        GetDlgItemTextW(hDlg, IDC_PL_FOLDER, Data->Folder, ARRAYSIZE(Data->Folder));
                        Len = wcslen(Data->Folder);
                        if (Len >= 3 && Data->Folder[Len - 1] != L'\\' && Len + 1 < ARRAYSIZE(Data->Folder))
                        {
                            Data->Folder[Len++] = L'\\';
                            Data->Folder[Len] = 0;
                        }
                        if (Len < 3 || Data->Folder[1] != L':' || Data->Folder[Len - 1] != L'\\')
                        {
                            FormatMessageBox(hDlg, IDS_PL_BAD_FOLDER, MB_ICONERROR, NULL);
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
                            FormatMessageBox(hDlg, IDS_PL_NO_LETTER_FREE, MB_ICONERROR, NULL);
                            return TRUE;
                        }
                    }
                    EndDialog(hDlg, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

static VOID
LoadPathList(HWND hDlg, PLETTER_DATA Data)
{
    WCHAR Paths[512], Shown[PATHBUF];
    PCWSTR Path;
    HWND hList = GetDlgItem(hDlg, IDC_CL_LIST);
    SIZE_T Len;

    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    CurrentPaths(Data->Target.Guid, Paths, ARRAYSIZE(Paths));
    Data->HasLetter = FALSE;
    for (Path = Paths; *Path; Path += wcslen(Path) + 1)
    {
        int Index;

        if (IsLetterPath(Path))
            Data->HasLetter = TRUE;
        StringCchCopyW(Shown, ARRAYSIZE(Shown), Path);
        Len = wcslen(Shown);
        /* Windows shows "J:" and "Q:\\mnt", without the closing backslash */
        if (Len > 0 && Shown[Len - 1] == L'\\')
            Shown[Len - 1] = 0;
        Index = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)Shown);
        SendMessageW(hList, LB_SETITEMDATA, Index, IsLetterPath(Path) ? 1 : 2);
    }
    if (SendMessageW(hList, LB_GETCOUNT, 0, 0) > 0)
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
            WCHAR Text[200];

            Data = (PLETTER_DATA)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"Change Drive Letter and Paths for %s", Data->Name);
            SetWindowTextW(hDlg, Text);
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
                        EnableWindow(GetDlgItem(hDlg, IDC_CL_CHANGE), Kind != 0);
                        EnableWindow(GetDlgItem(hDlg, IDC_CL_REMOVE), Kind != 0);
                    }
                    return TRUE;

                case IDC_CL_ADD:
                case IDC_CL_CHANGE:
                {
                    BOOL Change = (LOWORD(wParam) == IDC_CL_CHANGE);
                    WCHAR Paths[512], Old[PATHBUF], New[8], OldRoot[PATHBUF];
                    DWORD Error;

                    if (Data->Target.Protected)
                    {
                        ShowError(hDlg, DM_ERR_PROTECTED, NULL);
                        return TRUE;
                    }
                    Data->ChangeMode = Change;
                    Data->Current[0] = 0;
                    if (Change)
                        SendMessageW(hList, LB_GETTEXT, Sel, (LPARAM)Data->Current);
                    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_PICKLETTER), hDlg, PickProc, (LPARAM)Data) != IDOK)
                        return TRUE;
                    if (Change && !ConfirmBox(hDlg, IDS_CONFIRM_TITLE, IDS_CONFIRM_LETTER, NULL))
                        return TRUE;
                    CurrentPaths(Data->Target.Guid, Paths, ARRAYSIZE(Paths));
                    if (Change)
                    {
                        StringCchCopyW(Old, ARRAYSIZE(Old), Data->Current);
                        StringCchPrintfW(OldRoot, ARRAYSIZE(OldRoot), L"%s\\", Old);
                    }
                    if (Data->IsFolder)
                    {
                        /* the old path is taken away and put back when the new one cannot be made */
                        if (Change)
                            Error = DmReplaceMountPath(Data->Target.Guid, OldRoot, Data->Folder);
                        else
                            Error = DmAddMountPath(Data->Target.Guid, Data->Folder);
                    }
                    else if (Change)
                    {
                        /* only the letter that was picked in the list is replaced; the others stay */
                        WCHAR NewRoot[4];

                        NewRoot[0] = Data->Letter; NewRoot[1] = L':'; NewRoot[2] = L'\\'; NewRoot[3] = 0;
                        Error = DmReplaceMountPath(Data->Target.Guid, OldRoot, NewRoot);
                    }
                    else
                    {
                        New[0] = Data->Letter; New[1] = L':'; New[2] = L'\\'; New[3] = 0;
                        Error = DmAddMountPath(Data->Target.Guid, New);
                    }
                    if (Error == ERROR_INVALID_FUNCTION && Data->IsFolder)
                    {
                        /* a file system without reparse points answers the request with "invalid function" */
                        FormatMessageBox(hDlg, IDS_PL_NO_REPARSE, MB_ICONERROR, NULL);
                        Error = ERROR_SUCCESS;
                    }
                    if (Error != ERROR_SUCCESS)
                        ShowError(hDlg, Error, NULL);
                    Data->Changed = TRUE;
                    LoadPathList(hDlg, Data);
                    return TRUE;
                }

                case IDC_CL_REMOVE:
                {
                    WCHAR Path[PATHBUF];
                    DWORD Error;

                    if (Kind == 0)
                        return TRUE;
                    if (Data->Target.Protected)
                    {
                        ShowError(hDlg, DM_ERR_PROTECTED, NULL);
                        return TRUE;
                    }
                    if (SendMessageW(hList, LB_GETTEXTLEN, Sel, 0) >= PATHBUF - 2 ||
                        SendMessageW(hList, LB_GETTEXT, Sel, (LPARAM)Path) == LB_ERR)
                        return TRUE;
                    if (!ConfirmBox(hDlg, IDS_CONFIRM_TITLE, IDS_CONFIRM_REMOVE_PATH, NULL))
                        return TRUE;
                    if (FAILED(StringCchCatW(Path, ARRAYSIZE(Path), L"\\")))
                        return TRUE;
                    Error = DmRemoveMountPathOf(Data->Target.Guid, Path);
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

BOOL
DlgLetters(HWND hOwner, const TARGET *Target)
{
    PLETTER_DATA Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LETTER_DATA));
    BOOL Changed;

    if (Data == NULL)
        return FALSE;
    Data->Target = *Target;
    VolumeDisplayName(Target, Data->Name, ARRAYSIZE(Data->Name));
    Changed = (BOOL)DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_LETTER), hOwner, LetterProc, (LPARAM)Data);
    HeapFree(GetProcessHeap(), 0, Data);
    return Changed;
}

/* --------------------------------------------------------------------- shrink */

typedef struct _SHRINK_DATA
{
    TARGET Target;
    ULONGLONG TotalMB;
    ULONGLONG MaxMB;
    BOOL Changed;
    DWORD Error;
    ULONGLONG TotalBytes, MaxBytes;
} SHRINK_DATA, *PSHRINK_DATA;

static VOID
ShrinkShow(HWND hDlg, PSHRINK_DATA Data)
{
    WCHAR Text[32];
    BOOL Ok;
    ULONGLONG Amount;

    Amount = 0;
    Ok = GetWindowTextLengthW(GetDlgItem(hDlg, IDC_SH_AMOUNT)) < (int)ARRAYSIZE(Text);
    GetDlgItemTextW(hDlg, IDC_SH_AMOUNT, Text, ARRAYSIZE(Text));
    Ok = Ok && ParseNumber(Text, 1, Data->MaxMB, &Amount);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Ok ? Data->TotalMB - Amount : Data->TotalMB);
    SetDlgItemTextW(hDlg, IDC_SH_AFTER, Text);
    EnableWindow(GetDlgItem(hDlg, IDOK), Ok);
}

static INT_PTR CALLBACK
ShrinkProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PSHRINK_DATA Data = (PSHRINK_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Text[64];
            HICON Icon = LoadIconW(NULL, IDI_INFORMATION);

            Data = (PSHRINK_DATA)lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"Shrink %s", Data->Target.Letter[0] ? Data->Target.Letter : L"Volume");
            SetWindowTextW(hDlg, Text);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Data->TotalMB);
            SetDlgItemTextW(hDlg, IDC_SH_BEFORE, Text);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Data->MaxMB);
            SetDlgItemTextW(hDlg, IDC_SH_AVAIL, Text);
            SetDlgItemTextW(hDlg, IDC_SH_AMOUNT, Text);
            SendDlgItemMessageW(hDlg, IDC_SH_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(hDlg, IDC_SH_AMOUNT), 0);
            SendDlgItemMessageW(hDlg, IDC_SH_SPIN, UDM_SETRANGE32, 1, (LPARAM)Data->MaxMB);
            SendDlgItemMessageW(hDlg, IDC_SH_SPIN, UDM_SETPOS32, 0, (LPARAM)Data->MaxMB);
            SendDlgItemMessageW(hDlg, IDC_SH_ICON, STM_SETICON, (WPARAM)Icon, 0);
            ShrinkShow(hDlg, Data);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_SH_AMOUNT && HIWORD(wParam) == EN_CHANGE)
            {
                ShrinkShow(hDlg, Data);
                return TRUE;
            }
            if (LOWORD(wParam) == IDOK)
            {
                WCHAR Text[32];
                ULONGLONG Amount;
                DWORD Error;

                if (GetWindowTextLengthW(GetDlgItem(hDlg, IDC_SH_AMOUNT)) >= (int)ARRAYSIZE(Text))
                    return TRUE;
                GetDlgItemTextW(hDlg, IDC_SH_AMOUNT, Text, ARRAYSIZE(Text));
                if (!ParseNumber(Text, 1, Data->MaxMB, &Amount))
                    return TRUE;
                SetCursor(LoadCursorW(NULL, IDC_WAIT));
                Error = DmShrinkVolume(Data->Target.Disk, Data->Target.Offset, Data->Target.Length, Data->Target.Guid, Amount * DM_MIB, &Data->Target.Id);
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
                if (Error != ERROR_SUCCESS)
                    ShowError(hDlg, Error, NULL);
                Data->Changed = TRUE;
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

static DWORD
ShrinkQuery(PVOID Param)
{
    PSHRINK_DATA Data = Param;

    Data->Error = DmQueryShrink(Data->Target.Disk, Data->Target.Offset, Data->Target.Guid, &Data->TotalBytes, &Data->MaxBytes);
    return Data->Error;
}

BOOL
DlgShrink(HWND hOwner, const TARGET *Target)
{
    SHRINK_DATA Data;
    WCHAR Text[64];

    ZeroMemory(&Data, sizeof(Data));
    Data.Target = *Target;
    LoadStringBuffer(IDS_SH_QUERYING, Text, ARRAYSIZE(Text));
    /* the result of the worker, or the reason it did not run */
    Data.Error = RunBusy(hOwner, Text, FALSE, ShrinkQuery, &Data, NULL);
    if (Data.Error != ERROR_SUCCESS)
    {
        ShowError(hOwner, Data.Error, NULL);
        return FALSE;
    }
    Data.TotalMB = Data.TotalBytes / DM_MIB;
    Data.MaxMB = Data.MaxBytes / DM_MIB;
    if (Data.MaxMB == 0)
    {
        FormatMessageBox(hOwner, IDS_SH_NOTHING, MB_ICONINFORMATION, NULL);
        return FALSE;
    }
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SHRINK), hOwner, ShrinkProc, (LPARAM)&Data);
    return Data.Changed;
}
