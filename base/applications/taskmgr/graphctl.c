/*
 * PROJECT:     ReactOS Task Manager
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Graph Plotting controls.
 * COPYRIGHT:   Copyright 2002 Robert Dickenson <robd@reactos.org>
 *              Copyright 2021 Wu Haotian <rigoligo03@gmail.com>
 *              Copyright 2021 Valerij Zaporogeci <vlrzprgts@gmail.com>
 */

#include "precomp.h"

#include <math.h>

WNDPROC OldGraphCtrlWndProc;

/* The refresh thread draws new points while the UI thread paints and resizes */
static CRITICAL_SECTION GraphCtrlLock;
static BOOL bGraphCtrlLockInitialized = FALSE;

BOOL
GraphCtrl_Create(PTM_GRAPH_CONTROL inst, HWND hWnd, HWND hParentWnd, PTM_FORMAT fmt)
{
    HDC     hdc, hdcg;
    HBITMAP hbmOld;
    UINT    Size;
    INT     p;
    RECT    rc;

    if (!bGraphCtrlLockInitialized)
    {
        InitializeCriticalSection(&GraphCtrlLock);
        bGraphCtrlLockInitialized = TRUE;
    }

    inst->hParentWnd = hParentWnd;
    inst->hWnd = hWnd;

    /* The history covers the screen width; the bitmap only the window */
    Size = GetSystemMetrics(SM_CXSCREEN);
    Size /= PLOT_SHIFT;
    inst->PointBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size * NUM_PLOTS);
    if (!inst->PointBuffer)
    {
        goto fail;
    }

    inst->NumberOfPoints = Size;
    inst->CurrIndex = 0;

    /* Styling */
    inst->hPenGrid = CreatePen(PS_SOLID, 0, fmt->clrGrid);
    inst->hPen0 = CreatePen(PS_SOLID, 0, fmt->clrPlot0);
    inst->hPen1 = CreatePen(PS_SOLID, 0, fmt->clrPlot1);
    inst->hBrushBack = CreateSolidBrush(fmt->clrBack);

    if (!inst->hPenGrid ||
        !inst->hPen0 ||
        !inst->hPen1 ||
        !inst->hBrushBack)
    {
        goto fail;
    }

    if (fmt->GridCellWidth >= PLOT_SHIFT << 2)
        inst->GridCellWidth = fmt->GridCellWidth;
    else
        inst->GridCellWidth = PLOT_SHIFT << 2;
    if (fmt->GridCellHeight >= PLOT_SHIFT << 2)
        inst->GridCellHeight = fmt->GridCellHeight;
    else
        inst->GridCellHeight = PLOT_SHIFT << 2;

    inst->DrawSecondaryPlot = fmt->DrawSecondaryPlot;

    GetClientRect(hWnd, &rc);
    inst->BitmapWidth = max(rc.right, 1);
    inst->BitmapHeight = max(rc.bottom, 1);
    inst->ftPixelsPerPercent = (FLOAT)(inst->BitmapHeight) / 100.00f;

    hdc = GetDC(hParentWnd);
    if (!hdc)
        goto fail;

    inst->hdcGraph = hdcg = CreateCompatibleDC(hdc);
    inst->hbmGraph = CreateCompatibleBitmap(hdc, inst->BitmapWidth, inst->BitmapHeight);
    ReleaseDC(hParentWnd, hdc);
    if (!hdcg || !inst->hbmGraph)
        goto fail;

    hbmOld = (HBITMAP)SelectObject(hdcg, inst->hbmGraph);
    DeleteObject(hbmOld);

    SetBkColor(hdcg, fmt->clrBack);
    rc.right = inst->BitmapWidth;
    rc.bottom = inst->BitmapHeight;
    FillRect(hdcg, &rc, inst->hBrushBack);

    inst->CurrShift = 0;
    SelectObject(hdcg, inst->hPenGrid);
    for (p = inst->GridCellHeight - 1;
         p < inst->BitmapHeight;
         p += inst->GridCellHeight)
    {
        MoveToEx(hdcg, 0, p, NULL);
        LineTo(hdcg, inst->BitmapWidth, p);
    }
    for (p = inst->BitmapWidth - 1;
         p > 0;
         p -= inst->GridCellWidth)
    {
        MoveToEx(hdcg, p, 0, NULL);
        LineTo(hdcg, p, inst->BitmapHeight);
    }
    SelectObject(hdcg, inst->hPen0);

    SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)inst);
    return TRUE;

fail:
    GraphCtrl_Dispose(inst);
    return FALSE;
}

void
GraphCtrl_Dispose(PTM_GRAPH_CONTROL inst)
{
    if (inst->hWnd && (PTM_GRAPH_CONTROL)GetWindowLongPtrW(inst->hWnd, GWLP_USERDATA) == inst)
        SetWindowLongPtrW(inst->hWnd, GWLP_USERDATA, 0);

    if (inst->PointBuffer)
        HeapFree(GetProcessHeap(), 0, inst->PointBuffer);

    if (inst->hdcGraph)
        DeleteDC(inst->hdcGraph);

    if (inst->hPenGrid)
        DeleteObject(inst->hPenGrid);

    if (inst->hPen0)
        DeleteObject(inst->hPen0);

    if (inst->hPen1)
        DeleteObject(inst->hPen1);

    if (inst->hBrushBack)
        DeleteObject(inst->hBrushBack);

    if (inst->hbmGraph)
        DeleteObject(inst->hbmGraph);
}

void
GraphCtrl_AddPoint(PTM_GRAPH_CONTROL inst, BYTE val0, BYTE val1)
{
    HDC    hdcg;
    PBYTE  t;
    RECT   rcDirt;

    INT    Prev0, Prev1, RetainingWidth;
    INT    PrevY, CurrY, p, v;

    EnterCriticalSection(&GraphCtrlLock);
    hdcg = inst->hdcGraph;
    RetainingWidth = inst->BitmapWidth - PLOT_SHIFT;
    t = inst->PointBuffer;
    Prev0 = *(t + inst->CurrIndex);
    Prev1 = *(t + inst->CurrIndex + inst->NumberOfPoints);
    if (inst->CurrIndex < inst->NumberOfPoints - 1)
    {
        inst->CurrIndex++;
    }
    else
    {
        inst->CurrIndex = 0;
    }
    *(t + inst->CurrIndex) = val0;
    *(t + inst->CurrIndex + inst->NumberOfPoints) = val1;

    if (RetainingWidth < 1)
    {
        LeaveCriticalSection(&GraphCtrlLock);
        return;
    }

    /* Drawing points, first shifting the plot left */
    BitBlt(hdcg, 0, 0, RetainingWidth, inst->BitmapHeight, hdcg, PLOT_SHIFT, 0, SRCCOPY);

    rcDirt.left = RetainingWidth;
    rcDirt.top = 0;
    rcDirt.right = inst->BitmapWidth;
    rcDirt.bottom = inst->BitmapHeight;
    FillRect(hdcg, &rcDirt, inst->hBrushBack);

    SelectObject(hdcg, inst->hPenGrid);
    for (p = inst->GridCellHeight - 1;
         p < inst->BitmapHeight;
         p += inst->GridCellHeight)
    {
        MoveToEx(hdcg, RetainingWidth, p, NULL);
        LineTo(hdcg, inst->BitmapWidth, p);
    }
    v = inst->CurrShift + PLOT_SHIFT;
    if (v >= inst->GridCellWidth)
    {
        v -= inst->GridCellWidth;
        p = inst->BitmapWidth - v - 1;
        MoveToEx(hdcg, p, 0, NULL);
        LineTo(hdcg, p, inst->BitmapHeight);
    }
    inst->CurrShift = v;

    if (inst->DrawSecondaryPlot)
    {
        SelectObject(inst->hdcGraph, inst->hPen1);

        PrevY = inst->BitmapHeight - Prev1 * inst->ftPixelsPerPercent;
        MoveToEx(inst->hdcGraph, RetainingWidth - 1, PrevY, NULL);
        CurrY = inst->BitmapHeight - val1 * inst->ftPixelsPerPercent;
        LineTo(inst->hdcGraph, inst->BitmapWidth - 1, CurrY);
    }

    SelectObject(inst->hdcGraph, inst->hPen0);
    PrevY = inst->BitmapHeight - Prev0 * inst->ftPixelsPerPercent;
    MoveToEx(inst->hdcGraph, RetainingWidth - 1, PrevY, NULL);
    CurrY = inst->BitmapHeight - val0 * inst->ftPixelsPerPercent;
    LineTo(inst->hdcGraph, inst->BitmapWidth - 1, CurrY);
    LeaveCriticalSection(&GraphCtrlLock);
}

void
GraphCtrl_RedrawBitmap(PTM_GRAPH_CONTROL inst, INT h)
{
    HDC    hdcg;
    PBYTE  t;
    RECT   rc;
    INT    i, j, y, x, p;
    FLOAT  coef;

    EnterCriticalSection(&GraphCtrlLock);
    hdcg = inst->hdcGraph;
    rc.left = 0; rc.top = 0;
    rc.right = inst->BitmapWidth; rc.bottom = h;
    FillRect(hdcg, &rc, inst->hBrushBack);

    SelectObject(hdcg, inst->hPenGrid);

    for (p = inst->GridCellHeight - 1;
         p < inst->BitmapHeight;
         p += inst->GridCellHeight)
    {
        MoveToEx(hdcg, 0, p, NULL);
        LineTo(hdcg, inst->BitmapWidth, p);
    }

    for (p = inst->BitmapWidth - inst->CurrShift - 1;
         p > 0;
         p -= inst->GridCellWidth)
    {
        MoveToEx(hdcg, p, 0, NULL);
        LineTo(hdcg, p, inst->BitmapHeight);
    }

    coef = inst->ftPixelsPerPercent;

    if (inst->DrawSecondaryPlot)
    {
        SelectObject(hdcg, inst->hPen1);
        t = inst->PointBuffer + inst->NumberOfPoints;
        x = inst->BitmapWidth - 1;
        j = inst->CurrIndex;
        y = h - *(t + j) * coef;
        MoveToEx(hdcg, x, y, NULL);
        for (i = 0; i < inst->NumberOfPoints; i++)
        {
            j = (j ? j : inst->NumberOfPoints) - 1;
            y = h - *(t + j) * coef;
            x -= PLOT_SHIFT;
            LineTo(hdcg, x, y);
        }
    }

    SelectObject(hdcg, inst->hPen0);
    t = inst->PointBuffer;
    x = inst->BitmapWidth - 1;
    j = inst->CurrIndex;
    y = h - *(t + j) * coef;
    MoveToEx(hdcg, x, y, NULL);

    for (i = 0; i < inst->NumberOfPoints; i++)
    {
        j = (j ? j : inst->NumberOfPoints) - 1;
        y = h - *(t + j) * coef;
        x -= PLOT_SHIFT;
        LineTo(hdcg, x, y);
    }
    LeaveCriticalSection(&GraphCtrlLock);
}

void
GraphCtrl_Resize(PTM_GRAPH_CONTROL inst, INT nw, INT nh)
{
    HDC     hdc;
    HBITMAP hbmNew;

    nw = max(nw, 1);
    nh = max(nh, 1);

    EnterCriticalSection(&GraphCtrlLock);
    hdc = GetDC(inst->hParentWnd);
    hbmNew = CreateCompatibleBitmap(hdc, nw, nh);
    ReleaseDC(inst->hParentWnd, hdc);
    if (hbmNew)
    {
        SelectObject(inst->hdcGraph, hbmNew);
        DeleteObject(inst->hbmGraph);
        inst->hbmGraph = hbmNew;
        inst->BitmapWidth = nw;
        inst->BitmapHeight = nh;
        inst->ftPixelsPerPercent = (FLOAT)nh / 100.00f;
        GraphCtrl_RedrawBitmap(inst, nh);
    }
    LeaveCriticalSection(&GraphCtrlLock);
}

INT_PTR CALLBACK
GraphCtrl_WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PTM_GRAPH_CONTROL graph;

    switch (message)
    {
        case WM_ERASEBKGND:
            return TRUE;
        /*
         *  Filter out mouse  & keyboard messages
         */
        // case WM_APPCOMMAND:
        case WM_CAPTURECHANGED:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEACTIVATE:
        case WM_MOUSEHOVER:
        case WM_MOUSELEAVE:
        case WM_MOUSEMOVE:
        // case WM_MOUSEWHEEL:
        case WM_NCLBUTTONDBLCLK:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONUP:
        case WM_NCMBUTTONDBLCLK:
        case WM_NCMBUTTONDOWN:
        case WM_NCMBUTTONUP:
        // case WM_NCMOUSEHOVER:
        // case WM_NCMOUSELEAVE:
        case WM_NCMOUSEMOVE:
        case WM_NCRBUTTONDBLCLK:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONUP:
        // case WM_NCXBUTTONDBLCLK:
        // case WM_NCXBUTTONDOWN:
        // case WM_NCXBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        // case WM_XBUTTONDBLCLK:
        // case WM_XBUTTONDOWN:
        // case WM_XBUTTONUP:
        case WM_ACTIVATE:
        case WM_CHAR:
        case WM_DEADCHAR:
        case WM_GETHOTKEY:
        case WM_HOTKEY:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_KILLFOCUS:
        case WM_SETFOCUS:
        case WM_SETHOTKEY:
        case WM_SYSCHAR:
        case WM_SYSDEADCHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return 0;

        /* Clicks go to the page, which handles them (compact mode, CPU history switch) */
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_SIZE:
        {
            graph = (PTM_GRAPH_CONTROL)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
            if (!graph)
                return 0;

            if (LOWORD(lParam) != graph->BitmapWidth || HIWORD(lParam) != graph->BitmapHeight)
            {
                GraphCtrl_Resize(graph, LOWORD(lParam), HIWORD(lParam));
            }
            InvalidateRect(hWnd, NULL, FALSE);

            return 0;
        }

        case WM_PAINT:
        {
            RECT        rcClient;
            HDC         hdc;
            PAINTSTRUCT ps;

            graph = (PTM_GRAPH_CONTROL)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
            if (!graph)
                break;

            hdc = BeginPaint(hWnd, &ps);
            GetClientRect(hWnd, &rcClient);
            EnterCriticalSection(&GraphCtrlLock);
            BitBlt(hdc, 0, 0,
                   rcClient.right,
                   rcClient.bottom,
                   graph->hdcGraph,
                   graph->BitmapWidth - rcClient.right,
                   0,
                   SRCCOPY);
            LeaveCriticalSection(&GraphCtrlLock);
            EndPaint(hWnd, &ps);
            return 0;
        }
    }

    /*
     *  We pass on all non-handled messages
     */
    return CallWindowProcW(OldGraphCtrlWndProc, hWnd, message, wParam, lParam);
}
