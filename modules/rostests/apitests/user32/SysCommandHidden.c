/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     WM_SYSCOMMAND SC_MAXIMIZE on a window that is not visible
 *
 * SC_MAXIMIZE changes the state of a hidden window (style, size, WM_SIZE) but
 * neither shows nor activates it. Chrome maximizes a window that was never shown this way
 * and must not receive an activation before it has finished constructing the window.
 */

#include "precomp.h"

static int g_Activate, g_NcActivate, g_ShowWindow, g_SetFocus, g_SizeType = -1;

static LRESULT CALLBACK
SysCmdProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_ACTIVATE:
            if (wParam != WA_INACTIVE) g_Activate++;
            break;
        case WM_NCACTIVATE:
            if (wParam) g_NcActivate++;
            break;
        case WM_SHOWWINDOW:
            g_ShowWindow++;
            break;
        case WM_SETFOCUS:
            g_SetFocus++;
            break;
        case WM_SIZE:
            g_SizeType = (int)wParam;
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void
ResetCounters(void)
{
    g_Activate = g_NcActivate = g_ShowWindow = g_SetFocus = 0;
    g_SizeType = -1;
}

START_TEST(SysCommandHidden)
{
    WNDCLASSW wc = {0};
    HWND hwnd, hwndActive;
    RECT rc, rcWork, rcNormal;
    WINDOWPLACEMENT wp;

    wc.lpfnWndProc = SysCmdProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SysCommandHiddenClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc))
    {
        skip("RegisterClass failed\n");
        return;
    }

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"SysCommandHidden", WS_OVERLAPPEDWINDOW,
                           50, 60, 300, 200, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
    {
        skip("CreateWindowEx failed\n");
        return;
    }
    hwndActive = GetActiveWindow();
    GetWindowRect(hwnd, &rcNormal);
    ok(!IsWindowVisible(hwnd), "window is visible after creation\n");
    ResetCounters();

    /* SC_MAXIMIZE: state changes, window stays hidden and is not activated */
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    ok(IsZoomed(hwnd), "window is not maximized\n");
    ok(!IsIconic(hwnd), "window is minimized\n");
    ok(!IsWindowVisible(hwnd), "SC_MAXIMIZE made the hidden window visible\n");
    ok(!(GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE), "WS_VISIBLE set\n");
    ok_int(g_Activate, 0);
    ok_int(g_NcActivate, 0);
    ok_int(g_SetFocus, 0);
    ok_int(g_ShowWindow, 0);
    ok_int(g_SizeType, SIZE_MAXIMIZED);
    ok(GetActiveWindow() != hwnd, "hidden window became the active window\n");
    ok(GetActiveWindow() == hwndActive, "active window changed\n");
    ok(GetForegroundWindow() != hwnd, "hidden window became the foreground window\n");

    GetWindowRect(hwnd, &rc);
    ok(rc.right - rc.left >= rcWork.right - rcWork.left,
       "maximized width %ld is smaller than the work area width %ld\n",
       rc.right - rc.left, rcWork.right - rcWork.left);
    ok(rc.bottom - rc.top >= rcWork.bottom - rcWork.top,
       "maximized height %ld is smaller than the work area height %ld\n",
       rc.bottom - rc.top, rcWork.bottom - rcWork.top);

    wp.length = sizeof(wp);
    ok(GetWindowPlacement(hwnd, &wp), "GetWindowPlacement failed\n");
    ok_int(wp.showCmd, SW_SHOWMAXIMIZED);


    /* a second SC_MAXIMIZE changes nothing */
    ResetCounters();
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    ok(IsZoomed(hwnd), "window is not maximized\n");
    ok(!IsWindowVisible(hwnd), "window became visible\n");
    ok_int(g_Activate, 0);

    /* showing it afterwards displays it maximized */
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    ok(IsWindowVisible(hwnd), "window is not visible after SW_SHOWMAXIMIZED\n");
    ok(IsZoomed(hwnd), "window is not maximized after SW_SHOWMAXIMIZED\n");

    /* hidden maximized window: SC_RESTORE returns it to its normal position (the
       behaviour of SC_RESTORE and SC_MINIMIZE on a hidden window is not tested) */
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    ok(!IsZoomed(hwnd), "window is still maximized\n");
    GetWindowRect(hwnd, &rc);
    ok(EqualRect(&rc, &rcNormal), "restored rect (%ld,%ld,%ld,%ld) differs from the normal rect (%ld,%ld,%ld,%ld)\n",
       rc.left, rc.top, rc.right, rc.bottom, rcNormal.left, rcNormal.top, rcNormal.right, rcNormal.bottom);

    /* a hidden window can be maximized again, and ShowWindow still shows it */
    ShowWindow(hwnd, SW_HIDE);
    ResetCounters();
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    ok(IsZoomed(hwnd), "window is not maximized\n");
    ok(!IsWindowVisible(hwnd), "window became visible\n");
    ok_int(g_Activate, 0);
    ShowWindow(hwnd, SW_SHOW);
    ok(IsWindowVisible(hwnd), "window is not visible after SW_SHOW\n");

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
