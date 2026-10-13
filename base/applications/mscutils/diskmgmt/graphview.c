/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The graphical disk map
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <math.h>
#include "diskmgmt.h"

#define GRAPH_CLASS     L"DiskMgmtGraphView"

#define ROW_PITCH       86
#define ROW_H           85
#define ROW_TOP         3
#define LABEL_X         2
#define LABEL_W         99
#define REGION_X        103
#define REGION_DY       4
#define REGION_H        78
#define HEADER_H        12
#define RIGHT_MARGIN    6
#define MIN_REGION_W    24
#define LEGEND_H        22

#define COLOR_UNALLOCATED   RGB(0, 0, 0)
#define COLOR_PRIMARY       RGB(0, 0, 128)
#define COLOR_EXTENDED      RGB(0, 128, 0)
#define COLOR_FREE          RGB(0, 255, 0)
#define COLOR_LOGICAL       RGB(0, 0, 255)

typedef struct _GITEM
{
    RECT rc;
    int Disk;
    int Region;             /* -1: the label box of the disk */
} GITEM;

typedef struct _GEXT
{
    BOOL Has;
    RECT Frame;
} GEXT;

typedef struct _GRAPH
{
    PDM_MODEL Model;
    GITEM *Items;
    int ItemCount;
    GEXT *Ext;
    int ContentHeight;
    int ScrollY;
    DM_SELECTION Sel;
    HICON IconFixed;
    HICON IconCd;
    HICON IconRemovable;
} GRAPH;

static GRAPH *
GetGraph(HWND hWnd)
{
    return (GRAPH *)GetWindowLongPtrW(hWnd, 0);
}

static void
GraphFreeLayout(GRAPH *G)
{
    if (G->Items)
        HeapFree(GetProcessHeap(), 0, G->Items);
    if (G->Ext)
        HeapFree(GetProcessHeap(), 0, G->Ext);
    G->Items = NULL;
    G->Ext = NULL;
    G->ItemCount = 0;
}

static double
Weight(ULONGLONG Bytes)
{
    ULONGLONG MB = Bytes / DM_MIB;

    if (MB < 3)
        return 1.0;
    return log((double)MB);
}

static int
VisibleHeight(HWND hWnd)
{
    RECT rc;

    GetClientRect(hWnd, &rc);
    return max(rc.bottom - LEGEND_H, 0);
}

static void
UpdateScrollBar(HWND hWnd, GRAPH *G)
{
    SCROLLINFO Si;
    int Page = VisibleHeight(hWnd);
    int Max = max(G->ContentHeight - Page, 0);

    if (G->ScrollY > Max)
        G->ScrollY = Max;
    ZeroMemory(&Si, sizeof(Si));
    Si.cbSize = sizeof(Si);
    Si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    Si.nMin = 0;
    Si.nMax = max(G->ContentHeight - 1, 0);
    Si.nPage = Page;
    Si.nPos = G->ScrollY;
    SetScrollInfo(hWnd, SB_VERT, &Si, TRUE);
}

static void
GraphLayout(HWND hWnd, GRAPH *G)
{
    RECT rc;
    int Avail, d, r, n = 0;
    double MaxLn = 1.0;

    GraphFreeLayout(G);
    G->ContentHeight = 0;
    if (G->Model == NULL || G->Model->DiskCount == 0)
    {
        UpdateScrollBar(hWnd, G);
        return;
    }

    GetClientRect(hWnd, &rc);
    Avail = rc.right - REGION_X - RIGHT_MARGIN;
    if (Avail < 120)
        Avail = 120;

    for (d = 0; d < (int)G->Model->DiskCount; d++)
    {
        PDM_DISK Disk = G->Model->Disks[d];

        n += 1 + (int)Disk->RegionCount;
        if (Disk->Device != DM_DEV_CDROM && Weight(Disk->CapacityMB * DM_MIB) > MaxLn)
            MaxLn = Weight(Disk->CapacityMB * DM_MIB);
    }
    G->Items = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(GITEM));
    G->Ext = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, G->Model->DiskCount * sizeof(GEXT));
    if (G->Items == NULL || G->Ext == NULL)
    {
        GraphFreeLayout(G);
        return;
    }

    for (d = 0; d < (int)G->Model->DiskCount; d++)
    {
        PDM_DISK Disk = G->Model->Disks[d];
        int Top = ROW_TOP + d * ROW_PITCH;
        double Sum = 0.0;
        int BarWidth, MinWidth, X = REGION_X;
        int *Widths;
        GITEM *Item = &G->Items[G->ItemCount++];

        Item->Disk = d;
        Item->Region = -1;
        SetRect(&Item->rc, LABEL_X, Top, LABEL_X + LABEL_W, Top + ROW_H);

        for (r = 0; r < (int)Disk->RegionCount; r++)
            Sum += Weight(Disk->Regions[r].Length);
        Widths = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (Disk->RegionCount + 1) * sizeof(int));
        if (Widths == NULL)
        {
            GraphFreeLayout(G);
            return;
        }

        if (Disk->Device == DM_DEV_CDROM)
            BarWidth = max(Avail * 40 / 100, 220);
        else
            BarWidth = (int)(Avail * Weight(Disk->CapacityMB * DM_MIB) / MaxLn);
        /* many blocks shrink the minimum width so that the bar still fits */
        MinWidth = Disk->RegionCount ? max(3, min(MIN_REGION_W, Avail / (int)Disk->RegionCount)) : MIN_REGION_W;
        BarWidth = max(BarWidth, (int)Disk->RegionCount * MinWidth);
        if (BarWidth > Avail)
            BarWidth = Avail;

        /* widths in proportion to the log of the size, then every block at least MinWidth, and the bar fitted */
        {
            int Total = 0;

            for (r = 0; r < (int)Disk->RegionCount; r++)
            {
                Widths[r] = Sum > 0 ? (int)(BarWidth * Weight(Disk->Regions[r].Length) / Sum) : BarWidth;
                if (Widths[r] < MinWidth)
                    Widths[r] = MinWidth;
                Total += Widths[r];
            }
            if (Total > BarWidth)
            {
                int Spare = 0, Excess = Total - BarWidth;

                for (r = 0; r < (int)Disk->RegionCount; r++)
                    Spare += Widths[r] - MinWidth;
                for (r = 0; r < (int)Disk->RegionCount && Spare > 0; r++)
                    Widths[r] -= (int)((long long)(Widths[r] - MinWidth) * (Excess < Spare ? Excess : Spare) / Spare);
            }
            else if (Disk->RegionCount > 0)
            {
                Widths[Disk->RegionCount - 1] += BarWidth - Total;
            }
        }

        for (r = 0; r < (int)Disk->RegionCount; r++)
        {
            int Width = Widths[r];

            Item = &G->Items[G->ItemCount++];
            Item->Disk = d;
            Item->Region = r;
            SetRect(&Item->rc, X, Top + REGION_DY, X + Width, Top + REGION_DY + REGION_H);
            X += Width;

            if (Disk->Regions[r].InExtended)
            {
                RECT *Frame = &G->Ext[d].Frame;

                if (!G->Ext[d].Has)
                {
                    G->Ext[d].Has = TRUE;
                    *Frame = Item->rc;
                }
                else
                {
                    UnionRect(Frame, Frame, &Item->rc);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, Widths);
        if (G->Ext[d].Has)
            InflateRect(&G->Ext[d].Frame, 3, 3);
    }

    G->ContentHeight = ROW_TOP + (int)G->Model->DiskCount * ROW_PITCH + 2;
    UpdateScrollBar(hWnd, G);
}

static int
FindItem(GRAPH *G, const DM_SELECTION *Sel)
{
    int i;

    if (!Sel->Valid || G->Model == NULL)
        return -1;
    for (i = 0; i < G->ItemCount; i++)
    {
        PDM_DISK Disk = G->Model->Disks[G->Items[i].Disk];

        if (Disk->Number != Sel->Number || (Disk->Device == DM_DEV_CDROM) != (Sel->Device == DM_DEV_CDROM))
            continue;
        if (!Sel->HasRegion && G->Items[i].Region == -1)
            return i;
        if (Sel->HasRegion && G->Items[i].Region >= 0 && Disk->Regions[G->Items[i].Region].Offset == Sel->Offset)
            return i;
    }
    return -1;
}

static void
ItemToSelection(GRAPH *G, int Index, PDM_SELECTION Sel)
{
    PDM_DISK Disk = G->Model->Disks[G->Items[Index].Disk];

    ZeroMemory(Sel, sizeof(*Sel));
    Sel->Valid = TRUE;
    Sel->Device = Disk->Device;
    Sel->Number = Disk->Number;
    if (G->Items[Index].Region >= 0)
    {
        Sel->HasRegion = TRUE;
        Sel->Offset = Disk->Regions[G->Items[Index].Region].Offset;
    }
}

static void
EnsureVisible(HWND hWnd, GRAPH *G, int Index)
{
    int Page = VisibleHeight(hWnd);
    RECT *rc;
    int Row;

    if (Index < 0)
        return;
    rc = &G->Items[Index].rc;
    Row = rc->top - 1;
    if (Row < G->ScrollY)
        G->ScrollY = Row;
    else if (rc->bottom + 2 > G->ScrollY + Page)
        G->ScrollY = rc->bottom + 2 - Page;
    if (G->ScrollY < 0)
        G->ScrollY = 0;
    UpdateScrollBar(hWnd, G);
}

static void
Notify(HWND hWnd, UINT Message, LPARAM lParam)
{
    SendMessageW(GetParent(hWnd), Message, (WPARAM)GetDlgCtrlID(hWnd), lParam);
}

static void
SelectItem(HWND hWnd, GRAPH *G, int Index, BOOL Notification)
{
    DM_SELECTION Sel;

    if (Index < 0 || Index >= G->ItemCount)
        return;
    ItemToSelection(G, Index, &Sel);
    G->Sel = Sel;
    EnsureVisible(hWnd, G, Index);
    InvalidateRect(hWnd, NULL, FALSE);
    if (Notification)
        Notify(hWnd, DMM_SELCHANGE, 0);
}

static int
HitTest(GRAPH *G, int X, int Y)
{
    POINT pt;
    int i;

    pt.x = X;
    pt.y = Y + G->ScrollY;
    for (i = 0; i < G->ItemCount; i++)
        if (PtInRect(&G->Items[i].rc, pt))
            return i;
    return -1;
}

/* ------------------------------------------------------------------ painting */

static COLORREF
HeaderColor(const DM_REGION *Region)
{
    switch (Region->Kind)
    {
        case DM_KIND_UNALLOCATED: return COLOR_UNALLOCATED;
        case DM_KIND_LOGICAL:     return COLOR_LOGICAL;
        case DM_KIND_FREE:        return COLOR_FREE;
        default:                  return COLOR_PRIMARY;
    }
}

static void
FillColor(HDC hDC, const RECT *rc, COLORREF Color)
{
    HBRUSH Brush = CreateSolidBrush(Color);

    FillRect(hDC, rc, Brush);
    DeleteObject(Brush);
}

static void
DrawRegion(HDC hDC, GRAPH *G, const GITEM *Item, BOOL Selected)
{
    PDM_DISK Disk = G->Model->Disks[Item->Disk];
    PDM_REGION Region = &Disk->Regions[Item->Region];
    const RECT *B = &Item->rc;
    RECT rc, Body;
    DM_BLOCK_TEXT Text;
    HPEN Gray = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
    HPEN White = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ OldPen;
    int Saved;

    FillColor(hDC, B, GetSysColor(COLOR_BTNFACE));

    /* header stripe */
    SetRect(&rc, B->left + 1, B->top + 1, B->right - 2, B->top + 1 + HEADER_H - 1);
    FillColor(hDC, &rc, HeaderColor(Region));

    /* the body frame and body */
    SetRect(&Body, B->left + 3, B->top + 16, B->right - 5, B->bottom - 6);
    if (Body.right <= Body.left)
        Body.right = Body.left + 1;
    FillColor(hDC, &Body, GetSysColor(COLOR_WINDOW));
    if (Selected)
    {
        HBRUSH Hatch = CreateHatchBrush(HS_BDIAGONAL, RGB(128, 128, 128));

        SetBkColor(hDC, GetSysColor(COLOR_WINDOW));
        FillRect(hDC, &Body, Hatch);
        DeleteObject(Hatch);
    }

    OldPen = SelectObject(hDC, Gray);
    MoveToEx(hDC, Body.left - 1, Body.bottom, NULL);
    LineTo(hDC, Body.left - 1, Body.top - 1);
    LineTo(hDC, Body.right, Body.top - 1);
    if (Selected)
    {
        MoveToEx(hDC, Body.left - 2, Body.bottom + 1, NULL);
        LineTo(hDC, Body.left - 2, Body.top - 2);
        LineTo(hDC, Body.right + 1, Body.top - 2);
        LineTo(hDC, Body.right + 1, Body.bottom + 1);
        LineTo(hDC, Body.left - 2, Body.bottom + 1);
    }
    SelectObject(hDC, White);
    MoveToEx(hDC, Body.right, Body.top - 1, NULL);
    LineTo(hDC, Body.right, Body.bottom);
    LineTo(hDC, Body.left - 1, Body.bottom);

    /* box edge: white left/top, gray shadow right/bottom */
    MoveToEx(hDC, B->left, B->bottom - 1, NULL);
    LineTo(hDC, B->left, B->top);
    LineTo(hDC, B->right - 2, B->top);
    SelectObject(hDC, Gray);
    MoveToEx(hDC, B->right - 2, B->top, NULL);
    LineTo(hDC, B->right - 2, B->bottom - 1);
    MoveToEx(hDC, B->right - 1, B->top, NULL);
    LineTo(hDC, B->right - 1, B->bottom - 1);
    SelectObject(hDC, OldPen);
    DeleteObject(Gray);
    DeleteObject(White);

    /* the text of the block */
    DmBlockText(Disk, Region, &Text);
    Saved = SaveDC(hDC);
    IntersectClipRect(hDC, Body.left + 1, Body.top + 1, Body.right - 1, Body.bottom - 1);
    SetBkMode(hDC, TRANSPARENT);
    SetTextColor(hDC, GetSysColor(COLOR_WINDOWTEXT));
    {
        int Y = B->top + 19;
        RECT Line;

        if (Text.Line1[0])
        {
            SelectObject(hDC, g_hFontBold);
            SetRect(&Line, Body.left + 3, Y, Body.right, Y + 13);
            DrawTextW(hDC, Text.Line1, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP | DT_VCENTER);
        }
        SelectObject(hDC, g_hFont);
        SetRect(&Line, Body.left + 3, Y + 13, Body.right, Y + 26);
        DrawTextW(hDC, Text.Line2, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP | DT_VCENTER);
        SetRect(&Line, Body.left + 3, Y + 26, Body.right, Y + 39);
        DrawTextW(hDC, Text.Line3, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP | DT_VCENTER);
    }
    RestoreDC(hDC, Saved);
}

static void
DrawLabel(HDC hDC, GRAPH *G, const GITEM *Item, BOOL Selected)
{
    PDM_DISK Disk = G->Model->Disks[Item->Disk];
    DM_LABEL_TEXT Text;
    RECT rc = Item->rc;
    HICON Icon = Disk->Device == DM_DEV_CDROM ? G->IconCd : Disk->Device == DM_DEV_REMOVABLE ? G->IconRemovable : G->IconFixed;
    int Y = rc.top + 8, Saved = SaveDC(hDC);

    FillColor(hDC, &rc, GetSysColor(COLOR_BTNFACE));
    DrawEdge(hDC, &rc, Selected ? EDGE_SUNKEN : BDR_SUNKENOUTER, BF_RECT);
    if (Selected)
    {
        InflateRect(&rc, -2, -2);
        FillColor(hDC, &rc, GetSysColor(COLOR_3DLIGHT));
    }
    SetBkMode(hDC, TRANSPARENT);
    SetTextColor(hDC, GetSysColor(COLOR_BTNTEXT));
    DmLabelText(Disk, &Text);

    if (Icon)
        DrawIconEx(hDC, Item->rc.left + 9, Y - 1, Icon, 16, 16, 0, NULL, DI_NORMAL);
    {
        RECT Line;

        SelectObject(hDC, g_hFontBold);
        SetRect(&Line, Item->rc.left + 28, Y, Item->rc.right - 2, Y + 14);
        DrawTextW(hDC, Text.Line1, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
        SelectObject(hDC, g_hFont);
        SetRect(&Line, Item->rc.left + 8, Y + 21, Item->rc.right - 2, Y + 34);
        DrawTextW(hDC, Text.Line2, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
        SetRect(&Line, Item->rc.left + 8, Y + 34, Item->rc.right - 2, Y + 47);
        DrawTextW(hDC, Text.Line3, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
        SetRect(&Line, Item->rc.left + 8, Y + 47, Item->rc.right - 2, Y + 60);
        DrawTextW(hDC, Text.Line4, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
    }
    RestoreDC(hDC, Saved);
}

static void
DrawLegend(HDC hDC, const RECT *Client)
{
    static const struct { UINT Id; COLORREF Color; } Entries[] =
    {
        { IDS_LEGEND_UNALLOC,   COLOR_UNALLOCATED },
        { IDS_LEGEND_PRIMARY,   COLOR_PRIMARY },
        { IDS_LEGEND_EXTENDED,  COLOR_EXTENDED },
        { IDS_LEGEND_FREE,      COLOR_FREE },
        { IDS_LEGEND_LOGICAL,   COLOR_LOGICAL },
    };
    int X = 7, Y = Client->bottom - LEGEND_H + 5, i;
    int Saved = SaveDC(hDC);

    SelectObject(hDC, g_hFontBold);
    SetBkMode(hDC, TRANSPARENT);
    SetTextColor(hDC, GetSysColor(COLOR_BTNTEXT));
    for (i = 0; i < (int)ARRAYSIZE(Entries); i++)
    {
        WCHAR Text[64];
        RECT Box, Line;
        SIZE Size;

        LoadStringBuffer(Entries[i].Id, Text, ARRAYSIZE(Text));
        SetRect(&Box, X, Y, X + 11, Y + 11);
        FillColor(hDC, &Box, Entries[i].Color);
        GetTextExtentPoint32W(hDC, Text, (int)wcslen(Text), &Size);
        SetRect(&Line, X + 15, Y - 2, X + 15 + Size.cx + 2, Y + 14);
        DrawTextW(hDC, Text, -1, &Line, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
        X += 15 + Size.cx + 14;
    }
    RestoreDC(hDC, Saved);
}

static void
GraphPaint(HWND hWnd, GRAPH *G, HDC hTarget, const RECT *Update)
{
    RECT Client;
    HDC hDC;
    HBITMAP Bitmap, OldBitmap;
    int i, d, Visible, Selected;
    HFONT OldFont;

    GetClientRect(hWnd, &Client);
    if (Client.right <= 0 || Client.bottom <= 0)
        return;
    if (G->Model == NULL || G->Items == NULL || G->Ext == NULL)
    {
        FillColor(hTarget, &Client, GetSysColor(COLOR_BTNFACE));
        return;
    }
    Selected = FindItem(G, &G->Sel);
    hDC = CreateCompatibleDC(hTarget);
    Bitmap = CreateCompatibleBitmap(hTarget, Client.right, Client.bottom);
    OldBitmap = SelectObject(hDC, Bitmap);
    OldFont = SelectObject(hDC, g_hFont);

    FillColor(hDC, &Client, GetSysColor(COLOR_BTNFACE));

    Visible = VisibleHeight(hWnd);
    {
        int Saved = SaveDC(hDC);

        IntersectClipRect(hDC, 0, 0, Client.right, Visible);
        SetViewportOrgEx(hDC, 0, -G->ScrollY, NULL);

        for (i = 0; i < G->ItemCount; i++)
        {
            const GITEM *Item = &G->Items[i];
            PDM_DISK Disk = G->Model->Disks[Item->Disk];

            if (Item->rc.bottom < G->ScrollY || Item->rc.top > G->ScrollY + Visible)
                continue;
            if (Item->Region == -1)
                DrawLabel(hDC, G, Item, i == Selected);
            else if (!Disk->Regions[Item->Region].InExtended)
                DrawRegion(hDC, G, Item, i == Selected);
        }
        /* the frames of the extended partitions go under the logical drives */
        for (d = 0; d < (int)G->Model->DiskCount; d++)
        {
            if (G->Ext[d].Has)
                FillColor(hDC, &G->Ext[d].Frame, COLOR_EXTENDED);
        }
        for (i = 0; i < G->ItemCount; i++)
        {
            const GITEM *Item = &G->Items[i];
            PDM_DISK Disk = G->Model->Disks[Item->Disk];

            if (Item->Region >= 0 && Disk->Regions[Item->Region].InExtended &&
                Item->rc.bottom >= G->ScrollY && Item->rc.top <= G->ScrollY + Visible)
            {
                DrawRegion(hDC, G, Item, i == Selected);
            }
        }
        RestoreDC(hDC, Saved);
    }

    DrawLegend(hDC, &Client);
    BitBlt(hTarget, Update->left, Update->top, Update->right - Update->left, Update->bottom - Update->top,
           hDC, Update->left, Update->top, SRCCOPY);
    SelectObject(hDC, OldFont);
    SelectObject(hDC, OldBitmap);
    DeleteObject(Bitmap);
    DeleteDC(hDC);
}

/* ---------------------------------------------------------------- navigation */

static int
CenterX(const GITEM *Item)
{
    return (Item->rc.left + Item->rc.right) / 2;
}

static int
Navigate(GRAPH *G, int Current, UINT Key)
{
    int i, Best = -1, Distance = 0x7FFFFFFF;

    if (G->ItemCount == 0)
        return -1;
    if (Current < 0)
        return 0;

    switch (Key)
    {
        case VK_LEFT:
            return (Current > 0 && G->Items[Current - 1].Disk == G->Items[Current].Disk) ? Current - 1 : Current;
        case VK_RIGHT:
            return (Current + 1 < G->ItemCount && G->Items[Current + 1].Disk == G->Items[Current].Disk) ? Current + 1 : Current;
        case VK_HOME:
            return 0;
        case VK_END:
            return G->ItemCount - 1;
        case VK_UP:
        case VK_DOWN:
        {
            int Disk = G->Items[Current].Disk + (Key == VK_UP ? -1 : 1);
            int X = CenterX(&G->Items[Current]);

            for (i = 0; i < G->ItemCount; i++)
            {
                if (G->Items[i].Disk != Disk)
                    continue;
                if (G->Items[Current].Region == -1)
                {
                    if (G->Items[i].Region == -1)
                        return i;
                    continue;
                }
                if (G->Items[i].Region >= 0 && abs(CenterX(&G->Items[i]) - X) < Distance)
                {
                    Distance = abs(CenterX(&G->Items[i]) - X);
                    Best = i;
                }
            }
            return Best >= 0 ? Best : Current;
        }
    }
    return Current;
}

/* -------------------------------------------------------------- window class */

static LRESULT CALLBACK
GraphWndProc(HWND hWnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
    GRAPH *G = GetGraph(hWnd);

    switch (Message)
    {
        case WM_CREATE:
        {
            G = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GRAPH));
            if (G == NULL)
                return -1;
            SetWindowLongPtrW(hWnd, 0, (LONG_PTR)G);
            ExtractIconExW(L"shell32.dll", -9, NULL, &G->IconFixed, 1);
            ExtractIconExW(L"shell32.dll", -12, NULL, &G->IconCd, 1);
            ExtractIconExW(L"shell32.dll", -8, NULL, &G->IconRemovable, 1);
            return 0;
        }

        case WM_DESTROY:
            if (G)
            {
                GraphFreeLayout(G);
                if (G->IconFixed)
                    DestroyIcon(G->IconFixed);
                if (G->IconCd)
                    DestroyIcon(G->IconCd);
                if (G->IconRemovable)
                    DestroyIcon(G->IconRemovable);
                HeapFree(GetProcessHeap(), 0, G);
                SetWindowLongPtrW(hWnd, 0, 0);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT Ps;
            HDC hDC = BeginPaint(hWnd, &Ps);

            if (G)
                GraphPaint(hWnd, G, hDC, &Ps.rcPaint);
            EndPaint(hWnd, &Ps);
            return 0;
        }

        case WM_SIZE:
            if (G)
            {
                GraphLayout(hWnd, G);
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            return 0;

        case WM_VSCROLL:
        {
            SCROLLINFO Si;
            int Pos = G->ScrollY;

            Si.cbSize = sizeof(Si);
            Si.fMask = SIF_ALL;
            GetScrollInfo(hWnd, SB_VERT, &Si);
            switch (LOWORD(wParam))
            {
                case SB_LINEUP:     Pos -= 20; break;
                case SB_LINEDOWN:   Pos += 20; break;
                case SB_PAGEUP:     Pos -= Si.nPage; break;
                case SB_PAGEDOWN:   Pos += Si.nPage; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: Pos = Si.nTrackPos; break;
                case SB_TOP:        Pos = 0; break;
                case SB_BOTTOM:     Pos = G->ContentHeight; break;
            }
            Pos = max(0, min(Pos, max(G->ContentHeight - VisibleHeight(hWnd), 0)));
            if (Pos != G->ScrollY)
            {
                G->ScrollY = Pos;
                UpdateScrollBar(hWnd, G);
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            int Delta = GET_WHEEL_DELTA_WPARAM(wParam);

            G->ScrollY = max(0, min(G->ScrollY - Delta / 3, max(G->ContentHeight - VisibleHeight(hWnd), 0)));
            UpdateScrollBar(hWnd, G);
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        {
            int Hit = HitTest(G, (short)LOWORD(lParam), (short)HIWORD(lParam));

            SetFocus(hWnd);
            if (Hit >= 0)
            {
                SelectItem(hWnd, G, Hit, TRUE);
                if (Message == WM_LBUTTONDBLCLK)
                    Notify(hWnd, DMM_ACTIVATE, 0);
            }
            return 0;
        }

        case WM_CONTEXTMENU:
        {
            POINT pt;

            if ((short)LOWORD(lParam) == -1 && (short)HIWORD(lParam) == -1)
            {
                GraphGetSelectedPoint(hWnd, &pt);
                ClientToScreen(hWnd, &pt);
            }
            else
            {
                POINT Client;
                int Hit;

                pt.x = (short)LOWORD(lParam);
                pt.y = (short)HIWORD(lParam);
                Client = pt;
                ScreenToClient(hWnd, &Client);
                Hit = HitTest(G, Client.x, Client.y);
                if (Hit < 0)
                    return 0;
                SelectItem(hWnd, G, Hit, TRUE);
            }
            Notify(hWnd, DMM_CONTEXT, MAKELPARAM(pt.x, pt.y));
            return 0;
        }

        case WM_KEYDOWN:
        {
            int Current = FindItem(G, &G->Sel);

            switch (wParam)
            {
                case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
                {
                    int Next = Navigate(G, Current, (UINT)wParam);

                    if (Next >= 0 && Next != Current)
                        SelectItem(hWnd, G, Next, TRUE);
                    return 0;
                }
                case VK_PRIOR: case VK_NEXT:
                    SendMessageW(hWnd, WM_VSCROLL, wParam == VK_PRIOR ? SB_PAGEUP : SB_PAGEDOWN, 0);
                    return 0;
                case VK_RETURN:
                    if (Current >= 0)
                        Notify(hWnd, DMM_ACTIVATE, 0);
                    return 0;
            }
            break;
        }
    }
    return DefWindowProcW(hWnd, Message, wParam, lParam);
}

BOOL
GraphRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = GraphWndProc;
    wc.cbWndExtra = sizeof(LONG_PTR);
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = GRAPH_CLASS;
    return RegisterClassExW(&wc) != 0;
}

HWND
GraphCreate(HWND hParent, UINT Id)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, GRAPH_CLASS, NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP | WS_CLIPSIBLINGS,
                           0, 0, 0, 0, hParent, (HMENU)(UINT_PTR)Id, g_hInst, NULL);
}

VOID
GraphSetModel(HWND hWnd, PDM_MODEL Model)
{
    GRAPH *G = GetGraph(hWnd);

    G->Model = Model;
    GraphLayout(hWnd, G);
    if (FindItem(G, &G->Sel) < 0)
        G->Sel.Valid = FALSE;
    InvalidateRect(hWnd, NULL, FALSE);
}

VOID
GraphSetSelection(HWND hWnd, const DM_SELECTION *Selection)
{
    GRAPH *G = GetGraph(hWnd);
    int Index;

    G->Sel = *Selection;
    Index = FindItem(G, &G->Sel);
    if (Index < 0)
        G->Sel.Valid = FALSE;
    else
        EnsureVisible(hWnd, G, Index);
    InvalidateRect(hWnd, NULL, FALSE);
}

BOOL
GraphGetSelection(HWND hWnd, PDM_SELECTION Selection)
{
    GRAPH *G = GetGraph(hWnd);

    *Selection = G->Sel;
    return G->Sel.Valid;
}

VOID
GraphGetSelectedPoint(HWND hWnd, LPPOINT Point)
{
    GRAPH *G = GetGraph(hWnd);
    int Index = FindItem(G, &G->Sel);

    Point->x = Point->y = 10;
    if (Index >= 0)
    {
        Point->x = G->Items[Index].rc.left + 10;
        Point->y = G->Items[Index].rc.top + 10 - G->ScrollY;
    }
}
