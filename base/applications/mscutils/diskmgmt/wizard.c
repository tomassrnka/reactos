/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The New Simple Volume wizard and the Extend Volume wizard
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Pages, texts and defaults follow the wizards of Disk Management on Windows Server 2008 R2.
 */

#include "uicommon.h"
#include <shlobj.h>

#define MIN_VOLUME_MB   8

/* ------------------------------------------------------------------- helpers */

static HFONT
CreateTitleFont(HWND hDlg)
{
    LOGFONTW Font;
    HFONT hOld = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);

    ZeroMemory(&Font, sizeof(Font));
    if (hOld == NULL || GetObjectW(hOld, sizeof(Font), &Font) == 0)
        StringCchCopyW(Font.lfFaceName, ARRAYSIZE(Font.lfFaceName), L"MS Shell Dlg");
    Font.lfHeight = -17;
    Font.lfWidth = 0;
    Font.lfWeight = FW_BOLD;
    return CreateFontIndirectW(&Font);
}

static VOID
SetPageTitleFont(HWND hDlg, UINT Id, HFONT *Font)
{
    if (*Font == NULL)
        *Font = CreateTitleFont(hDlg);
    if (*Font)
        SendDlgItemMessageW(hDlg, Id, WM_SETFONT, (WPARAM)*Font, TRUE);
}

static VOID
InitPage(PROPSHEETPAGEW *Page, UINT Template, DLGPROC Proc, LPARAM Data, PCWSTR Caption, UINT TitleId, UINT SubtitleId,
         PWSTR Buffer)
{
    ZeroMemory(Page, sizeof(*Page));
    Page->dwSize = sizeof(*Page);
    Page->hInstance = g_hInst;
    Page->pszTemplate = MAKEINTRESOURCEW(Template);
    Page->pfnDlgProc = Proc;
    Page->lParam = Data;
    /* every page repeats the caption of the wizard, or the sheet falls back to a generic one */
    Page->pszTitle = Caption;
    if (TitleId)
    {
        /* an interior page: the header of the sheet shows the title and the subtitle */
        Page->dwFlags = PSP_USETITLE | PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
        LoadStringBuffer(TitleId, Buffer, 64);
        Page->pszHeaderTitle = Buffer;
        LoadStringBuffer(SubtitleId, Buffer + 64, 160);
        Page->pszHeaderSubTitle = Buffer + 64;
    }
    else
    {
        Page->dwFlags = PSP_USETITLE | PSP_HIDEHEADER;
    }
}

static VOID
RunWizard(HWND hOwner, PROPSHEETPAGEW *Pages, UINT Count, WCHAR *Caption)
{
    PROPSHEETHEADERW Header;

    ZeroMemory(&Header, sizeof(Header));
    Header.dwSize = sizeof(Header);
    Header.dwFlags = PSH_WIZARD97 | PSH_HEADER | PSH_WATERMARK | PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
    Header.hwndParent = hOwner;
    Header.hInstance = g_hInst;
    Header.pszCaption = Caption;
    Header.nPages = Count;
    Header.ppsp = Pages;
    Header.pszbmWatermark = MAKEINTRESOURCEW(IDB_WIZ_WATERMARK);
    Header.pszbmHeader = MAKEINTRESOURCEW(IDB_WIZ_HEADER);
    PropertySheetW(&Header);
}

static VOID
StopPage(HWND hDlg, BOOL Stay)
{
    SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, Stay ? -1 : 0);
}

static BOOL
ParseSize(HWND hDlg, UINT Id, ULONGLONG Min, ULONGLONG Max, PULONGLONG Value)
{
    WCHAR Text[32];

    /* a text longer than the buffer is not a size, and must not be cut to one */
    if (GetWindowTextLengthW(GetDlgItem(hDlg, Id)) >= (int)ARRAYSIZE(Text))
        return FALSE;
    GetDlgItemTextW(hDlg, Id, Text, ARRAYSIZE(Text));
    return ParseNumber(Text, Min, Max, Value);
}

/* Windows enables Next only while the size is a number inside the range, and says nothing else. */
static VOID
UpdateNext(HWND hDlg, UINT EditId, ULONGLONG Min, ULONGLONG Max)
{
    ULONGLONG Value;

    PropSheet_SetWizButtons(GetParent(hDlg), ParseSize(hDlg, EditId, Min, Max, &Value) ? (PSWIZB_BACK | PSWIZB_NEXT) : PSWIZB_BACK);
}

static VOID
SizeRangeMessage(HWND hDlg, ULONGLONG Min, ULONGLONG Max)
{
    WCHAR Format[160], Text[200], Title[64];

    LoadStringBuffer(IDS_NV_SIZE_RANGE, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Min, Max);
    LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
    MessageBoxW(hDlg, Text, Title, MB_OK | MB_ICONERROR);
}

/* ============================================================== new simple volume */

typedef struct _NSV
{
    TARGET Target;
    PDL_LAYOUT Planned;         /* the table the plan was made on */
    DL_FREE Free;
    BOOL NeedsExtended;
    DM_FS_LIST FsList;
    ULONGLONG MaxMB;
    ULONGLONG SizeMB;
    PATH_CHOICE Choice;
    WCHAR Letter;
    WCHAR Folder[MAX_PATH];
    BOOL DoFormat;
    WCHAR Fs[DM_FS_NAME];
    ULONG Cluster;
    WCHAR Label[64];
    BOOL Quick;
    BOOL Changed;
    ULONGLONG FormatForMB;      /* the size the choices of the format page were made for */
    HFONT hTitleFont;
} NSV, *PNSV;

static PNSV
PageData(HWND hDlg)
{
    return (PNSV)GetWindowLongPtrW(hDlg, DWLP_USER);
}

static INT_PTR CALLBACK
NsvWelcomeProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PNSV Data = (PNSV)((LPPROPSHEETPAGEW)lParam)->lParam;

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            SetPageTitleFont(hDlg, IDC_NVW_TITLE, &Data->hTitleFont);
            return TRUE;
        }
        case WM_NOTIFY:
            if (((NMHDR *)lParam)->code == PSN_SETACTIVE)
            {
                PropSheet_SetWizButtons(GetParent(hDlg), PSWIZB_NEXT);
                StopPage(hDlg, FALSE);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static INT_PTR CALLBACK
NsvSizeProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PNSV Data = PageData(hDlg);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Text[32];

            Data = (PNSV)((LPPROPSHEETPAGEW)lParam)->lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Data->MaxMB);
            SetDlgItemTextW(hDlg, IDC_NV_MAX, Text);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%u", MIN_VOLUME_MB);
            SetDlgItemTextW(hDlg, IDC_NV_MIN, Text);
            SendDlgItemMessageW(hDlg, IDC_NV_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(hDlg, IDC_NV_SIZE), 0);
            SendDlgItemMessageW(hDlg, IDC_NV_SPIN, UDM_SETRANGE32, MIN_VOLUME_MB, (LPARAM)Data->MaxMB);
            SendDlgItemMessageW(hDlg, IDC_NV_SPIN, UDM_SETPOS32, 0, (LPARAM)Data->MaxMB);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_NV_SIZE && HIWORD(wParam) == EN_CHANGE)
            {
                UpdateNext(hDlg, IDC_NV_SIZE, MIN_VOLUME_MB, Data->MaxMB);
                return TRUE;
            }
            break;
        case WM_NOTIFY:
            switch (((NMHDR *)lParam)->code)
            {
                case PSN_SETACTIVE:
                    UpdateNext(hDlg, IDC_NV_SIZE, MIN_VOLUME_MB, Data->MaxMB);
                    StopPage(hDlg, FALSE);
                    return TRUE;
                case PSN_WIZNEXT:
                {
                    ULONGLONG Size;

                    if (!ParseSize(hDlg, IDC_NV_SIZE, MIN_VOLUME_MB, Data->MaxMB, &Size))
                    {
                        SizeRangeMessage(hDlg, MIN_VOLUME_MB, Data->MaxMB);
                        StopPage(hDlg, TRUE);
                        return TRUE;
                    }
                    Data->SizeMB = Size;
                    StopPage(hDlg, FALSE);
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

static VOID
LetterUpdate(HWND hDlg)
{
    BOOL Letter = IsDlgButtonChecked(hDlg, IDC_NL_RADIO_LETTER) == BST_CHECKED;
    BOOL Folder = IsDlgButtonChecked(hDlg, IDC_NL_RADIO_FOLDER) == BST_CHECKED;

    EnableWindow(GetDlgItem(hDlg, IDC_NL_LETTER), Letter);
    EnableWindow(GetDlgItem(hDlg, IDC_NL_FOLDER), Folder);
    EnableWindow(GetDlgItem(hDlg, IDC_NL_BROWSE), Folder);
}

static INT_PTR CALLBACK
NsvLetterProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PNSV Data = PageData(hDlg);

    switch (Message)
    {
        case WM_INITDIALOG:
            Data = (PNSV)((LPPROPSHEETPAGEW)lParam)->lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            FillLetterCombo(GetDlgItem(hDlg, IDC_NL_LETTER), 0);
            CheckRadioButton(hDlg, IDC_NL_RADIO_LETTER, IDC_NL_RADIO_NONE, IDC_NL_RADIO_LETTER);
            LetterUpdate(hDlg);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_NL_RADIO_LETTER:
                case IDC_NL_RADIO_FOLDER:
                case IDC_NL_RADIO_NONE:
                    LetterUpdate(hDlg);
                    return TRUE;
                case IDC_NL_BROWSE:
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
                            SetDlgItemTextW(hDlg, IDC_NL_FOLDER, Path);
                        CoTaskMemFree(Item);
                    }
                    return TRUE;
                }
            }
            break;

        case WM_NOTIFY:
            switch (((NMHDR *)lParam)->code)
            {
                case PSN_SETACTIVE:
                    PropSheet_SetWizButtons(GetParent(hDlg), PSWIZB_BACK | PSWIZB_NEXT);
                    StopPage(hDlg, FALSE);
                    return TRUE;
                case PSN_WIZNEXT:
                    if (IsDlgButtonChecked(hDlg, IDC_NL_RADIO_FOLDER) == BST_CHECKED)
                    {
                        SIZE_T Len;

                        /* a path that does not fit whole must not be cut to another folder */
                        if (GetWindowTextLengthW(GetDlgItem(hDlg, IDC_NL_FOLDER)) >= (int)ARRAYSIZE(Data->Folder) - 2)
                        {
                            FormatMessageBox(hDlg, IDS_PL_BAD_FOLDER, MB_ICONERROR, NULL);
                            StopPage(hDlg, TRUE);
                            return TRUE;
                        }
                        GetDlgItemTextW(hDlg, IDC_NL_FOLDER, Data->Folder, ARRAYSIZE(Data->Folder));
                        Len = wcslen(Data->Folder);
                        if (Len >= 3 && Data->Folder[Len - 1] != L'\\' && Len + 1 < ARRAYSIZE(Data->Folder))
                        {
                            Data->Folder[Len++] = L'\\';
                            Data->Folder[Len] = 0;
                        }
                        if (Len < 3 || Data->Folder[1] != L':' || Data->Folder[Len - 1] != L'\\')
                        {
                            FormatMessageBox(hDlg, IDS_PL_BAD_FOLDER, MB_ICONERROR, NULL);
                            StopPage(hDlg, TRUE);
                            return TRUE;
                        }
                        Data->Choice = PATH_FOLDER;
                    }
                    else if (IsDlgButtonChecked(hDlg, IDC_NL_RADIO_NONE) == BST_CHECKED)
                    {
                        Data->Choice = PATH_NONE;
                    }
                    else
                    {
                        Data->Letter = ComboLetter(hDlg, IDC_NL_LETTER);
                        if (Data->Letter == 0)
                        {
                            FormatMessageBox(hDlg, IDS_PL_NO_LETTER_FREE, MB_ICONERROR, NULL);
                            StopPage(hDlg, TRUE);
                            return TRUE;
                        }
                        Data->Choice = PATH_LETTER;
                    }
                    StopPage(hDlg, FALSE);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

static VOID
FormatUpdate(HWND hDlg, PNSV Data)
{
    BOOL Yes = IsDlgButtonChecked(hDlg, IDC_NF_RADIO_YES) == BST_CHECKED;
    WCHAR Fs[DM_FS_NAME] = L"";
    int Index = (int)SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETCURSEL, 0, 0);

    if (Index >= 0)
        SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETLBTEXT, Index, (LPARAM)Fs);
    EnableWindow(GetDlgItem(hDlg, IDC_NF_FS), Yes);
    EnableWindow(GetDlgItem(hDlg, IDC_NF_CLUSTER), Yes);
    EnableWindow(GetDlgItem(hDlg, IDC_NF_LABEL), Yes);
    EnableWindow(GetDlgItem(hDlg, IDC_NF_QUICK), Yes);
    /* the file systems of ReactOS keep no compression flag */
    EnableWindow(GetDlgItem(hDlg, IDC_NF_COMPRESS), FALSE);
    SendDlgItemMessageW(hDlg, IDC_NF_LABEL, EM_LIMITTEXT,
                        (_wcsicmp(Fs, L"FAT") == 0 || _wcsicmp(Fs, L"FAT32") == 0) ? 11 : 32, 0);
    UNREFERENCED_PARAMETER(Data);
}

static INT_PTR CALLBACK
NsvFormatProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PNSV Data = PageData(hDlg);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Label[64];

            Data = (PNSV)((LPPROPSHEETPAGEW)lParam)->lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            LoadStringBuffer(IDS_NV_LABEL_DEFAULT, Label, ARRAYSIZE(Label));
            SetDlgItemTextW(hDlg, IDC_NF_LABEL, Label);
            CheckRadioButton(hDlg, IDC_NF_RADIO_NO, IDC_NF_RADIO_YES, Data->FsList.Count ? IDC_NF_RADIO_YES : IDC_NF_RADIO_NO);
            CheckDlgButton(hDlg, IDC_NF_QUICK, BST_CHECKED);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_NF_RADIO_NO || LOWORD(wParam) == IDC_NF_RADIO_YES)
            {
                FormatUpdate(hDlg, Data);
                return TRUE;
            }
            if (LOWORD(wParam) == IDC_NF_FS && HIWORD(wParam) == CBN_SELCHANGE)
            {
                WCHAR Fs[DM_FS_NAME] = L"";
                int Index = (int)SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETCURSEL, 0, 0);

                if (Index >= 0)
                    SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETLBTEXT, Index, (LPARAM)Fs);
                FillClusterCombo(GetDlgItem(hDlg, IDC_NF_CLUSTER), Fs, Data->SizeMB * DM_MIB);
                FormatUpdate(hDlg, Data);
                return TRUE;
            }
            break;

        case WM_NOTIFY:
            switch (((NMHDR *)lParam)->code)
            {
                case PSN_SETACTIVE:
                {
                    WCHAR Fs[DM_FS_NAME] = L"";
                    int Index;

                    /* the file systems that fit the size chosen on the page before; coming back to the page keeps the choices */
                    if (Data->FormatForMB != Data->SizeMB)
                    {
                        FillFsCombo(GetDlgItem(hDlg, IDC_NF_FS), &Data->FsList, NULL, Data->SizeMB * DM_MIB);
                        Index = (int)SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETCURSEL, 0, 0);
                        if (Index >= 0)
                            SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETLBTEXT, Index, (LPARAM)Fs);
                        FillClusterCombo(GetDlgItem(hDlg, IDC_NF_CLUSTER), Fs, Data->SizeMB * DM_MIB);
                        Data->FormatForMB = Data->SizeMB;
                    }
                    FormatUpdate(hDlg, Data);
                    PropSheet_SetWizButtons(GetParent(hDlg), PSWIZB_BACK | PSWIZB_NEXT);
                    StopPage(hDlg, FALSE);
                    return TRUE;
                }
                case PSN_WIZNEXT:
                {
                    int Index = (int)SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETCURSEL, 0, 0);

                    Data->DoFormat = IsDlgButtonChecked(hDlg, IDC_NF_RADIO_YES) == BST_CHECKED && Index >= 0;
                    Data->Fs[0] = 0;
                    if (Data->DoFormat)
                        SendDlgItemMessageW(hDlg, IDC_NF_FS, CB_GETLBTEXT, Index, (LPARAM)Data->Fs);
                    Data->Cluster = ComboCluster(hDlg, IDC_NF_CLUSTER);
                    GetDlgItemTextW(hDlg, IDC_NF_LABEL, Data->Label, ARRAYSIZE(Data->Label));
                    Data->Quick = IsDlgButtonChecked(hDlg, IDC_NF_QUICK) == BST_CHECKED;
                    StopPage(hDlg, FALSE);
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

static VOID
AddSummary(HWND hList, UINT FormatId, PCWSTR Argument)
{
    WCHAR Format[96], Text[200];

    LoadStringBuffer(FormatId, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Argument);
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)Text);
}

static VOID
NsvFillSummary(HWND hList, PNSV Data)
{
    WCHAR Text[160];

    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    AddSummary(hList, IDS_NS_TYPE, L"");
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%u", Data->Target.Disk);
    AddSummary(hList, IDS_NS_DISK, Text);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Data->SizeMB);
    AddSummary(hList, IDS_NS_SIZE, Text);
    switch (Data->Choice)
    {
        case PATH_LETTER:
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%c:", Data->Letter);
            break;
        case PATH_FOLDER:
            StringCchCopyW(Text, ARRAYSIZE(Text), Data->Folder);
            break;
        default:
            LoadStringBuffer(IDS_NS_NONE, Text, ARRAYSIZE(Text));
    }
    AddSummary(hList, IDS_NS_PATH, Text);
    if (Data->DoFormat)
    {
        AddSummary(hList, IDS_NS_FS, Data->Fs);
        if (Data->Cluster)
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", Data->Cluster);
        else
            LoadStringBuffer(IDS_FM_CLUSTER_DEFAULT, Text, ARRAYSIZE(Text));
        AddSummary(hList, IDS_NS_ALLOC, Text);
        AddSummary(hList, IDS_NS_LABEL, Data->Label);
        LoadStringBuffer(Data->Quick ? IDS_YES : IDS_NO, Text, ARRAYSIZE(Text));
        AddSummary(hList, IDS_NS_QUICK, Text);
    }
    else
    {
        LoadStringBuffer(IDS_NS_NONE, Text, ARRAYSIZE(Text));
        AddSummary(hList, IDS_NS_FS, Text);
    }
}

static BOOL
NsvPerform(HWND hOwner, PNSV Data)
{
    DM_CREATED Created;
    DWORD Error;

    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    Error = DmCreatePartition(Data->Target.Disk, Data->Target.Offset, Data->Target.InExtended, Data->SizeMB * DM_MIB, Data->Planned, &Data->Target.Id, &Created);
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    Data->Changed = TRUE;
    if (Error != ERROR_SUCCESS)
    {
        ShowError(hOwner, Error, NULL);
        return FALSE;
    }
    if (Created.VolumeGuid[0] == 0)
    {
        FormatMessageBox(hOwner, IDS_NV_CREATED_NO_VOLUME, MB_ICONWARNING, NULL);
        return TRUE;
    }
    Error = ApplyPathChoice(Created.VolumeGuid, Data->Choice, Data->Letter, Data->Folder);
    if (Error == ERROR_INVALID_FUNCTION && Data->Choice == PATH_FOLDER)
        FormatMessageBox(hOwner, IDS_PL_NO_REPARSE, MB_ICONERROR, NULL);
    else if (Error != ERROR_SUCCESS)
        ShowError(hOwner, Error, NULL);
    if (Data->DoFormat)
    {
        Error = RunFormat(hOwner, Created.VolumeGuid, Data->Fs, Data->Label, Data->Quick, Data->Cluster, Data->Target.Removable);
        if (Error == ERROR_SUCCESS && Data->Target.Style == DM_STYLE_MBR)
            TypeFix(hOwner, Data->Target.Disk, Created.Offset, Created.VolumeGuid);
    }
    return TRUE;
}

static INT_PTR CALLBACK
NsvFinishProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PNSV Data = PageData(hDlg);

    switch (Message)
    {
        case WM_INITDIALOG:
            Data = (PNSV)((LPPROPSHEETPAGEW)lParam)->lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            SetPageTitleFont(hDlg, IDC_NVW_FTITLE, &Data->hTitleFont);
            return TRUE;
        case WM_NOTIFY:
            switch (((NMHDR *)lParam)->code)
            {
                case PSN_SETACTIVE:
                    NsvFillSummary(GetDlgItem(hDlg, IDC_NVW_SUMMARY), Data);
                    PropSheet_SetWizButtons(GetParent(hDlg), PSWIZB_BACK | PSWIZB_FINISH);
                    StopPage(hDlg, FALSE);
                    return TRUE;
                case PSN_WIZFINISH:
                    NsvPerform(GetParent(hDlg), Data);
                    StopPage(hDlg, FALSE);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

BOOL
WizNewVolume(HWND hOwner, const TARGET *Target)
{
    PNSV Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NSV));
    PDL_LAYOUT Layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DL_LAYOUT));
    PROPSHEETPAGEW Pages[5];
    WCHAR Strings[3][224], Caption[96];
    DWORD Error;
    BOOL Changed = FALSE;

    if (Data == NULL || Layout == NULL)
        goto Done;
    Data->Target = *Target;
    Data->Planned = Layout;
    Data->Quick = TRUE;
    Error = DmPlanPartition(Target->Disk, Target->Offset, Target->InExtended, &Data->Free, &Data->NeedsExtended, Layout);
    if (Error == ERROR_SUCCESS && Data->Free.WizardMax < MIN_VOLUME_MB * DM_MIB)
        Error = DM_ERR_LAYOUT_BASE + DL_E_NO_SPACE;
    if (Error != ERROR_SUCCESS)
    {
        ShowError(hOwner, Error, NULL);
        goto Done;
    }
    Data->MaxMB = Data->Free.WizardMax / DM_MIB;
    DmQueryFileSystems(&Data->FsList);

    LoadStringBuffer(IDS_NVW_CAPTION, Caption, ARRAYSIZE(Caption));
    InitPage(&Pages[0], IDD_NVW_WELCOME, NsvWelcomeProc, (LPARAM)Data, Caption, 0, 0, NULL);
    InitPage(&Pages[1], IDD_NVW_SIZE, NsvSizeProc, (LPARAM)Data, Caption, IDS_NVW_SIZE_TITLE, IDS_NVW_SIZE_SUB, Strings[0]);
    InitPage(&Pages[2], IDD_NVW_LETTER, NsvLetterProc, (LPARAM)Data, Caption, IDS_NVW_LETTER_TITLE, IDS_NVW_LETTER_SUB, Strings[1]);
    InitPage(&Pages[3], IDD_NVW_FORMAT, NsvFormatProc, (LPARAM)Data, Caption, IDS_NVW_FORMAT_TITLE, IDS_NVW_FORMAT_SUB, Strings[2]);
    InitPage(&Pages[4], IDD_NVW_FINISH, NsvFinishProc, (LPARAM)Data, Caption, 0, 0, NULL);
    RunWizard(hOwner, Pages, ARRAYSIZE(Pages), Caption);
    Changed = Data->Changed;
    if (Data->hTitleFont)
        DeleteObject(Data->hTitleFont);
Done:
    if (Layout)
        HeapFree(GetProcessHeap(), 0, Layout);
    if (Data)
        HeapFree(GetProcessHeap(), 0, Data);
    return Changed;
}

/* ================================================================ extend volume */

typedef struct _EXV
{
    TARGET Target;
    ULONGLONG TotalMB;          /* the volume now */
    ULONGLONG MaxMB;            /* what can be added */
    ULONGLONG AddMB;
    BOOL Changed;
    HFONT hTitleFont;
} EXV, *PEXV;

static INT_PTR CALLBACK
ExvWelcomeProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PEXV Data = (PEXV)((LPPROPSHEETPAGEW)lParam)->lParam;

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            SetPageTitleFont(hDlg, IDC_EXW_TITLE, &Data->hTitleFont);
            return TRUE;
        }
        case WM_NOTIFY:
            if (((NMHDR *)lParam)->code == PSN_SETACTIVE)
            {
                PropSheet_SetWizButtons(GetParent(hDlg), PSWIZB_NEXT);
                StopPage(hDlg, FALSE);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static VOID
ExvShow(HWND hDlg, PEXV Data)
{
    WCHAR Text[64];
    ULONGLONG Add = 0;

    if (!ParseSize(hDlg, IDC_EX_AMOUNT, 1, Data->MaxMB, &Add))
        Add = 0;
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Data->TotalMB + Add);
    SetDlgItemTextW(hDlg, IDC_EX_TOTAL, Text);
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"Disk %u    %I64u MB", Data->Target.Disk, Add);
    SendDlgItemMessageW(hDlg, IDC_EX_SELECTED, LB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(hDlg, IDC_EX_SELECTED, LB_ADDSTRING, 0, (LPARAM)Text);
    SendDlgItemMessageW(hDlg, IDC_EX_SELECTED, LB_SETCURSEL, 0, 0);
}

static INT_PTR CALLBACK
ExvDisksProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PEXV Data = (PEXV)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            WCHAR Text[32];

            Data = (PEXV)((LPPROPSHEETPAGEW)lParam)->lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Data->MaxMB);
            SetDlgItemTextW(hDlg, IDC_EX_MAX, Text);
            SetDlgItemTextW(hDlg, IDC_EX_AMOUNT, Text);
            SendDlgItemMessageW(hDlg, IDC_EX_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(hDlg, IDC_EX_AMOUNT), 0);
            SendDlgItemMessageW(hDlg, IDC_EX_SPIN, UDM_SETRANGE32, 1, (LPARAM)Data->MaxMB);
            SendDlgItemMessageW(hDlg, IDC_EX_SPIN, UDM_SETPOS32, 0, (LPARAM)Data->MaxMB);
            /* disks other than this one would need dynamic disks, which this program does not make */
            EnableWindow(GetDlgItem(hDlg, IDC_EX_ADD), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_EX_REMOVE), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_EX_REMOVEALL), FALSE);
            ExvShow(hDlg, Data);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_EX_AMOUNT && HIWORD(wParam) == EN_CHANGE)
            {
                ExvShow(hDlg, Data);
                UpdateNext(hDlg, IDC_EX_AMOUNT, 1, Data->MaxMB);
                return TRUE;
            }
            break;
        case WM_NOTIFY:
            switch (((NMHDR *)lParam)->code)
            {
                case PSN_SETACTIVE:
                    UpdateNext(hDlg, IDC_EX_AMOUNT, 1, Data->MaxMB);
                    StopPage(hDlg, FALSE);
                    return TRUE;
                case PSN_WIZNEXT:
                {
                    ULONGLONG Add;

                    if (!ParseSize(hDlg, IDC_EX_AMOUNT, 1, Data->MaxMB, &Add))
                    {
                        SizeRangeMessage(hDlg, 1, Data->MaxMB);
                        StopPage(hDlg, TRUE);
                        return TRUE;
                    }
                    Data->AddMB = Add;
                    StopPage(hDlg, FALSE);
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

static INT_PTR CALLBACK
ExvFinishProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PEXV Data = (PEXV)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (Message)
    {
        case WM_INITDIALOG:
            Data = (PEXV)((LPPROPSHEETPAGEW)lParam)->lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            SetPageTitleFont(hDlg, IDC_EXW_FTITLE, &Data->hTitleFont);
            return TRUE;
        case WM_NOTIFY:
            switch (((NMHDR *)lParam)->code)
            {
                case PSN_SETACTIVE:
                {
                    WCHAR Format[96], Text[160];
                    HWND hList = GetDlgItem(hDlg, IDC_EX_SUMMARY);

                    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
                    LoadStringBuffer(IDS_EX_DISK, Format, ARRAYSIZE(Format));
                    StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Data->Target.Disk, Data->AddMB);
                    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)Text);
                    PropSheet_SetWizButtons(GetParent(hDlg), PSWIZB_BACK | PSWIZB_FINISH);
                    StopPage(hDlg, FALSE);
                    return TRUE;
                }
                case PSN_WIZFINISH:
                {
                    DWORD Error;

                    SetCursor(LoadCursorW(NULL, IDC_WAIT));
                    Error = DmExtendVolume(Data->Target.Disk, Data->Target.Offset, Data->Target.Length, Data->Target.Guid, Data->AddMB * DM_MIB, &Data->Target.Id);
                    SetCursor(LoadCursorW(NULL, IDC_ARROW));
                    Data->Changed = TRUE;
                    if (Error != ERROR_SUCCESS)
                        ShowError(GetParent(hDlg), Error, NULL);
                    StopPage(hDlg, FALSE);
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

BOOL
WizExtendVolume(HWND hOwner, const TARGET *Target, const DM_DISK *Disk)
{
    PEXV Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(EXV));
    PROPSHEETPAGEW Pages[3];
    WCHAR Strings[1][224], Caption[96];
    ULONGLONG MaxBytes = 0;
    DWORD Error;
    BOOL Changed = FALSE;

    UNREFERENCED_PARAMETER(Disk);
    if (Data == NULL)
        return FALSE;
    Data->Target = *Target;
    Error = DmQueryExtend(Target->Disk, Target->Offset, &MaxBytes);
    if (Error != ERROR_SUCCESS || MaxBytes < DM_MIB)
    {
        ShowError(hOwner, Error != ERROR_SUCCESS ? Error : DM_ERR_LAYOUT_BASE + DL_E_NO_SPACE, NULL);
        HeapFree(GetProcessHeap(), 0, Data);
        return FALSE;
    }
    Data->TotalMB = Target->Length / DM_MIB;
    Data->MaxMB = MaxBytes / DM_MIB;

    LoadStringBuffer(IDS_EXW_CAPTION, Caption, ARRAYSIZE(Caption));
    InitPage(&Pages[0], IDD_EXW_WELCOME, ExvWelcomeProc, (LPARAM)Data, Caption, 0, 0, NULL);
    InitPage(&Pages[1], IDD_EXW_DISKS, ExvDisksProc, (LPARAM)Data, Caption, IDS_EXW_DISKS_TITLE, IDS_EXW_DISKS_SUB, Strings[0]);
    InitPage(&Pages[2], IDD_EXW_FINISH, ExvFinishProc, (LPARAM)Data, Caption, 0, 0, NULL);
    RunWizard(hOwner, Pages, ARRAYSIZE(Pages), Caption);
    Changed = Data->Changed;
    if (Data->hTitleFont)
        DeleteObject(Data->hTitleFont);
    HeapFree(GetProcessHeap(), 0, Data);
    return Changed;
}
