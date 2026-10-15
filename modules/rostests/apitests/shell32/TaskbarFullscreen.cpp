/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Test that the taskbar goes back on top after a full-screen window is gone
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"

#define WAIT_LOWERED 4000
#define WAIT_RAISED 6000

static const WCHAR s_szClassName[] = L"TaskbarFullscreenTest";

static HWND GetTray(void)
{
    return FindWindowW(L"Shell_TrayWnd", NULL);
}

static BOOL IsTrayTopmost(void)
{
    HWND hwndTray = GetTray();
    return hwndTray && (GetWindowLongW(hwndTray, GWL_EXSTYLE) & WS_EX_TOPMOST);
}

static void Pump(DWORD dwMilliseconds)
{
    MSG msg;
    DWORD dwStart = GetTickCount();

    while (GetTickCount() - dwStart < dwMilliseconds)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
}

/* Wait until the taskbar topmost state is the wanted one */
static BOOL WaitForTray(BOOL bTopmost, DWORD dwMilliseconds)
{
    DWORD dwStart = GetTickCount();

    do
    {
        if (IsTrayTopmost() == bTopmost)
            return TRUE;
        Pump(100);
    } while (GetTickCount() - dwStart < dwMilliseconds);

    return IsTrayTopmost() == bTopmost;
}

static HWND CreateTestWindow(DWORD dwStyle, INT x, INT y, INT cx, INT cy)
{
    HWND hwnd = CreateWindowW(s_szClassName, L"TaskbarFullscreen", dwStyle, x, y, cx, cy,
                              NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd)
        return NULL;

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    return hwnd;
}

/* Show a window over the whole screen and wait until the taskbar gets out of its way */
static HWND CreateFullscreenWindow(INT cxScreen, INT cyScreen)
{
    HWND hwnd = CreateTestWindow(WS_POPUP, 0, 0, cxScreen, cyScreen);
    if (!hwnd)
        return NULL;

    if (GetForegroundWindow() != hwnd || !WaitForTray(FALSE, WAIT_LOWERED))
    {
        skip("The full-screen window did not take the taskbar's place\n");
        DestroyWindow(hwnd);
        return NULL;
    }
    return hwnd;
}

static void CheckTrayBack(const char *pszStep, const RECT *prcWork)
{
    RECT rcWork;

    ok(WaitForTray(TRUE, WAIT_RAISED), "%s: the taskbar is not topmost\n", pszStep);

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    ok(EqualRect(&rcWork, prcWork), "%s: work area (%ld,%ld,%ld,%ld), expected (%ld,%ld,%ld,%ld)\n", pszStep,
       rcWork.left, rcWork.top, rcWork.right, rcWork.bottom,
       prcWork->left, prcWork->top, prcWork->right, prcWork->bottom);
}

START_TEST(TaskbarFullscreen)
{
    WNDCLASSW wc;
    RECT rcWork, rcTray, rcWindow;
    HWND hwndTray, hwnd, hwndOther;
    INT cxScreen = GetSystemMetrics(SM_CXSCREEN), cyScreen = GetSystemMetrics(SM_CYSCREEN);

    hwndTray = GetTray();
    if (!hwndTray || !IsWindowVisible(hwndTray))
    {
        skip("Taskbar not found\n");
        return;
    }

    if (!IsTrayTopmost())
    {
        skip("The taskbar is not always on top\n");
        return;
    }

    if (GetSystemMetrics(SM_CMONITORS) != 1)
    {
        skip("Multi-monitor not supported yet\n");
        return;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = s_szClassName;
    if (!RegisterClassW(&wc))
    {
        skip("RegisterClassW failed\n");
        return;
    }

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    GetWindowRect(hwndTray, &rcTray);

    /* The full-screen window goes away */
    hwnd = CreateFullscreenWindow(cxScreen, cyScreen);
    if (hwnd)
    {
        DestroyWindow(hwnd);
        CheckTrayBack("destroyed", &rcWork);
    }

    /* The full-screen window is minimized */
    hwnd = CreateFullscreenWindow(cxScreen, cyScreen);
    if (hwnd)
    {
        ShowWindow(hwnd, SW_MINIMIZE);
        CheckTrayBack("minimized", &rcWork);
        DestroyWindow(hwnd);
    }

    /* The window gets smaller and another window is activated, as when a browser leaves its full-screen mode */
    hwnd = CreateFullscreenWindow(cxScreen, cyScreen);
    if (hwnd)
    {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 100, 80, 640, 420, SWP_NOACTIVATE);
        hwndOther = CreateTestWindow(WS_OVERLAPPEDWINDOW, 300, 200, 500, 300);
        CheckTrayBack("left full screen", &rcWork);

        /* A maximized window stays above the work area, not under the taskbar */
        if (hwndOther)
        {
            ShowWindow(hwndOther, SW_MAXIMIZE);
            Pump(500);
            GetWindowRect(hwndOther, &rcWindow);
            ok(rcWindow.bottom <= rcWork.bottom + 8, "maximized window reaches %ld, the work area ends at %ld\n",
               rcWindow.bottom, rcWork.bottom);
            ok(IsTrayTopmost(), "the taskbar is not topmost under the maximized window\n");
            DestroyWindow(hwndOther);
        }
        DestroyWindow(hwnd);
    }

    GetWindowRect(hwndTray, &rcWindow);
    ok(EqualRect(&rcWindow, &rcTray), "the taskbar moved to (%ld,%ld,%ld,%ld)\n",
       rcWindow.left, rcWindow.top, rcWindow.right, rcWindow.bottom);

    UnregisterClassW(s_szClassName, GetModuleHandleW(NULL));
}
