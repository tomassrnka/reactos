/*
 * PROJECT:     ReactOS Task Manager
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Performance Page
 * COPYRIGHT:   Copyright 1999-2001 Brian Palmer <brianp@reactos.org>
 */

#include "precomp.h"
#include <shlwapi.h>
#include <windowsx.h>
#include <limits.h>

#include "perfpage.h"

TM_GRAPH_CONTROL PerformancePageCpuUsageHistoryGraph;
TM_GRAPH_CONTROL PerformancePageMemUsageHistoryGraph;

HWND hPerformancePage;                /* Performance Property Page */
static HWND hCpuUsageGraph;                  /* CPU Usage Graph */
static HWND hMemUsageGraph;                  /* MEM Usage Graph */
HWND hPerformancePageCpuUsageHistoryGraph;           /* CPU Usage History Graph */
HWND hPerformancePageMemUsageHistoryGraph;           /* Memory Usage History Graph */
static HWND hTotalsFrame;                    /* Totals Frame */
static HWND hCommitChargeFrame;              /* Commit Charge Frame */
static HWND hKernelMemoryFrame;              /* Kernel Memory Frame */
static HWND hPhysicalMemoryFrame;            /* Physical Memory Frame */
static HWND hCpuUsageFrame;
static HWND hMemUsageFrame;
static HWND hCpuUsageHistoryFrame;
static HWND hMemUsageHistoryFrame;
static HWND hCommitChargeTotalEdit;          /* Commit Charge Total Edit Control */
static HWND hCommitChargeLimitEdit;          /* Commit Charge Limit Edit Control */
static HWND hCommitChargePeakEdit;           /* Commit Charge Peak Edit Control */
static HWND hKernelMemoryTotalEdit;          /* Kernel Memory Total Edit Control */
static HWND hKernelMemoryPagedEdit;          /* Kernel Memory Paged Edit Control */
static HWND hKernelMemoryNonPagedEdit;       /* Kernel Memory NonPaged Edit Control */
static HWND hPhysicalMemoryTotalEdit;        /* Physical Memory Total Edit Control */
static HWND hPhysicalMemoryAvailableEdit;    /* Physical Memory Available Edit Control */
static HWND hPhysicalMemorySystemCacheEdit;  /* Physical Memory System Cache Edit Control */
static HWND hTotalsHandleCountEdit;          /* Total Handles Edit Control */
static HWND hTotalsProcessCountEdit;         /* Total Processes Edit Control */
static HWND hTotalsThreadCountEdit;          /* Total Threads Edit Control */

#ifdef RUN_PERF_PAGE
static HANDLE hPerformanceThread = NULL;
static DWORD  dwPerformanceThread;
#endif

static int nPerformancePageWidth;
static int nPerformancePageHeight;
static int lastX, lastY;
DWORD WINAPI PerformancePageRefreshThread(PVOID Parameter);

#define IDC_CPU_GRAPH_FIRST     0x4000  /* Control IDs of the per-processor history graphs */
#define CPU_GRAPH_GAP           4       /* Pixels between two per-processor graphs */
#define CPU_GRAPH_MIN_WIDTH     40      /* Below this width in one row, use more rows */
#define CLICK_TIMER_ID          1

/* One history graph per processor, laid out over the CPU usage history graph */
static PTM_GRAPH_CONTROL PerCpuGraphs = NULL;
static HWND *hPerCpuGraphs = NULL;
static ULONG nPerCpuGraphs = 0;

/* Compact mode: only the CPU usage and CPU usage history fill the window */
static BOOL  bCompactMode = FALSE;
static HMENU hCompactSavedMenu;
static RECT  rcCompactSavedPage;
static SIZE  szCompactSavedClient;
static RECT  rcCompactSaved[4];
static int   nCompactSavedWidth, nCompactSavedHeight, nCompactSavedLastX, nCompactSavedLastY;

static void PerformancePage_LayoutCpuGraphs(void);
static void PerformancePage_EnterCompactMode(void);
static void PerformancePage_ExitCompactMode(void);

static BOOL  bClickPending = FALSE;

static void GetChildRect(HWND hWnd, HWND hParent, LPRECT lprc)
{
    GetWindowRect(hWnd, lprc);
    MapWindowPoints(NULL, hParent, (LPPOINT)lprc, sizeof(RECT)/sizeof(POINT));
}

static HMENU PerformancePage_GetMainMenu(void)
{
    return bCompactMode ? hCompactSavedMenu : GetMenu(hMainWnd);
}

void AdjustFrameSize(HWND hCntrl, HWND hDlg, int nXDifference, int nYDifference, int pos)
{
    RECT  rc;
    int   cx, cy, sx, sy;

    /* The window rectangle: the graphs have a client edge */
    GetWindowRect(hCntrl, &rc);
    MapWindowPoints(NULL, hDlg, (LPPOINT)(PRECT)(&rc), sizeof(RECT)/sizeof(POINT));
    if (pos) {
        cx = rc.left;
        cy = rc.top;
        sx = rc.right - rc.left;
        switch (pos) {
        case 1:
            break;
        case 2:
            cy += nYDifference / 2;
            break;
        case 3:
            sx += nXDifference;
            break;
        case 4:
            cy += nYDifference / 2;
            sx += nXDifference;
            break;
        }
        sy = rc.bottom - rc.top + nYDifference / 2;
        SetWindowPos(hCntrl, NULL, cx, cy, sx, sy, SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOZORDER);
    } else {
        cx = rc.left + nXDifference;
        cy = rc.top + nYDifference;
        SetWindowPos(hCntrl, NULL, cx, cy, 0, 0, SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOSIZE|SWP_NOZORDER);
    }
    InvalidateRect(hCntrl, NULL, TRUE);
}

static inline
void AdjustControlPosition(HWND hCntrl, HWND hDlg, int nXDifference, int nYDifference)
{
    AdjustFrameSize(hCntrl, hDlg, nXDifference, nYDifference, 0);
}

static inline
void AdjustCntrlPos(int ctrl_id, HWND hDlg, int nXDifference, int nYDifference)
{
    AdjustFrameSize(GetDlgItem(hDlg, ctrl_id), hDlg, nXDifference, nYDifference, 0);
}

static void PerformancePage_CreateCpuGraphs(HWND hDlg, PTM_FORMAT fmt)
{
    ULONG i, nCreated = 0, nProcessors;
    RECT rc;

    nProcessors = PerfDataGetProcessorCount();
    if (nProcessors < 2)
        return;

    PerCpuGraphs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nProcessors * sizeof(*PerCpuGraphs));
    hPerCpuGraphs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nProcessors * sizeof(*hPerCpuGraphs));
    if (!PerCpuGraphs || !hPerCpuGraphs)
        goto fail;

    GetChildRect(hPerformancePageCpuUsageHistoryGraph, hDlg, &rc);
    for (i = 0; i < nProcessors; i++)
    {
        hPerCpuGraphs[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"Button", NULL, WS_CHILD | BS_PUSHBUTTON,
                                           rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                           hDlg, (HMENU)(ULONG_PTR)(IDC_CPU_GRAPH_FIRST + i), hInst, NULL);
        if (!hPerCpuGraphs[i])
            goto fail;
        if (!GraphCtrl_Create(&PerCpuGraphs[i], hPerCpuGraphs[i], hDlg, fmt))
        {
            DestroyWindow(hPerCpuGraphs[i]);
            goto fail;
        }
        nCreated++;
        SetWindowLongPtrW(hPerCpuGraphs[i], GWLP_WNDPROC, (LONG_PTR)GraphCtrl_WndProc);
        SetWindowPos(hPerCpuGraphs[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    nPerCpuGraphs = nProcessors;
    return;

fail:
    /* Without them the page shows the single graph, as on a uniprocessor system */
    for (i = 0; i < nCreated; i++)
    {
        GraphCtrl_Dispose(&PerCpuGraphs[i]);
        DestroyWindow(hPerCpuGraphs[i]);
    }
    if (PerCpuGraphs)
        HeapFree(GetProcessHeap(), 0, PerCpuGraphs);
    if (hPerCpuGraphs)
        HeapFree(GetProcessHeap(), 0, hPerCpuGraphs);
    PerCpuGraphs = NULL;
    hPerCpuGraphs = NULL;
}

static void PerformancePage_LayoutCpuGraphs(void)
{
    RECT rc;
    ULONG i, nRows, nCols, r;
    INT nWidth, nHeight, cx, cy, nBest;
    BOOL bOneGraphPerCPU;

    bOneGraphPerCPU = (nPerCpuGraphs > 0) && TaskManagerSettings.CPUHistory_OneGraphPerCPU;
    if (nPerCpuGraphs == 0)
    {
        ShowWindow(hPerformancePageCpuUsageHistoryGraph, SW_SHOW);
        return;
    }

    /* The per-processor graphs share the area of the single graph */
    GetChildRect(hPerformancePageCpuUsageHistoryGraph, hPerformancePage, &rc);
    nWidth = rc.right - rc.left;
    nHeight = rc.bottom - rc.top;

    /* One row, as on Windows, unless that makes the graphs too narrow to read;
     * then the number of rows that gives the largest graphs */
    nRows = 1;
    if ((nWidth - (INT)(nPerCpuGraphs - 1) * CPU_GRAPH_GAP) / (INT)nPerCpuGraphs < CPU_GRAPH_MIN_WIDTH)
    {
        nBest = INT_MIN;
        for (r = 1; r <= nPerCpuGraphs; r++)
        {
            nCols = (nPerCpuGraphs + r - 1) / r;
            cx = (nWidth - (INT)(nCols - 1) * CPU_GRAPH_GAP) / (INT)nCols;
            cy = (nHeight - (INT)(r - 1) * CPU_GRAPH_GAP) / (INT)r;
            if (min(cx, cy) > nBest)
            {
                nBest = min(cx, cy);
                nRows = r;
            }
        }
    }
    nCols = (nPerCpuGraphs + nRows - 1) / nRows;
    cx = max((nWidth - (INT)(nCols - 1) * CPU_GRAPH_GAP) / (INT)nCols, 1);
    cy = max((nHeight - (INT)(nRows - 1) * CPU_GRAPH_GAP) / (INT)nRows, 1);

    for (i = 0; i < nPerCpuGraphs; i++)
    {
        SetWindowPos(hPerCpuGraphs[i], NULL,
                     rc.left + (INT)(i % nCols) * (cx + CPU_GRAPH_GAP),
                     rc.top + (INT)(i / nCols) * (cy + CPU_GRAPH_GAP),
                     cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hPerCpuGraphs[i], bOneGraphPerCPU ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(hPerformancePageCpuUsageHistoryGraph, bOneGraphPerCPU ? SW_HIDE : SW_SHOW);
}

/* A single click on the CPU usage history switches between one graph and one per CPU.
 * It takes effect after the double click time, so that a double click does not switch.
 * Every new button press first commits a click still pending, so a double click only
 * cancels its own first click. */
static void PerformancePage_CommitPendingClick(void)
{
    if (bClickPending)
    {
        KillTimer(hPerformancePage, CLICK_TIMER_ID);
        bClickPending = FALSE;
        PerformancePage_SetCpuHistoryMode(!TaskManagerSettings.CPUHistory_OneGraphPerCPU);
    }
}

static void PerformancePage_QueueClick(void)
{
    bClickPending = SetTimer(hPerformancePage, CLICK_TIMER_ID, GetDoubleClickTime(), NULL) != 0;
}

static void PerformancePage_CancelClick(void)
{
    KillTimer(hPerformancePage, CLICK_TIMER_ID);
    bClickPending = FALSE;
}

static BOOL PerformancePage_IsPointInCpuHistory(POINT pt)
{
    RECT rc;

    if (nPerCpuGraphs == 0)
        return FALSE;
    GetChildRect(hPerformancePageCpuUsageHistoryGraph, hPerformancePage, &rc);
    return PtInRect(&rc, pt);
}

static void PerformancePage_LayoutCompact(void)
{
    RECT rcPage, rc;
    INT nMargin, nTop, nBottom, i;
    HWND hWnds[4] = { hCpuUsageFrame, hCpuUsageGraph, hCpuUsageHistoryFrame, hPerformancePageCpuUsageHistoryGraph };
    RECT rcNew[4];

    /* The frames keep their left edge and the margins they had; they take the whole height,
     * and the history frame the remaining width. The graphs keep their insets in the frames. */
    GetClientRect(hPerformancePage, &rcPage);
    nMargin = rcCompactSaved[0].left;
    nTop = rcCompactSaved[0].top;
    nBottom = max(rcPage.bottom - nTop, nTop + 1);

    SetRect(&rcNew[0], rcCompactSaved[0].left, nTop, rcCompactSaved[0].right, nBottom);
    SetRect(&rcNew[2], rcCompactSaved[2].left, nTop, max(rcPage.right - nMargin, rcCompactSaved[2].left + 1), nBottom);
    for (i = 1; i < 4; i += 2)
    {
        rc = rcNew[i - 1];
        rc.left += rcCompactSaved[i].left - rcCompactSaved[i - 1].left;
        rc.top += rcCompactSaved[i].top - rcCompactSaved[i - 1].top;
        rc.right -= rcCompactSaved[i - 1].right - rcCompactSaved[i].right;
        rc.bottom -= rcCompactSaved[i - 1].bottom - rcCompactSaved[i].bottom;
        rc.right = max(rc.right, rc.left + 1);
        rc.bottom = max(rc.bottom, rc.top + 1);
        rcNew[i] = rc;
    }
    for (i = 0; i < 4; i++)
    {
        SetWindowPos(hWnds[i], NULL, rcNew[i].left, rcNew[i].top,
                     rcNew[i].right - rcNew[i].left, rcNew[i].bottom - rcNew[i].top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    PerformancePage_LayoutCpuGraphs();
    InvalidateRect(hPerformancePage, NULL, TRUE);
}

static BOOL CALLBACK PerformancePage_ShowChild(HWND hWnd, LPARAM lParam)
{
    INT nId;

    if (GetParent(hWnd) != hPerformancePage)
        return TRUE;
    nId = GetDlgCtrlID(hWnd);
    if (hWnd == hCpuUsageFrame || hWnd == hCpuUsageGraph ||
        hWnd == hCpuUsageHistoryFrame || hWnd == hPerformancePageCpuUsageHistoryGraph ||
        (nId >= IDC_CPU_GRAPH_FIRST && nId < IDC_CPU_GRAPH_FIRST + (INT)nPerCpuGraphs))
    {
        return TRUE;
    }
    ShowWindow(hWnd, (INT)lParam);
    return TRUE;
}

static void PerformancePage_EnterCompactMode(void)
{
    RECT rc;
    INT i;
    HWND hWnds[4] = { hCpuUsageFrame, hCpuUsageGraph, hCpuUsageHistoryFrame, hPerformancePageCpuUsageHistoryGraph };

    if (bCompactMode)
        return;

    /* Remember the normal layout; the page's own resize code continues from it on the way back */
    GetChildRect(hPerformancePage, hMainWnd, &rcCompactSavedPage);
    GetClientRect(hMainWnd, &rc);
    szCompactSavedClient.cx = rc.right;
    szCompactSavedClient.cy = rc.bottom;
    for (i = 0; i < 4; i++)
        GetChildRect(hWnds[i], hPerformancePage, &rcCompactSaved[i]);
    nCompactSavedWidth = nPerformancePageWidth;
    nCompactSavedHeight = nPerformancePageHeight;
    nCompactSavedLastX = lastX;
    nCompactSavedLastY = lastY;

    bCompactMode = TRUE;
    TaskManagerSettings.CompactMode = TRUE;

    EnumChildWindows(hPerformancePage, PerformancePage_ShowChild, SW_HIDE);
    ShowWindow(hTabWnd, SW_HIDE);
    ShowWindow(hStatusWnd, SW_HIDE);
    hCompactSavedMenu = GetMenu(hMainWnd);
    SetMenu(hMainWnd, NULL);
    SetWindowLongPtrW(hMainWnd, GWL_STYLE, GetWindowLongPtrW(hMainWnd, GWL_STYLE) & ~WS_CAPTION);
    SetWindowPos(hMainWnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    GetClientRect(hMainWnd, &rc);
    SetWindowPos(hPerformancePage, NULL, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    PerformancePage_LayoutCompact();
    InvalidateRect(hMainWnd, NULL, TRUE);
}

static void PerformancePage_ExitCompactMode(void)
{
    RECT rc;
    INT i;
    HWND hWnds[4] = { hCpuUsageFrame, hCpuUsageGraph, hCpuUsageHistoryFrame, hPerformancePageCpuUsageHistoryGraph };
    HMENU hOptionsMenu;

    if (!bCompactMode)
        return;

    SetWindowLongPtrW(hMainWnd, GWL_STYLE, GetWindowLongPtrW(hMainWnd, GWL_STYLE) | WS_CAPTION);
    SetMenu(hMainWnd, hCompactSavedMenu);
    SetWindowPos(hMainWnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    /* The tray icon menu may have changed this meanwhile */
    hOptionsMenu = GetSubMenu(hCompactSavedMenu, OPTIONS_MENU_INDEX);
    CheckMenuItem(hOptionsMenu, ID_OPTIONS_ALWAYSONTOP,
                  MF_BYCOMMAND | (TaskManagerSettings.AlwaysOnTop ? MF_CHECKED : MF_UNCHECKED));

    for (i = 0; i < 4; i++)
    {
        SetWindowPos(hWnds[i], NULL, rcCompactSaved[i].left, rcCompactSaved[i].top,
                     rcCompactSaved[i].right - rcCompactSaved[i].left,
                     rcCompactSaved[i].bottom - rcCompactSaved[i].top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    nPerformancePageWidth = nCompactSavedWidth;
    nPerformancePageHeight = nCompactSavedHeight;
    lastX = nCompactSavedLastX;
    lastY = nCompactSavedLastY;

    bCompactMode = FALSE;
    TaskManagerSettings.CompactMode = FALSE;

    EnumChildWindows(hPerformancePage, PerformancePage_ShowChild, SW_SHOW);
    ShowWindow(hTabWnd, SW_SHOW);
    ShowWindow(hStatusWnd, SW_SHOW);

    /* Back to the normal page size for the current window size */
    GetClientRect(hMainWnd, &rc);
    SetWindowPos(hPerformancePage, NULL, rcCompactSavedPage.left, rcCompactSavedPage.top,
                 rcCompactSavedPage.right - rcCompactSavedPage.left + rc.right - szCompactSavedClient.cx,
                 rcCompactSavedPage.bottom - rcCompactSavedPage.top + rc.bottom - szCompactSavedClient.cy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    PerformancePage_LayoutCpuGraphs();
    InvalidateRect(hMainWnd, NULL, TRUE);
}

void PerformancePage_RestoreCompactMode(void)
{
    if (TaskManagerSettings.CompactMode)
    {
        TaskManagerSettings.CompactMode = FALSE;
        if (TaskManagerSettings.ActiveTabPage == 2)
            PerformancePage_EnterCompactMode();
    }
}

/* Main window messages in compact mode: the window has no caption, so a drag anywhere moves it
 * and a double click restores the normal view, as on Windows */
BOOL PerformancePage_OnMainWindowMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT *plResult)
{
    RECT rcBefore, rcAfter;
    POINT pt;

    if (!bCompactMode)
        return FALSE;

    switch (message)
    {
        case WM_NCHITTEST:
            *plResult = DefWindowProcW(hWnd, message, wParam, lParam);
            if (*plResult == HTCLIENT)
                *plResult = HTCAPTION;
            return TRUE;

        case WM_NCLBUTTONDBLCLK:
            if (wParam != HTCAPTION)
                return FALSE;
            PerformancePage_CancelClick();
            PerformancePage_ExitCompactMode();
            *plResult = 0;
            return TRUE;

        case WM_NCLBUTTONDOWN:
            PerformancePage_CommitPendingClick();
            if (wParam != HTCAPTION)
                return FALSE;
            /* This returns when the button is released; a click that did not move the window
             * is a click on the page */
            GetWindowRect(hWnd, &rcBefore);
            DefWindowProcW(hWnd, message, wParam, lParam);
            GetWindowRect(hWnd, &rcAfter);
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hPerformancePage, &pt);
            if (EqualRect(&rcBefore, &rcAfter) && PerformancePage_IsPointInCpuHistory(pt))
                PerformancePage_QueueClick();
            *plResult = 0;
            return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK
PerformancePageWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    RECT rc;

    switch (message)
    {
        case WM_DESTROY:
        {
            ULONG i;

            PerformancePage_CancelClick();
#ifdef RUN_PERF_PAGE
            EndLocalThread(&hPerformanceThread, dwPerformanceThread);
#endif
            GraphCtrl_Dispose(&PerformancePageCpuUsageHistoryGraph);
            GraphCtrl_Dispose(&PerformancePageMemUsageHistoryGraph);
            for (i = 0; i < nPerCpuGraphs; i++)
                GraphCtrl_Dispose(&PerCpuGraphs[i]);
            nPerCpuGraphs = 0;
            if (PerCpuGraphs)
                HeapFree(GetProcessHeap(), 0, PerCpuGraphs);
            if (hPerCpuGraphs)
                HeapFree(GetProcessHeap(), 0, hPerCpuGraphs);
            PerCpuGraphs = NULL;
            hPerCpuGraphs = NULL;
            break;
        }

        case WM_INITDIALOG:
        {
            BOOL bGraph;
            TM_FORMAT fmt;

            /* CreateDialog sets it only after this message; the layout code needs it now */
            hPerformancePage = hDlg;

            /* Save the width and height */
            GetClientRect(hDlg, &rc);
            nPerformancePageWidth = rc.right;
            nPerformancePageHeight = rc.bottom;

            /* Update window position */
            SetWindowPos(hDlg, NULL, 15, 30, 0, 0, SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOSIZE|SWP_NOZORDER);

            /*
             * Get handles to all the controls
             */
            hTotalsFrame = GetDlgItem(hDlg, IDC_TOTALS_FRAME);
            hCommitChargeFrame = GetDlgItem(hDlg, IDC_COMMIT_CHARGE_FRAME);
            hKernelMemoryFrame = GetDlgItem(hDlg, IDC_KERNEL_MEMORY_FRAME);
            hPhysicalMemoryFrame = GetDlgItem(hDlg, IDC_PHYSICAL_MEMORY_FRAME);

            hCpuUsageFrame = GetDlgItem(hDlg, IDC_CPU_USAGE_FRAME);
            hMemUsageFrame = GetDlgItem(hDlg, IDC_MEM_USAGE_FRAME);
            hCpuUsageHistoryFrame = GetDlgItem(hDlg, IDC_CPU_USAGE_HISTORY_FRAME);
            hMemUsageHistoryFrame = GetDlgItem(hDlg, IDC_MEMORY_USAGE_HISTORY_FRAME);

            hCommitChargeTotalEdit = GetDlgItem(hDlg, IDC_COMMIT_CHARGE_TOTAL);
            hCommitChargeLimitEdit = GetDlgItem(hDlg, IDC_COMMIT_CHARGE_LIMIT);
            hCommitChargePeakEdit = GetDlgItem(hDlg, IDC_COMMIT_CHARGE_PEAK);
            hKernelMemoryTotalEdit = GetDlgItem(hDlg, IDC_KERNEL_MEMORY_TOTAL);
            hKernelMemoryPagedEdit = GetDlgItem(hDlg, IDC_KERNEL_MEMORY_PAGED);
            hKernelMemoryNonPagedEdit = GetDlgItem(hDlg, IDC_KERNEL_MEMORY_NONPAGED);
            hPhysicalMemoryTotalEdit = GetDlgItem(hDlg, IDC_PHYSICAL_MEMORY_TOTAL);
            hPhysicalMemoryAvailableEdit = GetDlgItem(hDlg, IDC_PHYSICAL_MEMORY_AVAILABLE);
            hPhysicalMemorySystemCacheEdit = GetDlgItem(hDlg, IDC_PHYSICAL_MEMORY_SYSTEM_CACHE);
            hTotalsHandleCountEdit = GetDlgItem(hDlg, IDC_TOTALS_HANDLE_COUNT);
            hTotalsProcessCountEdit = GetDlgItem(hDlg, IDC_TOTALS_PROCESS_COUNT);
            hTotalsThreadCountEdit = GetDlgItem(hDlg, IDC_TOTALS_THREAD_COUNT);

            hCpuUsageGraph = GetDlgItem(hDlg, IDC_CPU_USAGE_GRAPH);
            hMemUsageGraph = GetDlgItem(hDlg, IDC_MEM_USAGE_GRAPH);
            hPerformancePageMemUsageHistoryGraph = GetDlgItem(hDlg, IDC_MEM_USAGE_HISTORY_GRAPH);
            hPerformancePageCpuUsageHistoryGraph = GetDlgItem(hDlg, IDC_CPU_USAGE_HISTORY_GRAPH);

            /* Create the controls */
            fmt.clrBack = RGB(0, 0, 0);
            fmt.clrGrid = RGB(0, 128, 64);
            fmt.clrPlot0 = RGB(0, 255, 0);
            fmt.clrPlot1 = RGB(255, 0, 0);
            fmt.GridCellWidth = fmt.GridCellHeight = 12;
            fmt.DrawSecondaryPlot = TaskManagerSettings.ShowKernelTimes;
            bGraph = GraphCtrl_Create(&PerformancePageCpuUsageHistoryGraph, hPerformancePageCpuUsageHistoryGraph, hDlg, &fmt);
            if (!bGraph)
            {
                EndDialog(hDlg, 0);
                return FALSE;
            }

            fmt.clrPlot0 = RGB(255, 255, 0);
            fmt.clrPlot1 = RGB(100, 255, 255);
            fmt.DrawSecondaryPlot = TRUE;
            bGraph = GraphCtrl_Create(&PerformancePageMemUsageHistoryGraph, hPerformancePageMemUsageHistoryGraph, hDlg, &fmt);
            if (!bGraph)
            {
                EndDialog(hDlg, 0);
                return FALSE;
            }

            /*
             * Subclass graph buttons
             */
            OldGraphWndProc = (WNDPROC)SetWindowLongPtrW(hCpuUsageGraph, GWLP_WNDPROC, (LONG_PTR)Graph_WndProc);
            SetWindowLongPtrW(hMemUsageGraph, GWLP_WNDPROC, (LONG_PTR)Graph_WndProc);
            OldGraphCtrlWndProc = (WNDPROC)SetWindowLongPtrW(hPerformancePageMemUsageHistoryGraph, GWLP_WNDPROC, (LONG_PTR)GraphCtrl_WndProc);
            SetWindowLongPtrW(hPerformancePageCpuUsageHistoryGraph, GWLP_WNDPROC, (LONG_PTR)GraphCtrl_WndProc);

            /* One graph per processor on multiprocessor systems */
            fmt.clrPlot0 = RGB(0, 255, 0);
            fmt.clrPlot1 = RGB(255, 0, 0);
            fmt.DrawSecondaryPlot = TaskManagerSettings.ShowKernelTimes;
            PerformancePage_CreateCpuGraphs(hDlg, &fmt);
            PerformancePage_LayoutCpuGraphs();

            /* Start our refresh thread */
#ifdef RUN_PERF_PAGE
            hPerformanceThread = CreateThread(NULL, 0, PerformancePageRefreshThread, NULL, 0, &dwPerformanceThread);
#endif
            return TRUE;
        }

        case WM_NCHITTEST:
            /* In compact mode the main window takes the clicks: drag to move, double click to restore */
            if (bCompactMode)
            {
                SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, HTTRANSPARENT);
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            PerformancePage_CommitPendingClick();
            if (PerformancePage_IsPointInCpuHistory(pt))
                PerformancePage_QueueClick();
            break;
        }

        case WM_LBUTTONDBLCLK:
            PerformancePage_CancelClick();
            PerformancePage_EnterCompactMode();
            break;

        case WM_TIMER:
            if (wParam == CLICK_TIMER_ID)
            {
                KillTimer(hDlg, CLICK_TIMER_ID);
                PerformancePage_CommitPendingClick();
            }
            break;

        case WM_COMMAND:
            break;

        case WM_SIZE:
        {
            int  cx, cy;
            int  nXDifference;
            int  nYDifference;

            if (wParam == SIZE_MINIMIZED)
                return 0;

            if (bCompactMode)
            {
                PerformancePage_LayoutCompact();
                return 0;
            }

            cx = LOWORD(lParam);
            cy = HIWORD(lParam);
            nXDifference = cx - nPerformancePageWidth;
            nYDifference = cy - nPerformancePageHeight;
            nPerformancePageWidth = cx;
            nPerformancePageHeight = cy;

            /* Reposition the performance page's controls */
            AdjustFrameSize(hTotalsFrame, hDlg, 0, nYDifference, 0);
            AdjustFrameSize(hCommitChargeFrame, hDlg, 0, nYDifference, 0);
            AdjustFrameSize(hKernelMemoryFrame, hDlg, 0, nYDifference, 0);
            AdjustFrameSize(hPhysicalMemoryFrame, hDlg, 0, nYDifference, 0);
            AdjustCntrlPos(IDS_COMMIT_CHARGE_TOTAL, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_COMMIT_CHARGE_LIMIT, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_COMMIT_CHARGE_PEAK, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_KERNEL_MEMORY_TOTAL, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_KERNEL_MEMORY_PAGED, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_KERNEL_MEMORY_NONPAGED, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_PHYSICAL_MEMORY_TOTAL, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_PHYSICAL_MEMORY_AVAILABLE, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_PHYSICAL_MEMORY_SYSTEM_CACHE, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_TOTALS_HANDLE_COUNT, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_TOTALS_PROCESS_COUNT, hDlg, 0, nYDifference);
            AdjustCntrlPos(IDS_TOTALS_THREAD_COUNT, hDlg, 0, nYDifference);

            AdjustControlPosition(hCommitChargeTotalEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hCommitChargeLimitEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hCommitChargePeakEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hKernelMemoryTotalEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hKernelMemoryPagedEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hKernelMemoryNonPagedEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hPhysicalMemoryTotalEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hPhysicalMemoryAvailableEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hPhysicalMemorySystemCacheEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hTotalsHandleCountEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hTotalsProcessCountEdit, hDlg, 0, nYDifference);
            AdjustControlPosition(hTotalsThreadCountEdit, hDlg, 0, nYDifference);

            nXDifference += lastX;
            nYDifference += lastY;
            lastX = lastY = 0;
            if (nXDifference % 2)
            {
                if (nXDifference > 0)
                {
                    nXDifference--;
                    lastX++;
                }
                else
                {
                    nXDifference++;
                    lastX--;
                }
            }
            if (nYDifference % 2)
            {
                if (nYDifference > 0)
                {
                    nYDifference--;
                    lastY++;
                }
                else
                {
                    nYDifference++;
                    lastY--;
                }
            }
            AdjustFrameSize(hCpuUsageFrame, hDlg, nXDifference, nYDifference, 1);
            AdjustFrameSize(hMemUsageFrame, hDlg, nXDifference, nYDifference, 2);
            AdjustFrameSize(hCpuUsageHistoryFrame, hDlg, nXDifference, nYDifference, 3);
            AdjustFrameSize(hMemUsageHistoryFrame, hDlg, nXDifference, nYDifference, 4);
            AdjustFrameSize(hCpuUsageGraph, hDlg, nXDifference, nYDifference, 1);
            AdjustFrameSize(hMemUsageGraph, hDlg, nXDifference, nYDifference, 2);
            AdjustFrameSize(hPerformancePageCpuUsageHistoryGraph, hDlg, nXDifference, nYDifference, 3);
            AdjustFrameSize(hPerformancePageMemUsageHistoryGraph, hDlg, nXDifference, nYDifference, 4);
            PerformancePage_LayoutCpuGraphs();
            break;
        }
    }
    return 0;
}

void RefreshPerformancePage(void)
{
#ifdef RUN_PERF_PAGE
    /* Signal the event so that our refresh thread
     * will wake up and refresh the performance page */
    PostThreadMessage(dwPerformanceThread, WM_TIMER, 0, 0);
#endif
}

DWORD WINAPI PerformancePageRefreshThread(PVOID Parameter)
{
    ULONGLONG CommitChargeTotal;
    ULONGLONG CommitChargeLimit;
    ULONGLONG CommitChargePeak;

    ULONG CpuUsage;
    ULONG CpuKernelUsage;

    ULONGLONG KernelMemoryTotal;
    ULONGLONG KernelMemoryPaged;
    ULONGLONG KernelMemoryNonPaged;

    ULONGLONG PhysicalMemoryTotal;
    ULONGLONG PhysicalMemoryAvailable;
    ULONGLONG PhysicalMemorySystemCache;

    ULONG TotalHandles;
    ULONG TotalThreads;
    ULONG TotalProcesses;

    MSG msg;

    WCHAR Text[260];
    WCHAR szMemUsage[256], szCpuUsage[256], szProcesses[256];
    PULONG pCpuUsages = NULL;

    LoadStringW(hInst, IDS_STATUS_CPUUSAGE, szCpuUsage, _countof(szCpuUsage));
    LoadStringW(hInst, IDS_STATUS_MEMUSAGE, szMemUsage, _countof(szMemUsage));
    LoadStringW(hInst, IDS_STATUS_PROCESSES, szProcesses, _countof(szProcesses));

    /* Usages, then kernel usages, of all processors */
    if (nPerCpuGraphs > 0)
        pCpuUsages = HeapAlloc(GetProcessHeap(), 0, 2 * nPerCpuGraphs * sizeof(*pCpuUsages));

    while (1)
    {
        extern BOOL bTrackMenu; // From taskmgr.c

        int nBarsUsed1;
        int nBarsUsed2;
        ULONG i, nCpus;

        WCHAR szChargeTotalFormat[256];
        WCHAR szChargeLimitFormat[256];

        /* Wait for an the event or application close */
        if (GetMessage(&msg, NULL, 0, 0) <= 0)
        {
            if (pCpuUsages)
                HeapFree(GetProcessHeap(), 0, pCpuUsages);
            return 0;
        }

        if (msg.message == WM_TIMER)
        {
            /*
             * Update the commit charge info
             */
            CommitChargeTotal = PerfDataGetCommitChargeTotalK();
            CommitChargeLimit = PerfDataGetCommitChargeLimitK();
            CommitChargePeak  = PerfDataGetCommitChargePeakK();
            _ultow(CommitChargeTotal, Text, 10);
            SetWindowTextW(hCommitChargeTotalEdit, Text);
            _ultow(CommitChargeLimit, Text, 10);
            SetWindowTextW(hCommitChargeLimitEdit, Text);
            _ultow(CommitChargePeak, Text, 10);
            SetWindowTextW(hCommitChargePeakEdit, Text);

            StrFormatByteSizeW(CommitChargeTotal * 1024,
                               szChargeTotalFormat,
                               _countof(szChargeTotalFormat));

            StrFormatByteSizeW(CommitChargeLimit * 1024,
                               szChargeLimitFormat,
                               _countof(szChargeLimitFormat));

            if (!bTrackMenu)
            {
                wsprintfW(Text, szMemUsage, szChargeTotalFormat, szChargeLimitFormat,
                    (CommitChargeLimit ? ((CommitChargeTotal * 100) / CommitChargeLimit) : 0));
                SendMessageW(hStatusWnd, SB_SETTEXT, 2, (LPARAM)Text);
            }

            /*
             * Update the kernel memory info
             */
            KernelMemoryTotal = PerfDataGetKernelMemoryTotalK();
            KernelMemoryPaged = PerfDataGetKernelMemoryPagedK();
            KernelMemoryNonPaged = PerfDataGetKernelMemoryNonPagedK();
            _ultow(KernelMemoryTotal, Text, 10);
            SetWindowTextW(hKernelMemoryTotalEdit, Text);
            _ultow(KernelMemoryPaged, Text, 10);
            SetWindowTextW(hKernelMemoryPagedEdit, Text);
            _ultow(KernelMemoryNonPaged, Text, 10);
            SetWindowTextW(hKernelMemoryNonPagedEdit, Text);

            /*
             * Update the physical memory info
             */
            PhysicalMemoryTotal = PerfDataGetPhysicalMemoryTotalK();
            PhysicalMemoryAvailable = PerfDataGetPhysicalMemoryAvailableK();
            PhysicalMemorySystemCache = PerfDataGetPhysicalMemorySystemCacheK();
            _ultow(PhysicalMemoryTotal, Text, 10);
            SetWindowTextW(hPhysicalMemoryTotalEdit, Text);
            _ultow(PhysicalMemoryAvailable, Text, 10);
            SetWindowTextW(hPhysicalMemoryAvailableEdit, Text);
            _ultow(PhysicalMemorySystemCache, Text, 10);
            SetWindowTextW(hPhysicalMemorySystemCacheEdit, Text);

            /*
             * Update the totals info
             */
            TotalHandles = PerfDataGetSystemHandleCount();
            TotalThreads = PerfDataGetTotalThreadCount();
            TotalProcesses = PerfDataGetProcessCount();
            _ultow(TotalHandles, Text, 10);
            SetWindowTextW(hTotalsHandleCountEdit, Text);
            _ultow(TotalThreads, Text, 10);
            SetWindowTextW(hTotalsThreadCountEdit, Text);
            _ultow(TotalProcesses, Text, 10);
            SetWindowTextW(hTotalsProcessCountEdit, Text);
            if (!bTrackMenu)
            {
                wsprintfW(Text, szProcesses, TotalProcesses);
                SendMessageW(hStatusWnd, SB_SETTEXT, 0, (LPARAM)Text);
            }

            /*
             * Redraw the graphs
             */
            InvalidateRect(hCpuUsageGraph, NULL, FALSE);
            InvalidateRect(hMemUsageGraph, NULL, FALSE);

            /*
             * Get the CPU usage
             */
            CpuUsage = PerfDataGetProcessorUsage();
            CpuKernelUsage = PerfDataGetProcessorSystemUsage();

            if (!bTrackMenu)
            {
                wsprintfW(Text, szCpuUsage, CpuUsage);
                SendMessageW(hStatusWnd, SB_SETTEXT, 1, (LPARAM)Text);
            }

            /*
             * Get the memory usage
             */
            CommitChargeTotal = PerfDataGetCommitChargeTotalK();
            CommitChargeLimit = PerfDataGetCommitChargeLimitK();
            nBarsUsed1 = CommitChargeLimit ? ((CommitChargeTotal * 100) / CommitChargeLimit) : 0;

            PhysicalMemoryTotal = PerfDataGetPhysicalMemoryTotalK();
            PhysicalMemoryAvailable = PerfDataGetPhysicalMemoryAvailableK();
            nBarsUsed2 = PhysicalMemoryTotal ? ((PhysicalMemoryAvailable * 100) / PhysicalMemoryTotal) : 0;

            GraphCtrl_AddPoint(&PerformancePageCpuUsageHistoryGraph, CpuUsage, CpuKernelUsage);
            GraphCtrl_AddPoint(&PerformancePageMemUsageHistoryGraph, nBarsUsed1, nBarsUsed2);
            if (pCpuUsages)
            {
                nCpus = PerfDataGetProcessorUsages(pCpuUsages, pCpuUsages + nPerCpuGraphs, nPerCpuGraphs);
                for (i = 0; i < nCpus; i++)
                {
                    GraphCtrl_AddPoint(&PerCpuGraphs[i], pCpuUsages[i], pCpuUsages[nPerCpuGraphs + i]);
                    InvalidateRect(hPerCpuGraphs[i], NULL, FALSE);
                }
            }
            InvalidateRect(hPerformancePageMemUsageHistoryGraph, NULL, FALSE);
            InvalidateRect(hPerformancePageCpuUsageHistoryGraph, NULL, FALSE);
        }
    }
    return 0;
}

void PerformancePage_OnViewShowKernelTimes(void)
{
    HMENU hMenu;
    HMENU hViewMenu;
    ULONG i;

    hMenu = PerformancePage_GetMainMenu();
    hViewMenu = GetSubMenu(hMenu, 2);

    /* Check or uncheck the show 16-bit tasks menu item */
    if (GetMenuState(hViewMenu, ID_VIEW_SHOWKERNELTIMES, MF_BYCOMMAND) & MF_CHECKED)
    {
        CheckMenuItem(hViewMenu, ID_VIEW_SHOWKERNELTIMES, MF_BYCOMMAND|MF_UNCHECKED);
        TaskManagerSettings.ShowKernelTimes = FALSE;
    }
    else
    {
        CheckMenuItem(hViewMenu, ID_VIEW_SHOWKERNELTIMES, MF_BYCOMMAND|MF_CHECKED);
        TaskManagerSettings.ShowKernelTimes = TRUE;
    }

    PerformancePageCpuUsageHistoryGraph.DrawSecondaryPlot = TaskManagerSettings.ShowKernelTimes;
    GraphCtrl_RedrawBitmap(&PerformancePageCpuUsageHistoryGraph, PerformancePageCpuUsageHistoryGraph.BitmapHeight);
    InvalidateRect(hPerformancePageCpuUsageHistoryGraph, NULL, FALSE);
    for (i = 0; i < nPerCpuGraphs; i++)
    {
        PerCpuGraphs[i].DrawSecondaryPlot = TaskManagerSettings.ShowKernelTimes;
        GraphCtrl_RedrawBitmap(&PerCpuGraphs[i], PerCpuGraphs[i].BitmapHeight);
        InvalidateRect(hPerCpuGraphs[i], NULL, FALSE);
    }
    RefreshPerformancePage();
}

void PerformancePage_SetCpuHistoryMode(BOOL bOneGraphPerCPU)
{
    HMENU hViewMenu;
    HMENU hSubMenu;
    int i, nCount;

    TaskManagerSettings.CPUHistory_OneGraphPerCPU = bOneGraphPerCPU;

    /* The CPU History submenu exists only on multiprocessor systems, and its position varies */
    hViewMenu = GetSubMenu(PerformancePage_GetMainMenu(), 2);
    nCount = GetMenuItemCount(hViewMenu);
    for (i = 0; i < nCount; i++)
    {
        hSubMenu = GetSubMenu(hViewMenu, i);
        if (hSubMenu && GetMenuState(hSubMenu, ID_VIEW_CPUHISTORY_ONEGRAPHALL, MF_BYCOMMAND) != (UINT)-1)
        {
            CheckMenuRadioItem(hSubMenu, ID_VIEW_CPUHISTORY_ONEGRAPHALL, ID_VIEW_CPUHISTORY_ONEGRAPHPERCPU,
                               bOneGraphPerCPU ? ID_VIEW_CPUHISTORY_ONEGRAPHPERCPU : ID_VIEW_CPUHISTORY_ONEGRAPHALL,
                               MF_BYCOMMAND);
        }
    }

    PerformancePage_LayoutCpuGraphs();
}

void PerformancePage_OnViewCPUHistoryOneGraphAll(void)
{
    PerformancePage_SetCpuHistoryMode(FALSE);
}

void PerformancePage_OnViewCPUHistoryOneGraphPerCPU(void)
{
    PerformancePage_SetCpuHistoryMode(TRUE);
}
