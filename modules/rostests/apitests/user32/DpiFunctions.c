/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the DPI functions of user32: DPI awareness of a process, a thread and a window,
 *              GetDpiForSystem, GetDpiForWindow, GetSystemMetricsForDpi, SystemParametersInfoForDpi,
 *              AdjustWindowRectExForDpi and the conversions of points
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expectations were measured on Windows 10 22H2 (x64 and WOW64). The process awareness can be set
 * once, so what depends on it runs in fresh processes of the helper user32_apitest_dpi.exe.
 */

#include "precomp.h"
#include <stdlib.h>
#include "DpiScenarios.h"

typedef HANDLE CTX;

static UINT (WINAPI *pGetDpiForSystem)(void);
static UINT (WINAPI *pGetDpiForWindow)(HWND);
static int (WINAPI *pGetSystemMetricsForDpi)(int, UINT);
static BOOL (WINAPI *pSystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);
static BOOL (WINAPI *pAdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
static BOOL (WINAPI *pLogicalToPhysicalPointForPerMonitorDPI)(HWND, POINT *);
static BOOL (WINAPI *pPhysicalToLogicalPointForPerMonitorDPI)(HWND, POINT *);
static CTX (WINAPI *pGetThreadDpiAwarenessContext)(void);
static BOOL (WINAPI *pGetDpiForMonitorInternal)(HMONITOR, UINT, UINT *, UINT *);

static const char *g_Exports[] =
{
    "GetDpiForSystem", "GetDpiForWindow", "GetSystemMetricsForDpi", "SystemParametersInfoForDpi",
    "AdjustWindowRectExForDpi", "SetProcessDpiAwarenessContext", "GetThreadDpiAwarenessContext",
    "SetThreadDpiAwarenessContext", "GetWindowDpiAwarenessContext", "GetAwarenessFromDpiAwarenessContext",
    "AreDpiAwarenessContextsEqual", "IsValidDpiAwarenessContext", "EnableNonClientDpiScaling",
    "LogicalToPhysicalPointForPerMonitorDPI", "PhysicalToLogicalPointForPerMonitorDPI", "GetDpiFromDpiAwarenessContext",
    "GetDpiAwarenessContextForProcess", "IsProcessDPIAware", "SetProcessDPIAware", "GetProcessDpiAwarenessInternal",
    "SetProcessDpiAwarenessInternal", "GetDpiForMonitorInternal", "GetThreadDpiHostingBehavior",
    "SetThreadDpiHostingBehavior", "GetWindowDpiHostingBehavior"
};

static BOOL
LoadFunctions(void)
{
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    BOOL Missing = FALSE;
    size_t i;

    for (i = 0; i < ARRAYSIZE(g_Exports); i++)
    {
        if (GetProcAddress(hUser32, g_Exports[i]))
            continue;
        Missing = TRUE;
        if (is_reactos())
            ok(0, "user32 does not export %s\n", g_Exports[i]);
    }
    if (Missing)
    {
        if (!is_reactos())
            skip("user32 has no DPI functions\n");
        return FALSE;
    }

    pGetDpiForSystem = (void *)GetProcAddress(hUser32, "GetDpiForSystem");
    pGetDpiForWindow = (void *)GetProcAddress(hUser32, "GetDpiForWindow");
    pGetSystemMetricsForDpi = (void *)GetProcAddress(hUser32, "GetSystemMetricsForDpi");
    pSystemParametersInfoForDpi = (void *)GetProcAddress(hUser32, "SystemParametersInfoForDpi");
    pAdjustWindowRectExForDpi = (void *)GetProcAddress(hUser32, "AdjustWindowRectExForDpi");
    pLogicalToPhysicalPointForPerMonitorDPI = (void *)GetProcAddress(hUser32, "LogicalToPhysicalPointForPerMonitorDPI");
    pPhysicalToLogicalPointForPerMonitorDPI = (void *)GetProcAddress(hUser32, "PhysicalToLogicalPointForPerMonitorDPI");
    pGetThreadDpiAwarenessContext = (void *)GetProcAddress(hUser32, "GetThreadDpiAwarenessContext");
    pGetDpiForMonitorInternal = (void *)GetProcAddress(hUser32, "GetDpiForMonitorInternal");
    return TRUE;
}

/* The structure of Windows Vista and later; the headers of this tree have the older, smaller one */
typedef struct
{
    NONCLIENTMETRICSW Old;
    int iPaddedBorderWidth;
} NCM_VISTA;

static UINT GetRealSystemDpi(void);

/* A system whose display is scaled gives an unaware thread another DPI than the one a window or a DPI-aware thread has */
static BOOL
IsDisplayScaled(void)
{
    HDC hdc = GetDC(NULL);
    UINT Dpi = GetDeviceCaps(hdc, LOGPIXELSY);

    ReleaseDC(NULL, hdc);
    return GetRealSystemDpi() != Dpi;
}

static void
test_system_dpi(void)
{
    HDC hdc = GetDC(NULL);
    UINT Dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    HWND hwnd;

    ReleaseDC(NULL, hdc);

    SetLastError(0xdeadbeef);
    ok(pGetDpiForSystem() == Dpi, "GetDpiForSystem gave %u, GDI says %u\n", pGetDpiForSystem(), Dpi);
    ok(GetLastError() == 0xdeadbeef, "GetLastError() = %lu\n", GetLastError());

    hwnd = CreateWindowExW(0, L"static", NULL, WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");
    ok(pGetDpiForWindow(hwnd) == Dpi, "GetDpiForWindow gave %u, GDI says %u\n", pGetDpiForWindow(hwnd), Dpi);
    if (!IsDisplayScaled())
        ok(pGetDpiForWindow(GetDesktopWindow()) == Dpi, "GetDpiForWindow of the desktop gave %u\n", pGetDpiForWindow(GetDesktopWindow()));
    DestroyWindow(hwnd);

    SetLastError(0xdeadbeef);
    ok(pGetDpiForWindow(NULL) == 0, "GetDpiForWindow(NULL) gave %u\n", pGetDpiForWindow(NULL));
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(pGetDpiForWindow(hwnd) == 0, "GetDpiForWindow of a destroyed window gave %u\n", pGetDpiForWindow(hwnd));
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());
}

/* Metrics that depend on the DPI, with the DPIs asked for */
static void
test_metrics(void)
{
    static const int Scaled[] =
    {
        SM_CXVSCROLL, SM_CYHSCROLL, SM_CYVSCROLL, SM_CXHSCROLL, SM_CYCAPTION, SM_CYVTHUMB, SM_CXHTHUMB, SM_CXICON, SM_CYICON, SM_CYMENU,
        SM_CXSIZE, SM_CYSIZE, SM_CXICONSPACING, SM_CYICONSPACING, SM_CXSMICON, SM_CYSMICON, SM_CYSMCAPTION,
        SM_CXSMSIZE, SM_CYSMSIZE, SM_CXMENUSIZE, SM_CYMENUSIZE
    };
    static const int Fixed[] = { SM_CXBORDER, SM_CYBORDER, SM_CXEDGE, SM_CYEDGE, SM_CXFIXEDFRAME, SM_CXSCREEN, SM_CYSCREEN };
    UINT SystemDpi = pGetDpiForSystem();
    size_t i;
    int v96, v144, v192, v288, Last, Value;

    for (i = 0; i < ARRAYSIZE(Scaled); i++)
    {
        int Index = Scaled[i];

        SetLastError(0xdeadbeef);
        Value = pGetSystemMetricsForDpi(Index, 0);
        ok(Value == 0, "index %d: %d for the DPI 0\n", Index, Value);
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "index %d: GetLastError() = %lu\n", Index, GetLastError());

        /* ReactOS gives the icons a fixed size whatever the DPI of the system; the function scales them */
        if (SystemDpi == 96 || (Index != SM_CXICON && Index != SM_CYICON && Index != SM_CXSMICON && Index != SM_CYSMICON))
            ok(pGetSystemMetricsForDpi(Index, SystemDpi) == GetSystemMetrics(Index), "index %d: %d, GetSystemMetrics says %d\n",
               Index, pGetSystemMetricsForDpi(Index, SystemDpi), GetSystemMetrics(Index));

        v96 = pGetSystemMetricsForDpi(Index, 96);
        v144 = pGetSystemMetricsForDpi(Index, 144);
        v192 = pGetSystemMetricsForDpi(Index, 192);
        v288 = pGetSystemMetricsForDpi(Index, 288);
        ok(v96 > 0, "index %d: %d at 96 DPI\n", Index, v96);
        ok(v96 <= v144 && v144 <= v192 && v192 <= v288, "index %d: %d, %d, %d, %d do not grow with the DPI\n",
           Index, v96, v144, v192, v288);
        /* Twice the DPI gives about twice the size (the parts are rounded one by one) */
        ok(abs(v192 - 2 * v96) <= 2, "index %d: %d at 96 DPI, %d at 192 DPI\n", Index, v96, v192);
        ok(abs(v288 - 3 * v96) <= 3, "index %d: %d at 96 DPI, %d at 288 DPI\n", Index, v96, v288);
    }

    for (i = 0; i < ARRAYSIZE(Fixed); i++)
    {
        int Index = Fixed[i];

        Value = GetSystemMetrics(Index);
        ok(pGetSystemMetricsForDpi(Index, 96) == Value, "index %d: %d at 96 DPI, %d\n", Index, pGetSystemMetricsForDpi(Index, 96), Value);
        ok(pGetSystemMetricsForDpi(Index, 192) == Value, "index %d: %d at 192 DPI, %d\n", Index, pGetSystemMetricsForDpi(Index, 192), Value);
    }

    /* The frame is the dialog frame and a border that grows with the DPI */
    Last = 0;
    for (i = 96; i <= 480; i += 48)
    {
        Value = pGetSystemMetricsForDpi(SM_CXFRAME, i);
        ok(Value >= Last, "SM_CXFRAME at %Iu DPI: %d after %d\n", i, Value, Last);
        ok(Value == pGetSystemMetricsForDpi(SM_CYFRAME, i), "SM_CXFRAME and SM_CYFRAME differ at %Iu DPI\n", i);
        Last = Value;
    }
    ok(pGetSystemMetricsForDpi(SM_CXFRAME, 96) >= GetSystemMetrics(SM_CXDLGFRAME), "SM_CXFRAME is smaller than SM_CXDLGFRAME\n");

    /* The padded border exists at every DPI, whether or not GetSystemMetrics has one */
    ok(pGetSystemMetricsForDpi(92, 96) == 4, "padded border at 96 DPI: %d\n", pGetSystemMetricsForDpi(92, 96));
    ok(pGetSystemMetricsForDpi(92, 192) == 8, "padded border at 192 DPI: %d\n", pGetSystemMetricsForDpi(92, 192));

    /* Icons and cursors have their own steps */
    ok(pGetSystemMetricsForDpi(SM_CXICON, 120) == 40, "icon at 120 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXICON, 120));
    ok(pGetSystemMetricsForDpi(SM_CXICON, 192) == 64, "icon at 192 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXICON, 192));
    ok(pGetSystemMetricsForDpi(SM_CXSMICON, 120) == 20, "small icon at 120 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXSMICON, 120));
    ok(pGetSystemMetricsForDpi(SM_CXCURSOR, 96) == 32, "cursor at 96 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXCURSOR, 96));
    ok(pGetSystemMetricsForDpi(SM_CXCURSOR, 120) == 32, "cursor at 120 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXCURSOR, 120));
    ok(pGetSystemMetricsForDpi(SM_CXCURSOR, 144) == 48, "cursor at 144 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXCURSOR, 144));
    ok(pGetSystemMetricsForDpi(SM_CXCURSOR, 192) == 64, "cursor at 192 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXCURSOR, 192));
    ok(pGetSystemMetricsForDpi(SM_CXCURSOR, 288) == 96, "cursor at 288 DPI: %d\n", pGetSystemMetricsForDpi(SM_CXCURSOR, 288));

    /* No metric, and no DPI, gives anything for an index that does not exist */
    ok(pGetSystemMetricsForDpi(-1, 96) == 0, "index -1 gave %d\n", pGetSystemMetricsForDpi(-1, 96));
    ok(pGetSystemMetricsForDpi(200, 96) == 0, "index 200 gave %d\n", pGetSystemMetricsForDpi(200, 96));
}

static void
test_spi(void)
{
    NCM_VISTA Ncm96, Ncm192, Ncm;
    ICONMETRICSW Im96, Im192;
    LOGFONTW Lf96, Lf192;
    UINT Value;
    BOOL Ret;

    memset(&Ncm96, 0xcc, sizeof(Ncm96));
    Ncm96.Old.cbSize = sizeof(Ncm96);
    SetLastError(0xdeadbeef);
    Ret = pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Ncm96), &Ncm96, 0, 96);
    ok(Ret, "SPI_GETNONCLIENTMETRICS at 96 DPI failed: %lu\n", GetLastError());
    ok(GetLastError() == 0xdeadbeef, "GetLastError() = %lu\n", GetLastError());
    if (!Ret)
        return;
    memset(&Ncm192, 0xcc, sizeof(Ncm192));
    Ncm192.Old.cbSize = sizeof(Ncm192);
    Ret = pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Ncm192), &Ncm192, 0, 192);
    ok(Ret, "SPI_GETNONCLIENTMETRICS at 192 DPI failed: %lu\n", GetLastError());
    ok(Ncm96.Old.cbSize == sizeof(Ncm96), "cbSize is %u\n", Ncm96.Old.cbSize);
    ok(Ncm96.iPaddedBorderWidth == 4, "padded border at 96 DPI: %d\n", Ncm96.iPaddedBorderWidth);
    ok(Ncm192.iPaddedBorderWidth == 8, "padded border at 192 DPI: %d\n", Ncm192.iPaddedBorderWidth);
    ok(Ncm96.Old.iBorderWidth >= 1 && Ncm192.Old.iBorderWidth == 2 * Ncm96.Old.iBorderWidth, "border %d at 96, %d at 192 DPI\n",
       Ncm96.Old.iBorderWidth, Ncm192.Old.iBorderWidth);
    ok(abs(Ncm192.Old.iScrollWidth - 2 * Ncm96.Old.iScrollWidth) <= 1, "scroll width %d at 96, %d at 192 DPI\n",
       Ncm96.Old.iScrollWidth, Ncm192.Old.iScrollWidth);
    ok(abs(Ncm192.Old.iCaptionHeight - 2 * Ncm96.Old.iCaptionHeight) <= 1, "caption height %d at 96, %d at 192 DPI\n",
       Ncm96.Old.iCaptionHeight, Ncm192.Old.iCaptionHeight);
    ok(abs(Ncm192.Old.iMenuHeight - 2 * Ncm96.Old.iMenuHeight) <= 1, "menu height %d at 96, %d at 192 DPI\n",
       Ncm96.Old.iMenuHeight, Ncm192.Old.iMenuHeight);
    ok(abs(Ncm192.Old.iSmCaptionHeight - 2 * Ncm96.Old.iSmCaptionHeight) <= 1, "small caption height %d at 96, %d at 192 DPI\n",
       Ncm96.Old.iSmCaptionHeight, Ncm192.Old.iSmCaptionHeight);
    ok(Ncm96.Old.lfMessageFont.lfHeight < 0, "message font height %ld at 96 DPI\n", Ncm96.Old.lfMessageFont.lfHeight);
    ok(Ncm192.Old.lfMessageFont.lfHeight == 2 * Ncm96.Old.lfMessageFont.lfHeight, "message font height %ld at 96, %ld at 192 DPI\n",
       Ncm96.Old.lfMessageFont.lfHeight, Ncm192.Old.lfMessageFont.lfHeight);
    ok(Ncm192.Old.lfMenuFont.lfHeight == 2 * Ncm96.Old.lfMenuFont.lfHeight, "menu font height %ld at 96, %ld at 192 DPI\n",
       Ncm96.Old.lfMenuFont.lfHeight, Ncm192.Old.lfMenuFont.lfHeight);

    /* Only the structure of Windows Vista and later, with the size in the structure and in uiParam */
    memset(&Ncm, 0xcc, sizeof(Ncm));
    Ncm.Old.cbSize = sizeof(Ncm.Old);
    SetLastError(0xdeadbeef);
    Ret = pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Ncm.Old), &Ncm, 0, 96);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "old size: %d, error %lu\n", Ret, GetLastError());
    Ncm.Old.cbSize = sizeof(Ncm);
    SetLastError(0xdeadbeef);
    Ret = pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, 0, &Ncm, 0, 96);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "uiParam 0: %d, error %lu\n", Ret, GetLastError());

    memset(&Im96, 0xcc, sizeof(Im96));
    Im96.cbSize = sizeof(Im96);
    SetLastError(0xdeadbeef);
    Ret = pSystemParametersInfoForDpi(SPI_GETICONMETRICS, sizeof(Im96), &Im96, 0, 96);
    ok(Ret, "SPI_GETICONMETRICS at 96 DPI failed: %lu\n", GetLastError());
    memset(&Im192, 0xcc, sizeof(Im192));
    Im192.cbSize = sizeof(Im192);
    Ret = pSystemParametersInfoForDpi(SPI_GETICONMETRICS, sizeof(Im192), &Im192, 0, 192);
    ok(Ret, "SPI_GETICONMETRICS at 192 DPI failed: %lu\n", GetLastError());
    ok(abs(Im192.iHorzSpacing - 2 * Im96.iHorzSpacing) <= 1, "icon spacing %d at 96, %d at 192 DPI\n", Im96.iHorzSpacing, Im192.iHorzSpacing);
    ok(abs(Im192.iVertSpacing - 2 * Im96.iVertSpacing) <= 1, "icon spacing %d at 96, %d at 192 DPI\n", Im96.iVertSpacing, Im192.iVertSpacing);
    ok(Im192.lfFont.lfHeight == 2 * Im96.lfFont.lfHeight, "icon font height %ld at 96, %ld at 192 DPI\n", Im96.lfFont.lfHeight, Im192.lfFont.lfHeight);
    ok(Im96.iTitleWrap == Im192.iTitleWrap, "title wrap %d at 96, %d at 192 DPI\n", Im96.iTitleWrap, Im192.iTitleWrap);

    memset(&Lf96, 0xcc, sizeof(Lf96));
    Ret = pSystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(Lf96), &Lf96, 0, 96);
    ok(Ret, "SPI_GETICONTITLELOGFONT at 96 DPI failed: %lu\n", GetLastError());
    memset(&Lf192, 0xcc, sizeof(Lf192));
    Ret = pSystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(Lf192), &Lf192, 0, 192);
    ok(Ret, "SPI_GETICONTITLELOGFONT at 192 DPI failed: %lu\n", GetLastError());
    ok(Lf192.lfHeight == 2 * Lf96.lfHeight, "title font height %ld at 96, %ld at 192 DPI\n", Lf96.lfHeight, Lf192.lfHeight);

    /* Anything else does not depend on the DPI and is refused, unless there is no DPI */
    Value = 0xcccc;
    SetLastError(0xdeadbeef);
    Ret = pSystemParametersInfoForDpi(SPI_GETKEYBOARDDELAY, 0, &Value, 0, 96);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "SPI_GETKEYBOARDDELAY: %d, error %lu\n", Ret, GetLastError());
    ok(Value == 0xcccc, "SPI_GETKEYBOARDDELAY wrote %u\n", Value);
    Value = 0xcccc;
    Ret = pSystemParametersInfoForDpi(SPI_GETKEYBOARDDELAY, 0, &Value, 0, 0);
    ok(Ret && Value != 0xcccc, "SPI_GETKEYBOARDDELAY without a DPI: %d, %u\n", Ret, Value);
    memset(&Ncm, 0xcc, sizeof(Ncm));
    Ncm.Old.cbSize = sizeof(Ncm.Old);
    Ret = pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Ncm.Old), &Ncm, 0, 0);
    ok(Ret, "SPI_GETNONCLIENTMETRICS without a DPI failed: %lu\n", GetLastError());
}

static void
test_adjust_window_rect(void)
{
    static const struct { DWORD Style, ExStyle; BOOL Menu; } Cases[] =
    {
        { WS_OVERLAPPEDWINDOW, 0, FALSE },
        { WS_OVERLAPPEDWINDOW, 0, TRUE },
        { WS_POPUP, 0, FALSE },
        { WS_CAPTION, 0, FALSE },
        { WS_BORDER, 0, FALSE },
        { WS_THICKFRAME, 0, FALSE },
        { WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, FALSE },
        { WS_OVERLAPPEDWINDOW, WS_EX_CLIENTEDGE, FALSE },
    };
    UINT SystemDpi = pGetDpiForSystem();
    size_t i;
    RECT Expected, Rect, Smaller;
    BOOL Ret;

    for (i = 0; i < ARRAYSIZE(Cases); i++)
    {
        SetRect(&Expected, 0, 0, 100, 100);
        ok(AdjustWindowRectEx(&Expected, Cases[i].Style, Cases[i].Menu, Cases[i].ExStyle), "AdjustWindowRectEx failed\n");

        SetRect(&Rect, 0, 0, 100, 100);
        SetLastError(0xdeadbeef);
        Ret = pAdjustWindowRectExForDpi(&Rect, Cases[i].Style, Cases[i].Menu, Cases[i].ExStyle, SystemDpi);
        ok(Ret, "case %Iu: failed at the DPI of the system\n", i);
        ok(EqualRect(&Rect, &Expected), "case %Iu: %ld,%ld,%ld,%ld, AdjustWindowRectEx says %ld,%ld,%ld,%ld\n", i,
           Rect.left, Rect.top, Rect.right, Rect.bottom, Expected.left, Expected.top, Expected.right, Expected.bottom);

        /* The area of the client stays inside, and grows with the DPI */
        SetRect(&Smaller, 0, 0, 100, 100);
        ok(pAdjustWindowRectExForDpi(&Smaller, Cases[i].Style, Cases[i].Menu, Cases[i].ExStyle, 96), "case %Iu: failed at 96 DPI\n", i);
        SetRect(&Rect, 0, 0, 100, 100);
        ok(pAdjustWindowRectExForDpi(&Rect, Cases[i].Style, Cases[i].Menu, Cases[i].ExStyle, 192), "case %Iu: failed at 192 DPI\n", i);
        ok(Rect.left <= Smaller.left && Rect.top <= Smaller.top && Rect.right >= Smaller.right && Rect.bottom >= Smaller.bottom,
           "case %Iu: %ld,%ld,%ld,%ld at 192 DPI is inside %ld,%ld,%ld,%ld at 96 DPI\n", i, Rect.left, Rect.top, Rect.right,
           Rect.bottom, Smaller.left, Smaller.top, Smaller.right, Smaller.bottom);
        ok(Rect.left <= 0 && Rect.top <= 0 && Rect.right >= 100 && Rect.bottom >= 100, "case %Iu: client area is outside\n", i);
    }

    /* A window without a border or caption has nothing to add at any DPI */
    SetRect(&Rect, 0, 0, 100, 100);
    ok(pAdjustWindowRectExForDpi(&Rect, WS_POPUP, FALSE, 0, 192), "WS_POPUP failed\n");
    ok(Rect.left == 0 && Rect.top == 0 && Rect.right == 100 && Rect.bottom == 100, "WS_POPUP: %ld,%ld,%ld,%ld\n",
       Rect.left, Rect.top, Rect.right, Rect.bottom);

    SetRect(&Rect, 0, 0, 100, 100);
    SetLastError(0xdeadbeef);
    Ret = pAdjustWindowRectExForDpi(&Rect, WS_OVERLAPPEDWINDOW, FALSE, 0, 0);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "DPI 0: %d, error %lu\n", Ret, GetLastError());
    ok(Rect.left == 0 && Rect.top == 0 && Rect.right == 100 && Rect.bottom == 100, "DPI 0 changed the rectangle\n");
}

static void
test_points(void)
{
    HWND hwnd = CreateWindowExW(0, L"static", NULL, WS_POPUP | WS_VISIBLE, 100, 100, 50, 60, NULL, NULL, NULL, NULL);
    HWND child = CreateWindowExW(0, L"static", NULL, WS_CHILD | WS_VISIBLE, 10, 10, 20, 20, hwnd, NULL, NULL, NULL);
    RECT Rect, ChildRect;
    POINT pt;
    BOOL Ret;
    int Step;

    ok(hwnd != NULL && child != NULL, "CreateWindowEx failed\n");
    GetWindowRect(hwnd, &Rect);
    GetWindowRect(child, &ChildRect);

    /* One DPI for every monitor: nothing to convert, but the point has to be in the window, its right
     * and bottom edges included */
    for (Step = 0; Step < 2; Step++)
    {
        BOOL (WINAPI *Convert)(HWND, POINT *) = Step ? pPhysicalToLogicalPointForPerMonitorDPI : pLogicalToPhysicalPointForPerMonitorDPI;
        int dx, dy;

        for (dx = -1; dx <= 1; dx++)
        {
            for (dy = -1; dy <= 1; dy++)
            {
                BOOL Inside;

                pt.x = Rect.left + dx;
                pt.y = Rect.top + dy;
                SetLastError(0xdeadbeef);
                Ret = Convert(hwnd, &pt);
                Inside = dx >= 0 && dy >= 0;
                ok(Ret == Inside, "step %d: top left %+d,%+d gave %d\n", Step, dx, dy, Ret);
                ok(pt.x == Rect.left + dx && pt.y == Rect.top + dy, "step %d: %ld,%ld\n", Step, pt.x, pt.y);
                ok(GetLastError() == 0xdeadbeef, "step %d: GetLastError() = %lu\n", Step, GetLastError());

                pt.x = Rect.right + dx;
                pt.y = Rect.bottom + dy;
                Ret = Convert(hwnd, &pt);
                Inside = dx <= 0 && dy <= 0;
                ok(Ret == Inside, "step %d: bottom right %+d,%+d gave %d\n", Step, dx, dy, Ret);
            }
        }

        pt.x = ChildRect.left;
        pt.y = ChildRect.top;
        ok(Convert(child, &pt), "step %d: a point of the child failed\n", Step);
        pt.x = ChildRect.left - 5;
        pt.y = ChildRect.top;
        ok(!Convert(child, &pt), "step %d: a point left of the child worked\n", Step);
        pt.x = 5000;
        pt.y = 5000;
        ok(!Convert(GetDesktopWindow(), &pt), "step %d: a point far from the screen worked\n", Step);

        /* The window may be NULL; then any point will do */
        pt.x = -5000;
        pt.y = 6;
        SetLastError(0xdeadbeef);
        ok(Convert(NULL, &pt), "step %d: a NULL window failed\n", Step);
        ok(pt.x == -5000 && pt.y == 6, "step %d: converted to %ld,%ld\n", Step, pt.x, pt.y);

        SetLastError(0xdeadbeef);
        Ret = Convert((HWND)(ULONG_PTR)0x1234567, &pt);
        ok(!Ret && GetLastError() == ERROR_INVALID_WINDOW_HANDLE, "step %d: invalid window: %d, error %lu\n", Step, Ret, GetLastError());
    }

    DestroyWindow(child);
    DestroyWindow(hwnd);
}

static void
test_monitor(void)
{
    POINT Zero = { 0, 0 };
    HMONITOR hMonitor = MonitorFromPoint(Zero, MONITOR_DEFAULTTOPRIMARY);
    UINT SystemDpi = pGetDpiForSystem(), x, y;
    BOOL Ret;

    /* The effective and angular DPI are the DPI of the system */
    x = y = 0x5555;
    SetLastError(0xdeadbeef);
    Ret = pGetDpiForMonitorInternal(hMonitor, 0, &x, &y);
    ok(Ret && x == SystemDpi && y == SystemDpi, "effective DPI: %d, %u x %u, expected %u\n", Ret, x, y, SystemDpi);
    ok(GetLastError() == 0xdeadbeef, "GetLastError() = %lu\n", GetLastError());
    x = y = 0x5555;
    Ret = pGetDpiForMonitorInternal(hMonitor, 1, &x, &y);
    ok(Ret && x == SystemDpi && y == SystemDpi, "angular DPI: %d, %u x %u\n", Ret, x, y);

    /* The raw DPI needs the size of the screen: without it the call fails, as on Windows without
     * the data of the monitor, and with it (a real monitor) it gives a DPI */
    x = y = 0x5555;
    SetLastError(0xdeadbeef);
    Ret = pGetDpiForMonitorInternal(hMonitor, 2, &x, &y);
    if (Ret)
    {
        ok(x != 0 && y != 0 && x != 0x5555 && y != 0x5555, "raw DPI: %u x %u\n", x, y);
    }
    else
    {
        ok(GetLastError() == ERROR_NOT_SUPPORTED, "raw DPI: error %lu\n", GetLastError());
        ok(x == 0 && y == 0, "raw DPI: %u x %u\n", x, y);
    }

    x = y = 0x5555;
    SetLastError(0xdeadbeef);
    Ret = pGetDpiForMonitorInternal(hMonitor, 3, &x, &y);
    ok(!Ret && GetLastError() == ERROR_BAD_ARGUMENTS, "type 3: %d, error %lu\n", Ret, GetLastError());
    ok(x == 0x5555 && y == 0x5555, "type 3: %u x %u\n", x, y);
    SetLastError(0xdeadbeef);
    Ret = pGetDpiForMonitorInternal(NULL, 0, &x, &y);
    ok(!Ret && GetLastError() == ERROR_INVALID_HANDLE, "NULL monitor: %d, error %lu\n", Ret, GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pGetDpiForMonitorInternal(hMonitor, 0, NULL, &y);
    ok(!Ret && GetLastError() == ERROR_INVALID_ADDRESS, "NULL x: %d, error %lu\n", Ret, GetLastError());
}

START_TEST(DpiFunctions)
{
    if (!LoadFunctions())
        return;

    test_system_dpi();
    test_metrics();
    test_spi();
    test_adjust_window_rect();
    if (IsDisplayScaled())
        skip("The points of a scaled display are converted\n");
    else
        test_points();
    test_monitor();
}

/* Run the helper with a manifest or without, and collect what it prints */
static BOOL
RunHelper(const DPI_SCENARIO *Scenario, char *Output, DWORD OutputSize)
{
    WCHAR Dir[MAX_PATH], HelperPath[MAX_PATH], Copy[MAX_PATH], ManifestPath[MAX_PATH + 16], OutPath[MAX_PATH + 16], Cmd[MAX_PATH * 2 + 512];
    SECURITY_ATTRIBUTES Inherit = { sizeof(Inherit), NULL, TRUE };
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Info;
    HANDLE Read, Write;
    DWORD Total = 0, Got;
    WCHAR *Slash;
    BOOL Ok;

    GetModuleFileNameW(NULL, Dir, ARRAYSIZE(Dir));
    Slash = wcsrchr(Dir, L'\\');
    if (!Slash)
        return FALSE;
    Slash[1] = UNICODE_NULL;
    _snwprintf(HelperPath, ARRAYSIZE(HelperPath) - 1, L"%lsuser32_apitest_dpi.exe", Dir);
    HelperPath[ARRAYSIZE(HelperPath) - 1] = UNICODE_NULL;

    /* A manifest next to a copy of the helper decides the DPI awareness of its process */
    GetTempPathW(ARRAYSIZE(Dir), Dir);
    _snwprintf(Copy, ARRAYSIZE(Copy) - 1, L"%lsdpitest-%lu-%lu.exe", Dir, GetCurrentProcessId(), GetTickCount());
    Copy[ARRAYSIZE(Copy) - 1] = UNICODE_NULL;
    if (!CopyFileW(HelperPath, Copy, FALSE))
    {
        skip("Cannot copy the helper: %lu\n", GetLastError());
        return FALSE;
    }
    _snwprintf(ManifestPath, ARRAYSIZE(ManifestPath) - 1, L"%ls.manifest", Copy);
    ManifestPath[ARRAYSIZE(ManifestPath) - 1] = UNICODE_NULL;
    if (Scenario->Manifest)
    {
        const DPI_MANIFEST *Manifest = NULL;
        char Text[1024], Settings[512] = "";
        HANDLE File;
        size_t i;

        for (i = 0; i < ARRAYSIZE(g_DpiManifests); i++)
        {
            if (!strcmp(g_DpiManifests[i].Name, Scenario->Manifest))
                Manifest = &g_DpiManifests[i];
        }
        if (!Manifest)
        {
            DeleteFileW(Copy);
            return FALSE;
        }
        if (Manifest->DpiAware)
            sprintf(Settings + strlen(Settings), "<dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">%s</dpiAware>\n",
                    Manifest->DpiAware);
        if (Manifest->DpiAwareness)
            sprintf(Settings + strlen(Settings), "<dpiAwareness xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">%s</dpiAwareness>\n",
                    Manifest->DpiAwareness);
        sprintf(Text,
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
                "<assemblyIdentity type=\"win32\" name=\"ReactOS.Apitest.DpiProbe\" version=\"1.0.0.0\"/>\n"
                "<compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\"><application>\n"
                "<supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/>\n"
                "</application></compatibility>\n"
                "<application xmlns=\"urn:schemas-microsoft-com:asm.v3\"><windowsSettings>\n%s</windowsSettings></application>\n"
                "</assembly>\n", Settings);
        File = CreateFileW(ManifestPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (File == INVALID_HANDLE_VALUE)
        {
            DeleteFileW(Copy);
            return FALSE;
        }
        WriteFile(File, Text, strlen(Text), &Got, NULL);
        CloseHandle(File);
    }

    /* The output goes to a file, so that a helper that hangs cannot hold this test up */
    _snwprintf(OutPath, ARRAYSIZE(OutPath) - 1, L"%ls.out", Copy);
    OutPath[ARRAYSIZE(OutPath) - 1] = UNICODE_NULL;
    Write = CreateFileW(OutPath, GENERIC_WRITE, FILE_SHARE_READ, &Inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Write == INVALID_HANDLE_VALUE)
    {
        DeleteFileW(Copy);
        if (Scenario->Manifest)
            DeleteFileW(ManifestPath);
        return FALSE;
    }
    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdOutput = Write;
    Startup.hStdError = Write;
    Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    _snwprintf(Cmd, ARRAYSIZE(Cmd) - 1, L"\"%ls\" %hs", Copy, Scenario->Args);
    Cmd[ARRAYSIZE(Cmd) - 1] = UNICODE_NULL;
    Ok = CreateProcessW(NULL, Cmd, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Info);
    CloseHandle(Write);
    if (Ok)
    {
        DWORD Exit = 0;

        CloseHandle(Info.hThread);
        if (WaitForSingleObject(Info.hProcess, 60000) != WAIT_OBJECT_0)
        {
            TerminateProcess(Info.hProcess, 0xdead);
            WaitForSingleObject(Info.hProcess, 10000);
            ok(0, "%s: the helper did not finish\n", Scenario->Name);
        }
        else if (GetExitCodeProcess(Info.hProcess, &Exit))
        {
            ok(Exit == 0, "%s: the helper exited with 0x%lx\n", Scenario->Name, Exit);
        }
        CloseHandle(Info.hProcess);
        Read = CreateFileW(OutPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (Read != INVALID_HANDLE_VALUE)
        {
            while (Total < OutputSize - 1 && ReadFile(Read, Output + Total, OutputSize - 1 - Total, &Got, NULL) && Got)
                Total += Got;
            CloseHandle(Read);
        }
    }
    Output[Total] = ANSI_NULL;
    DeleteFileW(OutPath);
    DeleteFileW(Copy);
    if (Scenario->Manifest)
        DeleteFileW(ManifestPath);
    return Ok;
}

/* Lines of a text, without the carriage returns that a pipe adds */
static void
CompareOutput(const DPI_SCENARIO *Scenario, const char *Output)
{
    const char *Got = Output, *Want = Scenario->Expected;
    int Line = 1;

    while (*Got || *Want)
    {
        const char *GotEnd = strchr(Got, '\n'), *WantEnd = strchr(Want, '\n');
        size_t GotLen = GotEnd ? (size_t)(GotEnd - Got) : strlen(Got);
        size_t WantLen = WantEnd ? (size_t)(WantEnd - Want) : strlen(Want);

        if (GotLen && Got[GotLen - 1] == '\r')
            GotLen--;
        ok(GotLen == WantLen && !strncmp(Got, Want, WantLen), "%s, line %d: got '%.*s', expected '%.*s'\n", Scenario->Name,
           Line, (int)GotLen, Got, (int)WantLen, Want);
        Got = GotEnd ? GotEnd + 1 : Got + strlen(Got);
        Want = WantEnd ? WantEnd + 1 : Want + strlen(Want);
        Line++;
    }
}

/* The DPI of the system as a DPI-aware thread sees it (an unaware thread of Windows sees 96) */
static UINT
GetRealSystemDpi(void)
{
    CTX (WINAPI *pSetThreadDpiAwarenessContext)(CTX) =
        (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    CTX Previous = pSetThreadDpiAwarenessContext((CTX)(INT_PTR)-2);
    UINT Dpi = pGetDpiForSystem();

    pSetThreadDpiAwarenessContext(Previous);
    return Dpi;
}

START_TEST(DpiProcess)
{
    static char Output[64 * 1024];
    size_t i;
    BOOL HasManifestReader;

    if (!LoadFunctions())
        return;
    if (GetRealSystemDpi() != 96)
    {
        skip("The expectations are those of a system with 96 DPI\n");
        return;
    }
    if (!is_reactos() && GetNTVersion() < _WIN32_WINNT_WIN10)
    {
        skip("The expectations are those of Windows 10\n");
        return;
    }

    HasManifestReader = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlQueryActivationContextApplicationSettings") != NULL;

    for (i = 0; i < ARRAYSIZE(g_DpiScenarios); i++)
    {
        /* A system whose ntdll cannot read the settings of a manifest does not apply them */
        if (g_DpiScenarios[i].Manifest && !HasManifestReader)
            continue;
        if (!RunHelper(&g_DpiScenarios[i], Output, sizeof(Output)))
        {
            ok(0, "%s: the helper did not run\n", g_DpiScenarios[i].Name);
            continue;
        }
        CompareOutput(&g_DpiScenarios[i], Output);
    }
}
