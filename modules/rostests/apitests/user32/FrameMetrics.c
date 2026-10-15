/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The width of the frame of a window is SM_CXFRAME plus SM_CXPADDEDBORDER, whatever the function
 *              that tells it. Programs that draw their own title bar (Firefox, Chromium) take the margins of
 *              the client area from these metrics, so a metric that does not match the frame leaves a strip
 *              of the window that nobody paints.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SM_CXPADDEDBORDER
#define SM_CXPADDEDBORDER 92
#endif

static int (WINAPI *pGetSystemMetricsForDpi)(int, UINT);
static BOOL (WINAPI *pSystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);

/* NONCLIENTMETRICSW of Windows Vista and later: 504 bytes that end with iPaddedBorderWidth */
typedef struct _NCM504
{
    NONCLIENTMETRICSW Ncm;
    int iPaddedBorderWidth;
} NCM504;
C_ASSERT(sizeof(NCM504) == 504);

static UINT (WINAPI *pGetDpiForSystem)(void);

static void
test_adjust_rect(int Frame, int Caption)
{
    RECT Rect = { 100, 100, 400, 300 };

    AdjustWindowRectEx(&Rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    ok(Rect.left == 100 - Frame, "AdjustWindowRectEx: left border %ld, metrics say %d\n", 100 - Rect.left, Frame);
    ok(Rect.right == 400 + Frame, "AdjustWindowRectEx: right border %ld, metrics say %d\n", Rect.right - 400, Frame);
    ok(Rect.bottom == 300 + Frame, "AdjustWindowRectEx: bottom border %ld, metrics say %d\n", Rect.bottom - 300, Frame);
    ok(Rect.top == 100 - Frame - Caption, "AdjustWindowRectEx: top border %ld, metrics say %d\n", 100 - Rect.top, Frame + Caption);
}

static void
test_window_frame(int Frame, int Caption)
{
    RECT Window, Client;
    POINT Origin = { 0, 0 };
    HWND hWnd = CreateWindowExW(0, L"STATIC", L"frame", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, NULL, NULL, NULL, NULL);

    ok(hWnd != NULL, "CreateWindowEx failed: %lu\n", GetLastError());
    if (!hWnd)
        return;
    GetWindowRect(hWnd, &Window);
    GetClientRect(hWnd, &Client);
    ClientToScreen(hWnd, &Origin);
    ok(Origin.x - Window.left == Frame, "left border of the window %ld, metrics say %d\n", Origin.x - Window.left, Frame);
    ok(Window.right - (Origin.x + Client.right) == Frame, "right border of the window %ld, metrics say %d\n",
       Window.right - (Origin.x + Client.right), Frame);
    ok(Window.bottom - (Origin.y + Client.bottom) == Frame, "bottom border of the window %ld, metrics say %d\n",
       Window.bottom - (Origin.y + Client.bottom), Frame);
    ok(Origin.y - Window.top == Frame + Caption, "top border of the window %ld, metrics say %d\n", Origin.y - Window.top, Frame + Caption);

    /* A maximized window reaches over the work area by its frame */
    ShowWindow(hWnd, SW_MAXIMIZE);
    GetWindowRect(hWnd, &Window);
    {
        RECT Work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &Work, 0);
        ok(Window.left == Work.left - Frame, "maximized: left %ld, work area %ld, frame %d\n", Window.left, Work.left, Frame);
        ok(Window.top == Work.top - Frame, "maximized: top %ld, work area %ld, frame %d\n", Window.top, Work.top, Frame);
        ok(Window.right == Work.right + Frame, "maximized: right %ld, work area %ld, frame %d\n", Window.right, Work.right, Frame);
        ok(Window.bottom == Work.bottom + Frame, "maximized: bottom %ld, work area %ld, frame %d\n", Window.bottom, Work.bottom, Frame);
    }
    DestroyWindow(hWnd);
}

static void
test_for_dpi(int Frame)
{
    UINT Dpi = pGetDpiForSystem();
    int Value = pGetSystemMetricsForDpi(SM_CXFRAME, Dpi) + pGetSystemMetricsForDpi(SM_CXPADDEDBORDER, Dpi);

    /* Windows 10 splits the width into 4 and 4 here and into 8 and 0 in GetSystemMetrics of a program that is not DPI aware */
    ok(Value == Frame, "GetSystemMetricsForDpi at %u DPI: frame and padded border add to %d, GetSystemMetrics says %d\n", Dpi, Value, Frame);
    ok(pGetSystemMetricsForDpi(SM_CYFRAME, Dpi) + pGetSystemMetricsForDpi(SM_CXPADDEDBORDER, Dpi) == Value,
       "SM_CYFRAME and SM_CXFRAME differ at %u DPI\n", Dpi);
}

/* The frame is the border width of the metrics, the dialog frame and the padded border */
static void
test_ncm(int Frame)
{
    NCM504 Ncm;
    int DlgFrame = GetSystemMetrics(SM_CXDLGFRAME);

    memset(&Ncm, 0xcc, sizeof(Ncm));
    Ncm.Ncm.cbSize = sizeof(Ncm);
    ok(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Ncm), &Ncm, 0), "SPI_GETNONCLIENTMETRICS failed: %lu\n", GetLastError());
    ok(Ncm.Ncm.iBorderWidth + DlgFrame + Ncm.iPaddedBorderWidth == Frame,
       "SystemParametersInfo: border %d + dialog frame %d + padded border %d, the frame is %d\n",
       Ncm.Ncm.iBorderWidth, DlgFrame, Ncm.iPaddedBorderWidth, Frame);

    if (pSystemParametersInfoForDpi && pGetSystemMetricsForDpi)
    {
        UINT Dpi = pGetDpiForSystem();

        memset(&Ncm, 0xcc, sizeof(Ncm));
        Ncm.Ncm.cbSize = sizeof(Ncm);
        ok(pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Ncm), &Ncm, 0, Dpi),
           "SPI_GETNONCLIENTMETRICS for %u DPI failed: %lu\n", Dpi, GetLastError());
        DlgFrame = pGetSystemMetricsForDpi(SM_CXDLGFRAME, Dpi);
        ok(Ncm.Ncm.iBorderWidth + DlgFrame + Ncm.iPaddedBorderWidth == Frame,
           "SystemParametersInfoForDpi at %u DPI: border %d + dialog frame %d + padded border %d, the frame is %d\n",
           Dpi, Ncm.Ncm.iBorderWidth, DlgFrame, Ncm.iPaddedBorderWidth, Frame);
    }
}

START_TEST(FrameMetrics)
{
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    int Frame = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    int Caption = GetSystemMetrics(SM_CYCAPTION);

    trace("SM_CXFRAME %d, SM_CYFRAME %d, SM_CXPADDEDBORDER %d, SM_CYCAPTION %d, SM_CXEDGE %d\n", GetSystemMetrics(SM_CXFRAME),
          GetSystemMetrics(SM_CYFRAME), GetSystemMetrics(SM_CXPADDEDBORDER), Caption, GetSystemMetrics(SM_CXEDGE));
    ok(GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER) == Frame, "SM_CYFRAME and SM_CXFRAME differ\n");

    test_adjust_rect(Frame, Caption);
    test_window_frame(Frame, Caption);

    pGetSystemMetricsForDpi = (void *)GetProcAddress(hUser32, "GetSystemMetricsForDpi");
    pGetDpiForSystem = (void *)GetProcAddress(hUser32, "GetDpiForSystem");
    pSystemParametersInfoForDpi = (void *)GetProcAddress(hUser32, "SystemParametersInfoForDpi");
    test_ncm(Frame);
    if (pGetSystemMetricsForDpi && pGetDpiForSystem)
        test_for_dpi(Frame);
    else
        skip("GetSystemMetricsForDpi is not available\n");
}
