/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Read-only property sheets of a volume and of a disk
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <math.h>
#include <shlwapi.h>
#include "diskmgmt.h"

typedef struct _PROP_DATA
{
    const DM_DISK *Disk;
    const DM_REGION *Region;
    BOOL Changed;               /* the label was changed: the window reads the disks again */
} PROP_DATA, *PPROP_DATA;

#define COLOR_USED  RGB(0, 0, 255)
#define COLOR_FREE_SPACE RGB(255, 0, 255)

static VOID
FormatBytes(ULONGLONG Value, PWSTR Buffer, int Count)
{
    WCHAR Digits[32];
    NUMBERFMTW Format;
    WCHAR Suffix[32], Number[64];

    StringCchPrintfW(Digits, ARRAYSIZE(Digits), L"%I64u", Value);
    ZeroMemory(&Format, sizeof(Format));
    Format.NumDigits = 0;
    Format.LeadingZero = 0;
    Format.Grouping = 3;
    Format.lpDecimalSep = L".";
    Format.lpThousandSep = L",";
    Format.NegativeOrder = 1;
    if (!GetNumberFormatW(LOCALE_USER_DEFAULT, 0, Digits, &Format, Number, ARRAYSIZE(Number)))
        StringCchCopyW(Number, ARRAYSIZE(Number), Digits);
    LoadStringBuffer(IDS_BYTES_FMT, Suffix, ARRAYSIZE(Suffix));
    StringCchPrintfW(Buffer, Count, Suffix, Number);
}

static VOID
SetText(HWND hDlg, UINT Id, PCWSTR Text)
{
    SetDlgItemTextW(hDlg, Id, Text);
}

static VOID
SetMB(HWND hDlg, UINT Id, ULONGLONG MegaBytes)
{
    WCHAR Format[32], Text[48];

    LoadStringBuffer(IDS_MB_FMT, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Text, ARRAYSIZE(Text), Format, MegaBytes);
    SetDlgItemTextW(hDlg, Id, Text);
}

static HICON
DriveIcon(const DM_DISK *Disk)
{
    HICON Icon = NULL;
    int Id = Disk->Device == DM_DEV_CDROM ? -12 : Disk->Device == DM_DEV_REMOVABLE ? -8 : -9;

    ExtractIconExW(L"shell32.dll", Id, &Icon, NULL, 1);
    return Icon;
}

static VOID
RegionTitle(const DM_DISK *Disk, const DM_REGION *Region, PWSTR Buffer, int Count)
{
    WCHAR Format[64];

    if (Region->Letter[0])
    {
        LoadStringBuffer(IDS_PROP_VOLUME_TITLE, Format, ARRAYSIZE(Format));
        if (Region->Label[0])
        {
            WCHAR Name[96];

            StringCchPrintfW(Name, ARRAYSIZE(Name), L"%s (%s)", Region->Label, Region->Letter);
            StringCchPrintfW(Buffer, Count, Format, Name);
        }
        else
        {
            WCHAR Name[16];

            StringCchPrintfW(Name, ARRAYSIZE(Name), L"(%s)", Region->Letter);
            StringCchPrintfW(Buffer, Count, Format, Name);
        }
    }
    else
    {
        LoadStringBuffer(IDS_PROP_PARTITION_TITLE, Format, ARRAYSIZE(Format));
        StringCchPrintfW(Buffer, Count, Format, Disk->Number, Region->PartitionNumber);
    }
}

/* ----------------------------------------------------------- volume: general */

static VOID
DrawPie(const DRAWITEMSTRUCT *Draw, ULONGLONG Used, ULONGLONG Total)
{
    const double Pi = 3.14159265358979;
    const int Depth = 8;
    HDC hDC = Draw->hDC;
    RECT rc = Draw->rcItem, Top;
    HBRUSH Brush, OldBrush;
    HPEN OldPen;
    int Cx, Cy, Rx, Ry;

    FillRect(hDC, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetRect(&Top, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 - Depth);
    Cx = (Top.left + Top.right) / 2;
    Cy = (Top.top + Top.bottom) / 2;
    Rx = (Top.right - Top.left) / 2;
    Ry = (Top.bottom - Top.top) / 2;
    OldPen = SelectObject(hDC, GetStockObject(BLACK_PEN));

    /* the rim of the disc: the lower ellipse and the body that joins it to the top */
    Brush = CreateSolidBrush(RGB(128, 0, 128));
    OldBrush = SelectObject(hDC, Brush);
    Ellipse(hDC, Top.left, Top.top + Depth, Top.right, Top.bottom + Depth);
    SelectObject(hDC, GetStockObject(NULL_PEN));
    Rectangle(hDC, Top.left + 1, Cy, Top.right, Cy + Depth + 1);
    SelectObject(hDC, GetStockObject(BLACK_PEN));
    SelectObject(hDC, OldBrush);
    DeleteObject(Brush);

    /* the top: free space, with the used part as a wedge */
    Brush = CreateSolidBrush(COLOR_FREE_SPACE);
    OldBrush = SelectObject(hDC, Brush);
    Ellipse(hDC, Top.left, Top.top, Top.right, Top.bottom);
    SelectObject(hDC, OldBrush);
    DeleteObject(Brush);

    if (Total > 0 && Used > 0)
    {
        Brush = CreateSolidBrush(COLOR_USED);
        OldBrush = SelectObject(hDC, Brush);
        if (Used >= Total)
        {
            Ellipse(hDC, Top.left, Top.top, Top.right, Top.bottom);
        }
        else
        {
            /* from three o'clock, counter-clockwise */
            double Angle = 2.0 * Pi * (double)Used / (double)Total;

            Pie(hDC, Top.left, Top.top, Top.right, Top.bottom, Cx + Rx, Cy,
                Cx + (int)(Rx * cos(Angle)), Cy - (int)(Ry * sin(Angle)));
        }
        SelectObject(hDC, OldBrush);
        DeleteObject(Brush);
    }
    SelectObject(hDC, OldPen);
}

static INT_PTR CALLBACK
VolGeneralProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    PPROP_DATA Data;

    switch (Message)
    {
        case WM_INITDIALOG:
        {
            const DM_DISK *Disk;
            const DM_REGION *Region;
            WCHAR Text[160], Format[64];
            ULONGLONG Total, Free;
            HICON Icon;

            Data = (PPROP_DATA)((LPPROPSHEETPAGEW)lParam)->lParam;
            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            Disk = Data->Disk;
            Region = Data->Region;

            Icon = DriveIcon(Disk);
            if (Icon)
                SendDlgItemMessageW(hDlg, IDC_VOL_ICON, STM_SETICON, (WPARAM)Icon, 0);

            SetText(hDlg, IDC_VOL_NAME, Region->Label);
            if (Disk->Device == DM_DEV_CDROM || !Region->HasFsInfo || Region->VolumeGuid[0] == 0 || Region->IsDynamic)
                SendDlgItemMessageW(hDlg, IDC_VOL_NAME, EM_SETREADONLY, TRUE, 0);
            else
                SendDlgItemMessageW(hDlg, IDC_VOL_NAME, EM_LIMITTEXT,
                                    (_wcsicmp(Region->FileSystem, L"FAT") == 0 || _wcsicmp(Region->FileSystem, L"FAT32") == 0) ? 11 : 32, 0);
            LoadStringBuffer(Disk->Device == DM_DEV_CDROM ? IDS_TYPE_CDROM :
                             Disk->Device == DM_DEV_REMOVABLE ? IDS_TYPE_REMOVABLE : IDS_TYPE_LOCAL, Text, ARRAYSIZE(Text));
            SetText(hDlg, IDC_VOL_TYPE, Text);
            SetText(hDlg, IDC_VOL_FS, Region->FileSystem);

            if (Region->Letter[0])
            {
                LoadStringBuffer(IDS_DRIVE_FMT, Format, ARRAYSIZE(Format));
                StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Region->Letter);
                SetText(hDlg, IDC_VOL_DRIVE, Text);
            }

            if (Region->HasSpace)
            {
                Total = Region->VolTotal;
                Free = Region->VolFree;
                FormatBytes(Total - Free, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_USED_BYTES, Text);
                StrFormatByteSizeW(Total - Free, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_USED_HUMAN, Text);
                FormatBytes(Free, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_FREE_BYTES, Text);
                StrFormatByteSizeW(Free, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_FREE_HUMAN, Text);
                FormatBytes(Total, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_CAP_BYTES, Text);
                StrFormatByteSizeW(Total, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_CAP_HUMAN, Text);
                ShowWindow(GetDlgItem(hDlg, IDC_VOL_NOTE), SW_HIDE);
            }
            else
            {
                const UINT Hide[] = { IDC_VOL_USED_COLOR, IDC_VOL_FREE_COLOR, IDC_VOL_PIE };
                int i;

                for (i = 0; i < (int)ARRAYSIZE(Hide); i++)
                    ShowWindow(GetDlgItem(hDlg, Hide[i]), SW_HIDE);
                LoadStringBuffer(IDS_NOTE_NOFS, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_NOTE, Text);
                FormatBytes(Region->Length, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_CAP_BYTES, Text);
                StrFormatByteSizeW(Region->Length, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_VOL_CAP_HUMAN, Text);
                ShowWindow(GetDlgItem(hDlg, IDC_VOL_USED_BYTES), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_VOL_FREE_BYTES), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_VOL_USED_HUMAN), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_VOL_FREE_HUMAN), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_VOL_USED_LABEL), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_VOL_FREE_LABEL), SW_HIDE);
            }
            return TRUE;
        }

        case WM_NOTIFY:
            if (((NMHDR *)lParam)->code == PSN_APPLY)
            {
                WCHAR Label[64];

                Data = (PPROP_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);
                if (!(GetWindowLongW(GetDlgItem(hDlg, IDC_VOL_NAME), GWL_STYLE) & ES_READONLY))
                {
                    GetDlgItemTextW(hDlg, IDC_VOL_NAME, Label, ARRAYSIZE(Label));
                    if (wcscmp(Label, Data->Region->Label) != 0)
                    {
                        DWORD Error = DmSetVolumeLabel(Data->Region->VolumeGuid, Label);

                        if (Error == ERROR_SUCCESS)
                        {
                            Data->Changed = TRUE;
                        }
                        else
                        {
                            WCHAR Format[96], Message[256], Detail[160], Title[64];

                            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error, 0, Detail,
                                           ARRAYSIZE(Detail), NULL);
                            LoadStringBuffer(IDS_PROP_LABEL_FAILED, Format, ARRAYSIZE(Format));
                            StringCchPrintfW(Message, ARRAYSIZE(Message), Format, Detail);
                            LoadStringBuffer(IDS_ERROR_TITLE, Title, ARRAYSIZE(Title));
                            MessageBoxW(hDlg, Message, Title, MB_OK | MB_ICONERROR);
                            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                            return TRUE;
                        }
                    }
                }
                SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
                return TRUE;
            }
            return FALSE;

        case WM_DESTROY:
        {
            HICON Icon = (HICON)SendDlgItemMessageW(hDlg, IDC_VOL_ICON, STM_GETICON, 0, 0);

            if (Icon)
                DestroyIcon(Icon);
            return FALSE;
        }

        case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT *Draw = (const DRAWITEMSTRUCT *)lParam;

            Data = (PPROP_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);
            if (Draw->CtlID == IDC_VOL_PIE)
            {
                DrawPie(Draw, Data->Region->VolTotal - Data->Region->VolFree, Data->Region->VolTotal);
                return TRUE;
            }
            if (Draw->CtlID == IDC_VOL_USED_COLOR || Draw->CtlID == IDC_VOL_FREE_COLOR)
            {
                HBRUSH Brush = CreateSolidBrush(Draw->CtlID == IDC_VOL_USED_COLOR ? COLOR_USED : COLOR_FREE_SPACE);

                FillRect(Draw->hDC, &Draw->rcItem, Brush);
                FrameRect(Draw->hDC, &Draw->rcItem, GetStockObject(BLACK_BRUSH));
                DeleteObject(Brush);
                return TRUE;
            }
            return FALSE;
        }
    }
    return FALSE;
}

/* --------------------------------------------------------- volume: partition */

static PCWSTR
GptTypeName(const GUID *Type, PWSTR Buffer, int Count)
{
    static const struct { GUID Guid; PCWSTR Name; } Known[] =
    {
        { { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } }, L"Basic data partition" },
        { { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } }, L"EFI system partition" },
        { { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } }, L"Microsoft reserved partition" },
        { { 0xDE94BBA4, 0x06D1, 0x4D40, { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } }, L"Windows recovery partition" },
        { { 0x5808C8AA, 0x7E8F, 0x42E0, { 0x85, 0xD2, 0xE1, 0xE9, 0x04, 0x34, 0xCF, 0xB3 } }, L"Logical Disk Manager metadata partition" },
        { { 0xAF9B60A0, 0x1431, 0x4F62, { 0xBC, 0x68, 0x33, 0x11, 0x71, 0x4A, 0x69, 0xAD } }, L"Logical Disk Manager data partition" },
    };
    int i;

    for (i = 0; i < (int)ARRAYSIZE(Known); i++)
    {
        if (memcmp(&Known[i].Guid, Type, sizeof(GUID)) == 0)
        {
            StringCchPrintfW(Buffer, Count, L"%s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", Known[i].Name,
                             Type->Data1, Type->Data2, Type->Data3, Type->Data4[0], Type->Data4[1], Type->Data4[2],
                             Type->Data4[3], Type->Data4[4], Type->Data4[5], Type->Data4[6], Type->Data4[7]);
            return Buffer;
        }
    }
    StringCchPrintfW(Buffer, Count, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                     Type->Data1, Type->Data2, Type->Data3, Type->Data4[0], Type->Data4[1], Type->Data4[2],
                     Type->Data4[3], Type->Data4[4], Type->Data4[5], Type->Data4[6], Type->Data4[7]);
    return Buffer;
}

static VOID
AddRow(HWND hList, UINT NameId, PCWSTR Value)
{
    WCHAR Name[64];
    LVITEMW Item;
    int Index = ListView_GetItemCount(hList);

    LoadStringBuffer(NameId, Name, ARRAYSIZE(Name));
    ZeroMemory(&Item, sizeof(Item));
    Item.mask = LVIF_TEXT;
    Item.iItem = Index;
    Item.pszText = Name;
    SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&Item);
    Item.iSubItem = 1;
    Item.pszText = (PWSTR)Value;
    SendMessageW(hList, LVM_SETITEMTEXTW, Index, (LPARAM)&Item);
}

static INT_PTR CALLBACK
VolPartitionProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    if (Message == WM_INITDIALOG)
    {
        PPROP_DATA Data = (PPROP_DATA)((LPPROPSHEETPAGEW)lParam)->lParam;
        const DM_DISK *Disk = Data->Disk;
        const DM_REGION *Region = Data->Region;
        HWND hList = GetDlgItem(hDlg, IDC_PROP_LIST);
        LVCOLUMNW Column;
        WCHAR Text[512], Size[32], Name[32];
        RECT rc;

        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT);
        GetClientRect(hList, &rc);
        ZeroMemory(&Column, sizeof(Column));
        Column.mask = LVCF_TEXT | LVCF_WIDTH;
        LoadStringBuffer(IDS_COL_PROPERTY, Name, ARRAYSIZE(Name));
        Column.pszText = Name;
        Column.cx = 110;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&Column);
        LoadStringBuffer(IDS_COL_VALUE, Name, ARRAYSIZE(Name));
        Column.cx = rc.right - 110 - GetSystemMetrics(SM_CXVSCROLL);
        SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&Column);

        if (Region->Label[0])
            AddRow(hList, IDS_PROP_LABEL, Region->Label);
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%s %u", Disk->Device == DM_DEV_CDROM ? L"CD-ROM" : L"Disk", Disk->Number);
        AddRow(hList, IDS_PROP_DISK, Text);
        if (Disk->Device != DM_DEV_CDROM)
        {
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%u", Region->PartitionNumber);
            AddRow(hList, IDS_PROP_PARTNUM, Text);
            LoadStringBuffer(Disk->Style == DM_STYLE_GPT ? IDS_STYLE_GPT : Disk->Style == DM_STYLE_MBR ? IDS_STYLE_MBR : IDS_STYLE_RAW,
                             Text, ARRAYSIZE(Text));
            AddRow(hList, IDS_PROP_STYLE, Text);
            if (Disk->Style == DM_STYLE_GPT)
            {
                AddRow(hList, IDS_PROP_TYPE, GptTypeName(&Region->GptType, Text, ARRAYSIZE(Text)));
                if (Region->GptName[0])
                    AddRow(hList, IDS_PROP_PARTNAME, Region->GptName);
                AddRow(hList, IDS_PROP_PARTID, GptTypeName(&Region->GptId, Text, ARRAYSIZE(Text)) + 0);
            }
            else
            {
                StringCchPrintfW(Text, ARRAYSIZE(Text), L"0x%02X", Region->MbrType);
                AddRow(hList, IDS_PROP_TYPE, Text);
                LoadStringBuffer(Region->Active ? IDS_YES : IDS_NO, Text, ARRAYSIZE(Text));
                AddRow(hList, IDS_PROP_ACTIVE, Text);
            }
            FormatBytes(Region->Offset, Text, ARRAYSIZE(Text));
            AddRow(hList, IDS_PROP_OFFSET, Text);
        }
        FormatBytes(Region->Length, Text, ARRAYSIZE(Text));
        StrFormatByteSizeW(Region->Length, Size, ARRAYSIZE(Size));
        StringCchPrintfW(Text + wcslen(Text), ARRAYSIZE(Text) - wcslen(Text), L"  (%s)", Size);
        AddRow(hList, IDS_PROP_LENGTH, Text);
        AddRow(hList, IDS_PROP_STATUS, Region->Status);
        if (Region->HasVolume)
        {
            PCWSTR Path;
            WCHAR Paths[300];

            Paths[0] = 0;
            for (Path = Region->MountPaths; *Path; Path += wcslen(Path) + 1)
            {
                if (Paths[0])
                    StringCchCatW(Paths, ARRAYSIZE(Paths), L"  ");
                StringCchCatW(Paths, ARRAYSIZE(Paths), Path);
            }
            if (Paths[0])
                AddRow(hList, IDS_PROP_PATHS, Paths);
            AddRow(hList, IDS_PROP_VOLGUID, Region->VolumeGuid);
        }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------- disk: general */

static INT_PTR CALLBACK
DiskGeneralProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PPROP_DATA Data = (PPROP_DATA)((LPPROPSHEETPAGEW)lParam)->lParam;
            const DM_DISK *Disk = Data->Disk;
            WCHAR Text[160], Format[96];
            HICON Icon = DriveIcon(Disk);

            if (Icon)
                SendDlgItemMessageW(hDlg, IDC_DISK_ICON, STM_SETICON, (WPARAM)Icon, 0);
            SetText(hDlg, IDC_DISK_NAME, Disk->DeviceName[0] ? Disk->DeviceName : Disk->BusText);
            LoadStringBuffer(IDS_DEVTYPE_DISK, Text, ARRAYSIZE(Text));
            SetText(hDlg, IDC_DISK_DEVTYPE, Text);
            if (Disk->ScsiPath >= 0)
            {
                LoadStringBuffer(IDS_LOCATION_FMT, Format, ARRAYSIZE(Format));
                StringCchPrintfW(Text, ARRAYSIZE(Text), Format, Disk->ScsiPath, Disk->ScsiTarget, Disk->ScsiLun);
                SetText(hDlg, IDC_DISK_LOCATION, Text);
            }
            else
            {
                LoadStringBuffer(IDS_NOT_AVAILABLE, Text, ARRAYSIZE(Text));
                SetText(hDlg, IDC_DISK_LOCATION, Text);
            }
            SetText(hDlg, IDC_DISK_STATUS, Disk->StatusText);
            return TRUE;
        }
        case WM_DESTROY:
        {
            HICON Icon = (HICON)SendDlgItemMessageW(hDlg, IDC_DISK_ICON, STM_GETICON, 0, 0);

            if (Icon)
                DestroyIcon(Icon);
            return FALSE;
        }
    }
    return FALSE;
}

/* -------------------------------------------------------------- disk: volumes */

static INT_PTR CALLBACK
DiskVolumesProc(HWND hDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_INITDIALOG:
        {
            PPROP_DATA Data = (PPROP_DATA)((LPPROPSHEETPAGEW)lParam)->lParam;
            const DM_DISK *Disk = Data->Disk;
            WCHAR Text[96];
            HWND hList = GetDlgItem(hDlg, IDC_DV_LIST);
            LVCOLUMNW Column;
            RECT rc;
            ULONG r;
            int Row = 0;

            SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)Data);
            StringCchPrintfW(Text, ARRAYSIZE(Text), L"%s %u", Disk->Device == DM_DEV_CDROM ? L"CD-ROM" : L"Disk", Disk->Number);
            SetText(hDlg, IDC_DV_DISK, Text);
            StringCchCopyW(Text, ARRAYSIZE(Text), Disk->Dynamic ? L"Dynamic" : Disk->Style == DM_STYLE_RAW ? L"Unknown" : L"Basic");
            SetText(hDlg, IDC_DV_TYPE, Text);
            SetText(hDlg, IDC_DV_STATUS, Disk->StatusText);
            LoadStringBuffer(Disk->Style == DM_STYLE_GPT ? IDS_STYLE_GPT : Disk->Style == DM_STYLE_MBR ? IDS_STYLE_MBR : IDS_STYLE_RAW,
                             Text, ARRAYSIZE(Text));
            SetText(hDlg, IDC_DV_STYLE, Text);
            SetMB(hDlg, IDC_DV_CAPACITY, Disk->CapacityMB);
            SetMB(hDlg, IDC_DV_UNALLOC, Disk->UnallocatedMB);
            SetMB(hDlg, IDC_DV_RESERVED, Disk->ReservedMB);

            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT);
            GetClientRect(hList, &rc);
            ZeroMemory(&Column, sizeof(Column));
            Column.mask = LVCF_TEXT | LVCF_WIDTH;
            LoadStringBuffer(IDS_COL_VOL_NAME, Text, ARRAYSIZE(Text));
            Column.pszText = Text;
            Column.cx = rc.right - 80 - GetSystemMetrics(SM_CXVSCROLL);
            SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&Column);
            LoadStringBuffer(IDS_COL_VOL_CAP, Text, ARRAYSIZE(Text));
            Column.cx = 80;
            SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&Column);

            for (r = 0; r < Disk->RegionCount; r++)
            {
                const DM_REGION *Region = &Disk->Regions[r];
                DM_BLOCK_TEXT Block;
                LVITEMW Item;
                WCHAR Name[96], Cap[32], Format[32];

                if (Region->Kind != DM_KIND_PRIMARY && Region->Kind != DM_KIND_LOGICAL)
                    continue;
                if (Region->IsDynamic && !Region->HasVolume)
                    continue;
                DmBlockText(Disk, Region, &Block);
                StringCchCopyW(Name, ARRAYSIZE(Name), Block.Line1);
                LoadStringBuffer(IDS_MB_FMT, Format, ARRAYSIZE(Format));
                StringCchPrintfW(Cap, ARRAYSIZE(Cap), Format, Region->Length / DM_MIB);
                ZeroMemory(&Item, sizeof(Item));
                Item.mask = LVIF_TEXT | LVIF_PARAM;
                Item.iItem = Row;
                Item.pszText = Name;
                Item.lParam = (LPARAM)Region;
                SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&Item);
                Item.mask = LVIF_TEXT;
                Item.iSubItem = 1;
                Item.pszText = Cap;
                SendMessageW(hList, LVM_SETITEMTEXTW, Row, (LPARAM)&Item);
                Row++;
            }
            if (Row > 0)
                ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            EnableWindow(GetDlgItem(hDlg, IDC_DV_PROPERTIES), Row > 0);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_DV_PROPERTIES && HIWORD(wParam) == BN_CLICKED)
            {
                PPROP_DATA Data = (PPROP_DATA)GetWindowLongPtrW(hDlg, DWLP_USER);
                HWND hList = GetDlgItem(hDlg, IDC_DV_LIST);
                int Sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);

                if (Sel >= 0)
                {
                    LVITEMW Item;

                    ZeroMemory(&Item, sizeof(Item));
                    Item.mask = LVIF_PARAM;
                    Item.iItem = Sel;
                    SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&Item);
                    if (ShowRegionProperties(hDlg, Data->Disk, (const DM_REGION *)Item.lParam))
                    {
                        PDM_REGION Region = (PDM_REGION)Item.lParam;
                        WCHAR Label[64];

                        Data->Changed = TRUE;
                        /* the label may have changed: the snapshot of this list and the next Properties show what the volume holds now */
                        if (Region->HasVolume &&
                            GetVolumeInformationW(Region->VolumeGuid, Label, ARRAYSIZE(Label), NULL, NULL, NULL, NULL, 0))
                        {
                            DM_BLOCK_TEXT Block;
                            LVITEMW Text;

                            StringCchCopyW(Region->Label, ARRAYSIZE(Region->Label), Label);
                            DmBlockText(Data->Disk, Region, &Block);
                            ZeroMemory(&Text, sizeof(Text));
                            Text.mask = LVIF_TEXT;
                            Text.iItem = Sel;
                            Text.pszText = Block.Line1;
                            SendMessageW(hList, LVM_SETITEMTEXTW, Sel, (LPARAM)&Text);
                        }
                    }
                }
                return TRUE;
            }
            break;

        case WM_NOTIFY:
        {
            const NMHDR *Header = (const NMHDR *)lParam;

            if (Header->idFrom == IDC_DV_LIST && Header->code == NM_DBLCLK)
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_DV_PROPERTIES, BN_CLICKED), 0);
            break;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------- sheets */

static VOID
RunSheet(HWND hOwner, PCWSTR Title, PROPSHEETPAGEW *Pages, UINT Count)
{
    PROPSHEETHEADERW Header;

    ZeroMemory(&Header, sizeof(Header));
    Header.dwSize = sizeof(Header);
    Header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    Header.hwndParent = hOwner;
    Header.hInstance = g_hInst;
    Header.pszCaption = Title;
    Header.nPages = Count;
    Header.nStartPage = 0;
    Header.ppsp = Pages;
    PropertySheetW(&Header);
}

static VOID
InitPage(PROPSHEETPAGEW *Page, UINT Template, UINT TitleId, DLGPROC Proc, PPROP_DATA Data, PWSTR TitleBuffer)
{
    ZeroMemory(Page, sizeof(*Page));
    Page->dwSize = sizeof(*Page);
    Page->dwFlags = PSP_USETITLE;
    Page->hInstance = g_hInst;
    Page->pszTemplate = MAKEINTRESOURCEW(Template);
    LoadStringBuffer(TitleId, TitleBuffer, 64);
    Page->pszTitle = TitleBuffer;
    Page->pfnDlgProc = Proc;
    Page->lParam = (LPARAM)Data;
}

BOOL
ShowRegionProperties(HWND hOwner, const DM_DISK *Disk, const DM_REGION *Region)
{
    PROP_DATA Data;
    PROPSHEETPAGEW Pages[2];
    WCHAR Titles[2][64], Caption[128];

    Data.Disk = Disk;
    Data.Region = Region;
    Data.Changed = FALSE;
    InitPage(&Pages[0], IDD_VOL_GENERAL, IDS_PAGE_GENERAL, VolGeneralProc, &Data, Titles[0]);
    InitPage(&Pages[1], IDD_VOL_PARTITION, IDS_PAGE_PARTITION, VolPartitionProc, &Data, Titles[1]);
    RegionTitle(Disk, Region, Caption, ARRAYSIZE(Caption));
    RunSheet(hOwner, Caption, Pages, 2);
    return Data.Changed;
}

BOOL
ShowDiskProperties(HWND hOwner, const DM_DISK *Disk)
{
    PROP_DATA Data;
    PROPSHEETPAGEW Pages[2];
    WCHAR Titles[2][64], Format[64], Caption[192];

    Data.Disk = Disk;
    Data.Region = NULL;
    Data.Changed = FALSE;
    InitPage(&Pages[0], IDD_DISK_GENERAL, IDS_PAGE_GENERAL, DiskGeneralProc, &Data, Titles[0]);
    InitPage(&Pages[1], IDD_DISK_VOLUMES, IDS_PAGE_VOLUMES, DiskVolumesProc, &Data, Titles[1]);
    LoadStringBuffer(IDS_PROP_DISK_TITLE, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Caption, ARRAYSIZE(Caption), Format,
                     Disk->DeviceName[0] ? Disk->DeviceName : Disk->BusText);
    RunSheet(hOwner, Caption, Pages, 2);
    return Data.Changed;
}
