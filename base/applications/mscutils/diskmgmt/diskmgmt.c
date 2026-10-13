/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     View of the disks, partitions and volumes of the computer
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "diskmgmt.h"

#define MAIN_CLASS      L"DiskMgmtMainWindow"
#define SPLITTER_H      4
#define MIN_PANE_H      60

typedef enum _VIEW
{
    VIEW_DISKLIST = 0,
    VIEW_VOLLIST,
    VIEW_GRAPH,
    VIEW_HIDDEN
} VIEW;

HINSTANCE g_hInst;
HFONT g_hFont;
HFONT g_hFontBold;

static HWND g_hMain, g_hToolbar, g_hStatus, g_hDiskList, g_hVolList, g_hGraph;
static HIMAGELIST g_hImages;
static PDM_MODEL g_Model;
static PDM_ROW g_Rows;
static ULONG g_RowCount;
static DM_SELECTION g_Sel;
static BOOL g_Syncing;
static VIEW g_TopView = VIEW_VOLLIST;
static VIEW g_BottomView = VIEW_GRAPH;
static int g_SplitPos = -1;
static BOOL g_Dragging;
static int g_SortColumn;
static BOOL g_SortAscending = TRUE;

BOOL
LoadStringBuffer(UINT Id, PWSTR Buffer, int Count)
{
    if (LoadStringW(g_hInst, Id, Buffer, Count) == 0)
    {
        Buffer[0] = 0;
        return FALSE;
    }
    return TRUE;
}

/* ----------------------------------------------------------------- fonts */

static VOID
CreateFonts(VOID)
{
    NONCLIENTMETRICSW Metrics;

    ZeroMemory(&Metrics, sizeof(Metrics));
    Metrics.cbSize = sizeof(Metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0))
    {
        g_hFont = CreateFontIndirectW(&Metrics.lfMessageFont);
        Metrics.lfMessageFont.lfWeight = FW_BOLD;
        g_hFontBold = CreateFontIndirectW(&Metrics.lfMessageFont);
    }
    if (g_hFont == NULL)
        g_hFont = GetStockObject(DEFAULT_GUI_FONT);
    if (g_hFontBold == NULL)
        g_hFontBold = g_hFont;
}

/* -------------------------------------------------------- selection helpers */

static BOOL
ResolveSelection(PDM_DISK *Disk, PDM_REGION *Region)
{
    PDM_REGION Found = NULL;
    PDM_DISK Owner = NULL;

    *Disk = NULL;
    *Region = NULL;
    if (!g_Sel.Valid || g_Model == NULL)
        return FALSE;
    if (g_Sel.HasRegion)
    {
        Found = DmFindRegion(g_Model, g_Sel.Device, g_Sel.Number, g_Sel.Offset, &Owner);
        if (Found == NULL)
            return FALSE;
    }
    else
    {
        ULONG i;

        for (i = 0; i < g_Model->DiskCount; i++)
        {
            if (g_Model->Disks[i]->Number == g_Sel.Number &&
                (g_Model->Disks[i]->Device == DM_DEV_CDROM) == (g_Sel.Device == DM_DEV_CDROM))
            {
                Owner = g_Model->Disks[i];
                break;
            }
        }
        if (Owner == NULL)
            return FALSE;
    }
    *Disk = Owner;
    *Region = Found;
    return TRUE;
}

static BOOL
CanShowProperties(VOID)
{
    PDM_DISK Disk;
    PDM_REGION Region;

    if (!ResolveSelection(&Disk, &Region))
        return FALSE;
    return Region == NULL || Region->Kind == DM_KIND_PRIMARY || Region->Kind == DM_KIND_LOGICAL;
}

static VOID
UpdateToolbarState(VOID)
{
    if (g_hToolbar)
        SendMessageW(g_hToolbar, TB_ENABLEBUTTON, ID_PROPERTIES, MAKELONG(CanShowProperties(), 0));
}

/* ------------------------------------------------------------ list content */

static VOID
AddColumn(HWND hList, int Index, UINT Id, int Width)
{
    LVCOLUMNW Column;
    WCHAR Text[64];

    LoadStringBuffer(Id, Text, ARRAYSIZE(Text));
    ZeroMemory(&Column, sizeof(Column));
    Column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    Column.pszText = Text;
    Column.cx = Width;
    Column.iSubItem = Index;
    SendMessageW(hList, LVM_INSERTCOLUMNW, Index, (LPARAM)&Column);
}

static VOID
SetCell(HWND hList, int Item, int Column, PCWSTR Text)
{
    LVITEMW Entry;

    ZeroMemory(&Entry, sizeof(Entry));
    Entry.iSubItem = Column;
    Entry.pszText = (PWSTR)Text;
    SendMessageW(hList, LVM_SETITEMTEXTW, Item, (LPARAM)&Entry);
}

static int CALLBACK
VolumeCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
    const DM_ROW *a = &g_Rows[lParam1], *b = &g_Rows[lParam2];
    int Result = 0;

    UNREFERENCED_PARAMETER(lParamSort);
    switch (g_SortColumn)
    {
        case 0: Result = _wcsicmp(a->Name, b->Name); break;
        case 1: Result = _wcsicmp(a->Layout, b->Layout); break;
        case 2: Result = _wcsicmp(a->Type, b->Type); break;
        case 3: Result = _wcsicmp(a->FileSystem, b->FileSystem); break;
        case 4: Result = _wcsicmp(a->Status, b->Status); break;
        case 5: Result = (a->CapacityBytes > b->CapacityBytes) - (a->CapacityBytes < b->CapacityBytes); break;
        case 6: Result = (a->FreeBytes > b->FreeBytes) - (a->FreeBytes < b->FreeBytes); break;
        case 7:
        {
            ULONGLONG pa = a->CapacityBytes ? a->FreeBytes * 1000 / a->CapacityBytes : 0;
            ULONGLONG pb = b->CapacityBytes ? b->FreeBytes * 1000 / b->CapacityBytes : 0;

            Result = (pa > pb) - (pa < pb);
            break;
        }
        case 8: Result = _wcsicmp(a->FaultTolerance, b->FaultTolerance); break;
        case 9: Result = _wcsicmp(a->Overhead, b->Overhead); break;
    }
    if (Result == 0)
        Result = (int)lParam1 - (int)lParam2;
    return g_SortAscending ? Result : -Result;
}

static VOID
PopulateVolumeList(VOID)
{
    ULONG i;

    SendMessageW(g_hVolList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hVolList);
    if (g_Rows)
        HeapFree(GetProcessHeap(), 0, g_Rows);
    g_Rows = g_Model ? DmBuildVolumeRows(g_Model, &g_RowCount) : NULL;
    if (g_Rows == NULL)
        g_RowCount = 0;

    for (i = 0; i < g_RowCount; i++)
    {
        const DM_ROW *Row = &g_Rows[i];
        LVITEMW Item;

        ZeroMemory(&Item, sizeof(Item));
        Item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        Item.iItem = (int)i;
        Item.pszText = (PWSTR)Row->Name;
        Item.lParam = (LPARAM)i;
        Item.iImage = Row->Disk->Device == DM_DEV_CDROM ? 2 : Row->Disk->Device == DM_DEV_REMOVABLE ? 1 : 0;
        SendMessageW(g_hVolList, LVM_INSERTITEMW, 0, (LPARAM)&Item);
        SetCell(g_hVolList, (int)i, 1, Row->Layout);
        SetCell(g_hVolList, (int)i, 2, Row->Type);
        SetCell(g_hVolList, (int)i, 3, Row->FileSystem);
        SetCell(g_hVolList, (int)i, 4, Row->Status);
        SetCell(g_hVolList, (int)i, 5, Row->Capacity);
        SetCell(g_hVolList, (int)i, 6, Row->Free);
        SetCell(g_hVolList, (int)i, 7, Row->PercentFree);
        SetCell(g_hVolList, (int)i, 8, Row->FaultTolerance);
        SetCell(g_hVolList, (int)i, 9, Row->Overhead);
    }
    if (g_SortColumn != 0 || !g_SortAscending)
        ListView_SortItems(g_hVolList, VolumeCompare, 0);
    SendMessageW(g_hVolList, WM_SETREDRAW, TRUE, 0);
}

static VOID
PopulateDiskList(VOID)
{
    ULONG i;

    SendMessageW(g_hDiskList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hDiskList);
    for (i = 0; g_Model && i < g_Model->DiskCount; i++)
    {
        PDM_DISK Disk = g_Model->Disks[i];
        DM_LABEL_TEXT Label;
        LVITEMW Item;
        WCHAR Capacity[32], Unallocated[32], Style[32], Type[48];

        DmLabelText(Disk, &Label);
        ZeroMemory(&Item, sizeof(Item));
        Item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        Item.iItem = (int)i;
        Item.pszText = Label.Line1;
        Item.lParam = (LPARAM)i;
        Item.iImage = Disk->Device == DM_DEV_CDROM ? 2 : Disk->Device == DM_DEV_REMOVABLE ? 1 : 0;
        SendMessageW(g_hDiskList, LVM_INSERTITEMW, 0, (LPARAM)&Item);

        if (Disk->Device == DM_DEV_CDROM)
            StringCchCopyW(Type, ARRAYSIZE(Type), Disk->MediaKind[0] ? Disk->MediaKind : L"CD-ROM");
        else if (Disk->Device == DM_DEV_REMOVABLE)
            StringCchCopyW(Type, ARRAYSIZE(Type), L"Removable");
        else
            StringCchCopyW(Type, ARRAYSIZE(Type), Disk->Dynamic ? L"Dynamic" : Disk->Style == DM_STYLE_RAW ? L"Unknown" : L"Basic");
        SetCell(g_hDiskList, (int)i, 1, Type);

        if (!Disk->HasMedia && Disk->Device != DM_DEV_DISK)
        {
            Capacity[0] = 0;
            Unallocated[0] = 0;
        }
        else
        {
            DmFormatSizeMB(Disk->CapacityMB, Capacity, ARRAYSIZE(Capacity));
            DmFormatSizeMB(Disk->UnallocatedMB, Unallocated, ARRAYSIZE(Unallocated));
        }
        SetCell(g_hDiskList, (int)i, 2, Capacity);
        SetCell(g_hDiskList, (int)i, 3, Unallocated);
        SetCell(g_hDiskList, (int)i, 4, Disk->StatusText);
        SetCell(g_hDiskList, (int)i, 5, Disk->BusText);
        LoadStringBuffer(Disk->Style == DM_STYLE_GPT ? IDS_STYLE_GPT_SHORT : Disk->Style == DM_STYLE_MBR ? IDS_STYLE_MBR_SHORT : IDS_NOT_AVAILABLE,
                         Style, ARRAYSIZE(Style));
        SetCell(g_hDiskList, (int)i, 6, Style);
    }
    SendMessageW(g_hDiskList, WM_SETREDRAW, TRUE, 0);
}

static VOID
UpdateStatusBar(VOID)
{
    WCHAR Format[64], Text[128];

    LoadStringBuffer(IDS_STATUS_COUNTS, Format, ARRAYSIZE(Format));
    StringCchPrintfW(Text, ARRAYSIZE(Text), Format, g_Model ? g_Model->DiskCount : 0, g_RowCount);
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)Text);
}

/* --------------------------------------------------- selection and the lists */

static int
FindRowForSelection(VOID)
{
    PDM_DISK Disk;
    PDM_REGION Region;
    ULONG i;

    if (!g_Sel.Valid || !g_Sel.HasRegion || !ResolveSelection(&Disk, &Region) || Region == NULL)
        return -1;
    for (i = 0; i < g_RowCount; i++)
        if (g_Rows[i].Region == Region)
            return (int)i;
    return -1;
}

static int
FindListItem(HWND hList, LPARAM Data)
{
    LVFINDINFOW Find;

    ZeroMemory(&Find, sizeof(Find));
    Find.flags = LVFI_PARAM;
    Find.lParam = Data;
    return (int)SendMessageW(hList, LVM_FINDITEMW, (WPARAM)-1, (LPARAM)&Find);
}

static VOID
SelectListItem(HWND hList, int Item)
{
    if (Item < 0)
    {
        ListView_SetItemState(hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        return;
    }
    ListView_SetItemState(hList, Item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList, Item, FALSE);
}

/* Show g_Sel in every pane except the one it came from. */
static VOID
ShowSelection(HWND hSource)
{
    int Row = FindRowForSelection();
    PDM_DISK Disk;
    PDM_REGION Region;

    g_Syncing = TRUE;
    if (hSource != g_hVolList)
        SelectListItem(g_hVolList, Row < 0 ? -1 : FindListItem(g_hVolList, Row));
    if (hSource != g_hDiskList)
    {
        int Item = -1;

        if (ResolveSelection(&Disk, &Region) && g_Model)
        {
            ULONG i;

            for (i = 0; i < g_Model->DiskCount; i++)
                if (g_Model->Disks[i] == Disk)
                    Item = FindListItem(g_hDiskList, (LPARAM)i);
        }
        SelectListItem(g_hDiskList, Item);
    }
    if (hSource != g_hGraph)
        GraphSetSelection(g_hGraph, &g_Sel);
    g_Syncing = FALSE;
    UpdateToolbarState();
}

/* ------------------------------------------------------------------ layout */

static HWND
ViewWindow(VIEW View)
{
    switch (View)
    {
        case VIEW_DISKLIST: return g_hDiskList;
        case VIEW_VOLLIST:  return g_hVolList;
        case VIEW_GRAPH:    return g_hGraph;
        default:            return NULL;
    }
}

static VOID
LayoutMain(VOID)
{
    RECT Client, rc;
    int Top, Bottom, Avail, TopH, i;
    HWND hViews[3] = { g_hDiskList, g_hVolList, g_hGraph };
    RECT Pane[2];

    if (g_hStatus == NULL)
        return;
    GetClientRect(g_hMain, &Client);
    SendMessageW(g_hStatus, WM_SIZE, 0, 0);
    SendMessageW(g_hToolbar, TB_AUTOSIZE, 0, 0);

    GetWindowRect(g_hToolbar, &rc);
    Top = rc.bottom - rc.top;
    GetWindowRect(g_hStatus, &rc);
    Bottom = Client.bottom - (rc.bottom - rc.top);
    Avail = Bottom - Top;
    if (Avail < 0)
        Avail = 0;

    if (g_BottomView == VIEW_HIDDEN)
    {
        SetRect(&Pane[0], 0, Top, Client.right, Bottom);
        SetRectEmpty(&Pane[1]);
    }
    else
    {
        if (g_SplitPos < 0)
            g_SplitPos = Avail * 38 / 100;
        TopH = g_SplitPos;
        if (TopH > Avail - MIN_PANE_H - SPLITTER_H)
            TopH = Avail - MIN_PANE_H - SPLITTER_H;
        if (TopH < MIN_PANE_H)
            TopH = MIN_PANE_H;
        SetRect(&Pane[0], 0, Top, Client.right, Top + TopH);
        SetRect(&Pane[1], 0, Top + TopH + SPLITTER_H, Client.right, Bottom);
    }

    for (i = 0; i < 3; i++)
    {
        VIEW View = (VIEW)i;
        int Slot = (g_TopView == View) ? 0 : (g_BottomView == View) ? 1 : -1;

        if (Slot < 0 || IsRectEmpty(&Pane[Slot]))
        {
            ShowWindow(hViews[i], SW_HIDE);
            continue;
        }
        SetWindowPos(hViews[i], HWND_TOP, Pane[Slot].left, Pane[Slot].top,
                     Pane[Slot].right - Pane[Slot].left, Pane[Slot].bottom - Pane[Slot].top, SWP_SHOWWINDOW);
    }
}

static VOID
GetSplitterRect(LPRECT Rect)
{
    RECT Top;
    HWND hTop = ViewWindow(g_TopView);

    SetRectEmpty(Rect);
    if (hTop == NULL || g_BottomView == VIEW_HIDDEN)
        return;
    GetWindowRect(hTop, &Top);
    MapWindowPoints(NULL, g_hMain, (LPPOINT)&Top, 2);
    SetRect(Rect, 0, Top.bottom, Top.right, Top.bottom + SPLITTER_H);
}

/* -------------------------------------------------------------- refreshing */

static VOID
Refresh(VOID)
{
    PDM_MODEL Model;
    HCURSOR OldCursor = SetCursor(LoadCursorW(NULL, IDC_WAIT));

    Model = DmCollect();
    g_Syncing = TRUE;
    if (g_Model)
        DmFreeModel(g_Model);
    g_Model = Model;
    PopulateVolumeList();
    PopulateDiskList();
    GraphSetModel(g_hGraph, g_Model);
    g_Syncing = FALSE;
    UpdateStatusBar();
    ShowSelection(NULL);
    SetCursor(OldCursor);
}

/* ------------------------------------------------------- views and commands */

static VOID
SetViews(VIEW Top, VIEW Bottom)
{
    g_TopView = Top;
    g_BottomView = Bottom;
    LayoutMain();
    {
        HWND hFocus = GetFocus();

        if (hFocus == NULL || !IsWindowVisible(hFocus))
            SetFocus(ViewWindow(g_TopView));
    }
}

static VOID
ChooseTop(VIEW View)
{
    VIEW Bottom = g_BottomView;

    if (Bottom == View)
        Bottom = g_TopView;
    SetViews(View, Bottom);
}

static VOID
ChooseBottom(VIEW View)
{
    VIEW Top = g_TopView;

    if (View != VIEW_HIDDEN && Top == View)
        Top = (g_BottomView != VIEW_HIDDEN) ? g_BottomView : (View == VIEW_VOLLIST ? VIEW_GRAPH : VIEW_VOLLIST);
    SetViews(Top, View);
}

static VOID
ShowProperties(VOID)
{
    PDM_DISK Disk;
    PDM_REGION Region;

    if (!ResolveSelection(&Disk, &Region))
        return;
    if (Region == NULL)
        ShowDiskProperties(g_hMain, Disk);
    else if (Region->Kind == DM_KIND_PRIMARY || Region->Kind == DM_KIND_LOGICAL)
        ShowRegionProperties(g_hMain, Disk, Region);
}

static VOID
ShowContextMenu(int X, int Y)
{
    HMENU hMenu = LoadMenuW(g_hInst, MAKEINTRESOURCEW(IDR_POPUP));
    HMENU hPopup;

    if (hMenu == NULL)
        return;
    hPopup = GetSubMenu(hMenu, 0);
    EnableMenuItem(hPopup, ID_PROPERTIES, MF_BYCOMMAND | (CanShowProperties() ? MF_ENABLED : MF_GRAYED));
    SetMenuDefaultItem(hPopup, ID_PROPERTIES, FALSE);
    TrackPopupMenu(hPopup, TPM_LEFTALIGN | TPM_RIGHTBUTTON, X, Y, 0, g_hMain, NULL);
    DestroyMenu(hMenu);
}

static VOID
UpdateMenus(HMENU hMenu)
{
    CheckMenuRadioItem(hMenu, ID_TOP_DISKLIST, ID_TOP_GRAPH, ID_TOP_DISKLIST + (int)g_TopView, MF_BYCOMMAND);
    CheckMenuRadioItem(hMenu, ID_BOT_DISKLIST, ID_BOT_HIDDEN, ID_BOT_DISKLIST + (int)g_BottomView, MF_BYCOMMAND);
    EnableMenuItem(hMenu, ID_PROPERTIES, MF_BYCOMMAND | (CanShowProperties() ? MF_ENABLED : MF_GRAYED));
}

static VOID
ShowAbout(VOID)
{
    WCHAR Title[64], Text[160];
    HICON Icon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_DISKMGMT));

    LoadStringBuffer(IDS_APPTITLE, Title, ARRAYSIZE(Title));
    LoadStringBuffer(IDS_ABOUT_TEXT, Text, ARRAYSIZE(Text));
    ShellAboutW(g_hMain, Title, Text, Icon);
}

/* ---------------------------------------------------------------- creation */

static HWND
CreateList(UINT Id)
{
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
                                 WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 0, 0, 0, 0, g_hMain, (HMENU)(UINT_PTR)Id, g_hInst, NULL);

    if (hList)
    {
        SendMessageW(hList, WM_SETFONT, (WPARAM)g_hFont, FALSE);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
        ListView_SetImageList(hList, g_hImages, LVSIL_SMALL);
    }
    return hList;
}

static BOOL
CreateImages(VOID)
{
    static const int Ids[] = { -9, -8, -12 };
    int i;

    g_hImages = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ILC_MASK | ILC_COLOR32, 3, 1);
    if (g_hImages == NULL)
        return FALSE;
    for (i = 0; i < 3; i++)
    {
        HICON Icon = NULL;

        ExtractIconExW(L"shell32.dll", Ids[i], NULL, &Icon, 1);
        if (Icon)
        {
            ImageList_AddIcon(g_hImages, Icon);
            DestroyIcon(Icon);
        }
        else
        {
            ImageList_AddIcon(g_hImages, LoadIconW(NULL, IDI_APPLICATION));
        }
    }
    return TRUE;
}

static HIMAGELIST
CreateToolbarImages(VOID)
{
    static const UINT Bitmaps[] = { IDB_PROP, IDB_REFRESH };
    HIMAGELIST hList = ImageList_Create(16, 16, ILC_MASK | ILC_COLOR32, 2, 0);
    int i;

    if (hList == NULL)
        return NULL;
    for (i = 0; i < 2; i++)
    {
        HANDLE hBitmap = LoadImageW(g_hInst, MAKEINTRESOURCEW(Bitmaps[i]), IMAGE_BITMAP, 16, 16, LR_LOADTRANSPARENT);

        if (hBitmap == NULL)
            break;
        ImageList_AddMasked(hList, hBitmap, RGB(255, 0, 128));
        DeleteObject(hBitmap);
    }
    return hList;
}

static BOOL
CreateChildren(VOID)
{
    static const TBBUTTON Buttons[] =
    {
        { 0, ID_PROPERTIES, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, 0 },
        { 1, ID_REFRESH, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, 0 },
    };
    HIMAGELIST hToolImages;

    g_hToolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
                                 WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,
                                 0, 0, 0, 0, g_hMain, (HMENU)IDC_TOOLBAR, g_hInst, NULL);
    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                0, 0, 0, 0, g_hMain, (HMENU)IDC_STATUSBAR, g_hInst, NULL);
    if (g_hToolbar == NULL || g_hStatus == NULL || !CreateImages())
        return FALSE;

    SendMessageW(g_hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    hToolImages = CreateToolbarImages();
    if (hToolImages)
        SendMessageW(g_hToolbar, TB_SETIMAGELIST, 0, (LPARAM)hToolImages);
    SendMessageW(g_hToolbar, TB_ADDBUTTONS, ARRAYSIZE(Buttons), (LPARAM)Buttons);

    g_hVolList = CreateList(IDC_VOLLIST);
    g_hDiskList = CreateList(IDC_DISKLIST);
    g_hGraph = GraphCreate(g_hMain, IDC_GRAPH);
    if (g_hVolList == NULL || g_hDiskList == NULL || g_hGraph == NULL)
        return FALSE;

    AddColumn(g_hVolList, 0, IDS_COL_VOLUME, 130);
    AddColumn(g_hVolList, 1, IDS_COL_LAYOUT, 70);
    AddColumn(g_hVolList, 2, IDS_COL_TYPE, 60);
    AddColumn(g_hVolList, 3, IDS_COL_FS, 80);
    AddColumn(g_hVolList, 4, IDS_COL_STATUS, 250);
    AddColumn(g_hVolList, 5, IDS_COL_CAPACITY, 80);
    AddColumn(g_hVolList, 6, IDS_COL_FREE, 80);
    AddColumn(g_hVolList, 7, IDS_COL_PCTFREE, 60);
    AddColumn(g_hVolList, 8, IDS_COL_FAULT, 100);
    AddColumn(g_hVolList, 9, IDS_COL_OVERHEAD, 70);

    AddColumn(g_hDiskList, 0, IDS_COL_DISK, 100);
    AddColumn(g_hDiskList, 1, IDS_COL_TYPE, 90);
    AddColumn(g_hDiskList, 2, IDS_COL_CAPACITY, 90);
    AddColumn(g_hDiskList, 3, IDS_COL_UNALLOC, 120);
    AddColumn(g_hDiskList, 4, IDS_COL_STATUS, 100);
    AddColumn(g_hDiskList, 5, IDS_COL_DEVTYPE, 100);
    AddColumn(g_hDiskList, 6, IDS_COL_STYLE, 110);
    return TRUE;
}

static VOID
OnListNotify(NMHDR *Header)
{
    NMLISTVIEW *List = (NMLISTVIEW *)Header;
    HWND hList = Header->hwndFrom;

    switch (Header->code)
    {
        case LVN_ITEMCHANGED:
        {
            if (g_Syncing || !(List->uChanged & LVIF_STATE) || !(List->uNewState & LVIS_SELECTED))
                break;
            ZeroMemory(&g_Sel, sizeof(g_Sel));
            if (hList == g_hVolList)
            {
                LVITEMW Item;

                ZeroMemory(&Item, sizeof(Item));
                Item.mask = LVIF_PARAM;
                Item.iItem = List->iItem;
                SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&Item);
                if ((ULONG)Item.lParam < g_RowCount)
                {
                    const DM_ROW *Row = &g_Rows[Item.lParam];

                    g_Sel.Valid = TRUE;
                    g_Sel.Device = Row->Disk->Device;
                    g_Sel.Number = Row->Disk->Number;
                    g_Sel.HasRegion = TRUE;
                    g_Sel.Offset = Row->Region->Offset;
                }
            }
            else
            {
                LVITEMW Item;

                ZeroMemory(&Item, sizeof(Item));
                Item.mask = LVIF_PARAM;
                Item.iItem = List->iItem;
                SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&Item);
                if (g_Model && (ULONG)Item.lParam < g_Model->DiskCount)
                {
                    g_Sel.Valid = TRUE;
                    g_Sel.Device = g_Model->Disks[Item.lParam]->Device;
                    g_Sel.Number = g_Model->Disks[Item.lParam]->Number;
                }
            }
            ShowSelection(hList);
            break;
        }

        case LVN_COLUMNCLICK:
            if (hList == g_hVolList)
            {
                if (g_SortColumn == List->iSubItem)
                    g_SortAscending = !g_SortAscending;
                else
                {
                    g_SortColumn = List->iSubItem;
                    g_SortAscending = TRUE;
                }
                ListView_SortItems(g_hVolList, VolumeCompare, 0);
            }
            break;

        case NM_DBLCLK:
        case LVN_ITEMACTIVATE:
            ShowProperties();
            break;

        case NM_RCLICK:
        {
            DWORD Pos = GetMessagePos();

            ShowContextMenu((short)LOWORD(Pos), (short)HIWORD(Pos));
            break;
        }
    }
}

static LRESULT CALLBACK
MainWndProc(HWND hWnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
    switch (Message)
    {
        case WM_CREATE:
            g_hMain = hWnd;
            if (!CreateChildren())
                return -1;
            return 0;

        case WM_SIZE:
            LayoutMain();
            return 0;

        case WM_SETFOCUS:
            if (ViewWindow(g_TopView))
                SetFocus(ViewWindow(g_TopView));
            return 0;

        case WM_INITMENUPOPUP:
            UpdateMenus(GetMenu(hWnd));
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case ID_EXIT:           DestroyWindow(hWnd); break;
                case ID_REFRESH:        Refresh(); break;
                case ID_PROPERTIES:     ShowProperties(); break;
                case ID_TOP_DISKLIST:   ChooseTop(VIEW_DISKLIST); break;
                case ID_TOP_VOLLIST:    ChooseTop(VIEW_VOLLIST); break;
                case ID_TOP_GRAPH:      ChooseTop(VIEW_GRAPH); break;
                case ID_BOT_DISKLIST:   ChooseBottom(VIEW_DISKLIST); break;
                case ID_BOT_VOLLIST:    ChooseBottom(VIEW_VOLLIST); break;
                case ID_BOT_GRAPH:      ChooseBottom(VIEW_GRAPH); break;
                case ID_BOT_HIDDEN:     ChooseBottom(VIEW_HIDDEN); break;
                case ID_ABOUT:          ShowAbout(); break;
            }
            return 0;

        case WM_NOTIFY:
        {
            NMHDR *Header = (NMHDR *)lParam;

            if (Header->hwndFrom == g_hVolList || Header->hwndFrom == g_hDiskList)
            {
                OnListNotify(Header);
                return 0;
            }
            if (Header->code == TTN_GETDISPINFOW)
            {
                NMTTDISPINFOW *Tip = (NMTTDISPINFOW *)lParam;

                Tip->hinst = g_hInst;
                Tip->lpszText = MAKEINTRESOURCEW(Tip->hdr.idFrom == ID_REFRESH ? IDS_TIP_REFRESH : IDS_TIP_PROPERTIES);
                return 0;
            }
            break;
        }

        case DMM_SELCHANGE:
            GraphGetSelection(g_hGraph, &g_Sel);
            ShowSelection(g_hGraph);
            return 0;

        case DMM_ACTIVATE:
            ShowProperties();
            return 0;

        case DMM_CONTEXT:
            ShowContextMenu((short)LOWORD(lParam), (short)HIWORD(lParam));
            return 0;

        case WM_CONTEXTMENU:
            if ((HWND)wParam == g_hVolList || (HWND)wParam == g_hDiskList)
            {
                int X = (short)LOWORD(lParam), Y = (short)HIWORD(lParam);

                if (X == -1 && Y == -1)
                {
                    RECT rc;

                    GetWindowRect((HWND)wParam, &rc);
                    X = rc.left + 20;
                    Y = rc.top + 20;
                }
                ShowContextMenu(X, Y);
                return 0;
            }
            break;

        case WM_SETCURSOR:
        {
            RECT Split;
            POINT pt;

            GetSplitterRect(&Split);
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            if (!IsRectEmpty(&Split) && PtInRect(&Split, pt))
            {
                SetCursor(LoadCursorW(NULL, IDC_SIZENS));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN:
        {
            RECT Split;
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

            GetSplitterRect(&Split);
            if (!IsRectEmpty(&Split) && PtInRect(&Split, pt))
            {
                g_Dragging = TRUE;
                SetCapture(hWnd);
            }
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g_Dragging)
            {
                RECT Tool;
                int Y = (short)HIWORD(lParam);

                GetWindowRect(g_hToolbar, &Tool);
                g_SplitPos = Y - (Tool.bottom - Tool.top) - SPLITTER_H / 2;
                LayoutMain();
            }
            return 0;

        case WM_LBUTTONUP:
            if (g_Dragging)
            {
                g_Dragging = FALSE;
                ReleaseCapture();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, Message, wParam, lParam);
}

/* ------------------------------------------------------------------- dump */

static int
DumpTo(PCWSTR Path)
{
    PDM_MODEL Model = DmCollect();
    HANDLE File;

    if (Model == NULL)
        return 2;
    File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        DmFreeModel(Model);
        return 3;
    }
    DmDumpModel(Model, File);
    CloseHandle(File);
    DmFreeModel(Model);
    return 0;
}

static VOID
CycleFocus(BOOL Backwards)
{
    HWND Views[2], hFocus = GetFocus();
    int Count = 0, i, Current = -1;

    if (IsWindowVisible(ViewWindow(g_TopView)))
        Views[Count++] = ViewWindow(g_TopView);
    if (g_BottomView != VIEW_HIDDEN && IsWindowVisible(ViewWindow(g_BottomView)))
        Views[Count++] = ViewWindow(g_BottomView);
    for (i = 0; i < Count; i++)
        if (Views[i] == hFocus)
            Current = i;
    if (Count > 0)
        SetFocus(Views[(Current + (Backwards ? Count - 1 : 1) + Count) % Count]);
}

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX Controls;
    WNDCLASSEXW wc;
    HACCEL hAccel;
    WCHAR Title[64];
    MSG Msg;
    int Argc = 0, i;
    PWSTR *Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    g_hInst = hInstance;

    for (i = 1; Argv && i + 1 < Argc; i++)
    {
        if (_wcsicmp(Argv[i], L"/dump") == 0 || _wcsicmp(Argv[i], L"-dump") == 0)
        {
            int Result = DumpTo(Argv[i + 1]);

            LocalFree(Argv);
            return Result;
        }
    }
    if (Argv)
        LocalFree(Argv);

    Controls.dwSize = sizeof(Controls);
    Controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&Controls);
    CreateFonts();
    if (!GraphRegisterClass(hInstance))
        return 1;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_DISKMGMT));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_DISKMGMT), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = MAIN_CLASS;
    if (!RegisterClassExW(&wc))
        return 1;

    LoadStringBuffer(IDS_APPTITLE, Title, ARRAYSIZE(Title));
    g_hMain = CreateWindowExW(0, MAIN_CLASS, Title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 900, 620, NULL, NULL, hInstance, NULL);
    if (g_hMain == NULL)
        return 1;

    LayoutMain();
    Refresh();
    ShowWindow(g_hMain, nCmdShow);
    UpdateWindow(g_hMain);
    SetFocus(ViewWindow(g_TopView));

    hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));
    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (Msg.message == WM_KEYDOWN && Msg.wParam == VK_TAB && GetAncestor(Msg.hwnd, GA_ROOT) == g_hMain &&
            GetKeyState(VK_CONTROL) >= 0)
        {
            CycleFocus(GetKeyState(VK_SHIFT) < 0);
            continue;
        }
        if (!TranslateAcceleratorW(g_hMain, hAccel, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }
    return (int)Msg.wParam;
}
