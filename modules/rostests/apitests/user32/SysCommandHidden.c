/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     WM_SYSCOMMAND SC_MAXIMIZE, SC_RESTORE and SC_MINIMIZE on hidden and visible windows
 *
 * Verified on Windows 10 22H2 (interactive desktop and a service window station):
 * - SC_MAXIMIZE of a window that is already maximized and SC_MINIMIZE of one that is already
 *   minimized do nothing, even if the window is hidden: no message, the window stays hidden and
 *   inactive. Chrome creates its frame with WS_MAXIMIZE and sends it SC_MAXIMIZE while it is
 *   still hidden, and must not be activated before the browser window is fully constructed.
 * - Otherwise SC_MAXIMIZE and SC_RESTORE show and activate a hidden window (WM_SHOWWINDOW only
 *   for SC_RESTORE of a window that is neither minimized nor maximized).
 * - SC_MINIMIZE of a hidden window makes it visible and minimized without WM_SHOWWINDOW or focus;
 *   with a taskbar it is not activated either.
 * - SC_MAXIMIZE of a visible maximized window and SC_RESTORE of a visible normal one do nothing.
 */

#include "precomp.h"

static int g_Activate, g_ActivateMinimized, g_NcActivate, g_ShowWindow, g_SetFocus, g_PosChanging, g_SizeType;

static LRESULT CALLBACK
SysCmdProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE)
            {
                g_Activate++;
                if (HIWORD(wParam))
                    g_ActivateMinimized++;
            }
            break;
        case WM_NCACTIVATE:
            /* wParam is FALSE when the process is not in the foreground */
            g_NcActivate++;
            break;
        case WM_SHOWWINDOW:
            g_ShowWindow++;
            break;
        case WM_SETFOCUS:
            g_SetFocus++;
            break;
        case WM_WINDOWPOSCHANGING:
            g_PosChanging++;
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
    g_Activate = g_ActivateMinimized = g_NcActivate = g_ShowWindow = g_SetFocus = g_PosChanging = 0;
    g_SizeType = -1;
}

static void
FlushMessages(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

/* IsWindowVisible is FALSE on the invisible desktop of a service window station */
static BOOL
HasVisibleStyle(HWND hwnd)
{
    return (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
}

static HWND g_hwndHelper;
static RECT g_rcNormal, g_rcWork;

#define PREP_NONE       0
#define PREP_SHOWNMAX   1 /* shown maximized, then hidden */
#define PREP_VISIBLEMAX 2 /* visible maximized, not active */
#define PREP_VISIBLE    3 /* visible normal, not active */

static HWND
CreateTestWindow(DWORD dwExtraStyle, int Prep)
{
    HWND hwnd;

    SetActiveWindow(g_hwndHelper);
    FlushMessages();
    hwnd = CreateWindowExW(WS_EX_WINDOWEDGE, L"SysCommandHiddenClass", L"SysCommandHidden",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | dwExtraStyle,
                           50, 60, 300, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");
    if (!hwnd)
        return NULL;
    switch (Prep)
    {
        case PREP_SHOWNMAX:
            ShowWindow(hwnd, SW_SHOWMAXIMIZED);
            ShowWindow(hwnd, SW_HIDE);
            break;
        case PREP_VISIBLEMAX:
            ShowWindow(hwnd, SW_SHOWMAXIMIZED);
            break;
        case PREP_VISIBLE:
            ShowWindow(hwnd, SW_SHOWNORMAL);
            break;
    }
    SetActiveWindow(g_hwndHelper);
    FlushMessages();
    ok(GetActiveWindow() != hwnd, "test window is active before the command\n");
    ResetCounters();
    return hwnd;
}

static void
SendSysCommand(HWND hwnd, WPARAM cmd)
{
    SendMessageW(hwnd, WM_SYSCOMMAND, cmd, 0);
}

#define ok_nothing(hwnd, visible) \
    do { \
        ok_int(g_PosChanging, 0); \
        ok_int(g_ShowWindow, 0); \
        ok_int(g_Activate, 0); \
        ok_int(g_NcActivate, 0); \
        ok_int(g_SetFocus, 0); \
        ok_int(g_SizeType, -1); \
        ok_int(HasVisibleStyle(hwnd), (visible)); \
        ok(GetActiveWindow() != (hwnd), "window became active\n"); \
    } while (0)

#define ok_shown_active(hwnd) \
    do { \
        ok(HasVisibleStyle(hwnd), "window is not visible\n"); \
        ok(g_Activate >= 1, "no WM_ACTIVATE\n"); \
        ok_int(g_ActivateMinimized, 0); \
        ok(g_NcActivate >= 1, "no WM_NCACTIVATE\n"); \
        ok(g_SetFocus >= 1, "no WM_SETFOCUS\n"); \
        ok(GetActiveWindow() == (hwnd), "window is not the active window\n"); \
    } while (0)

static void
TestMaximize(void)
{
    HWND hwnd;
    RECT rc;
    WINDOWPLACEMENT wp;

    /* Chrome: created hidden with WS_MAXIMIZE, then SC_MAXIMIZE: nothing happens */
    hwnd = CreateTestWindow(WS_MAXIMIZE, PREP_NONE);
    if (!hwnd) return;
    ok(IsZoomed(hwnd), "window created with WS_MAXIMIZE is not maximized\n");
    SendSysCommand(hwnd, SC_MAXIMIZE);
    ok_nothing(hwnd, FALSE);
    ok(IsZoomed(hwnd), "window is not maximized\n");
    ok(!(GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE), "WS_VISIBLE set\n");
    ok(GetActiveWindow() == g_hwndHelper, "active window changed\n");
    /* ... and showing it afterwards shows and activates it maximized */
    ResetCounters();
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    ok_shown_active(hwnd);
    ok(IsZoomed(hwnd), "window is not maximized after SW_SHOWMAXIMIZED\n");
    DestroyWindow(hwnd);

    /* shown maximized, then hidden: SC_MAXIMIZE does nothing either */
    hwnd = CreateTestWindow(0, PREP_SHOWNMAX);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_MAXIMIZE);
    ok_nothing(hwnd, FALSE);
    ok(IsZoomed(hwnd), "window is not maximized\n");
    DestroyWindow(hwnd);

    /* visible maximized window that is not active: nothing */
    hwnd = CreateTestWindow(0, PREP_VISIBLEMAX);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_MAXIMIZE);
    ok_nothing(hwnd, TRUE);
    ok(IsZoomed(hwnd), "window is not maximized\n");
    DestroyWindow(hwnd);

    /* hidden normal window: SC_MAXIMIZE maximizes, shows and activates it, without WM_SHOWWINDOW */
    hwnd = CreateTestWindow(0, PREP_NONE);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_MAXIMIZE);
    ok_shown_active(hwnd);
    ok_int(g_ShowWindow, 0);
    ok_int(g_SizeType, SIZE_MAXIMIZED);
    ok(IsZoomed(hwnd), "window is not maximized\n");
    ok(!IsIconic(hwnd), "window is minimized\n");
    GetWindowRect(hwnd, &rc);
    ok(rc.right - rc.left >= g_rcWork.right - g_rcWork.left,
       "maximized width %ld is smaller than the work area width %ld\n",
       rc.right - rc.left, g_rcWork.right - g_rcWork.left);
    ok(rc.bottom - rc.top >= g_rcWork.bottom - g_rcWork.top,
       "maximized height %ld is smaller than the work area height %ld\n",
       rc.bottom - rc.top, g_rcWork.bottom - g_rcWork.top);
    wp.length = sizeof(wp);
    ok(GetWindowPlacement(hwnd, &wp), "GetWindowPlacement failed\n");
    ok_int(wp.showCmd, SW_SHOWMAXIMIZED);
    /* a second SC_MAXIMIZE changes nothing */
    SetActiveWindow(g_hwndHelper);
    FlushMessages();
    ResetCounters();
    SendSysCommand(hwnd, SC_MAXIMIZE);
    ok_nothing(hwnd, TRUE);
    DestroyWindow(hwnd);
}

static void
TestRestore(void)
{
    HWND hwnd;
    RECT rc;

    /* hidden normal window: shown (with WM_SHOWWINDOW) and activated */
    hwnd = CreateTestWindow(0, PREP_NONE);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_RESTORE);
    ok_shown_active(hwnd);
    ok_int(g_ShowWindow, 1);
    ok(!IsZoomed(hwnd) && !IsIconic(hwnd), "window is not in the normal state\n");
    GetWindowRect(hwnd, &rc);
    ok(EqualRect(&rc, &g_rcNormal), "rect (%ld,%ld,%ld,%ld) differs from the normal rect (%ld,%ld,%ld,%ld)\n",
       rc.left, rc.top, rc.right, rc.bottom, g_rcNormal.left, g_rcNormal.top, g_rcNormal.right, g_rcNormal.bottom);
    DestroyWindow(hwnd);

    /* hidden maximized window: restored to its normal position, shown and activated */
    hwnd = CreateTestWindow(WS_MAXIMIZE, PREP_NONE);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_RESTORE);
    ok_shown_active(hwnd);
    ok_int(g_ShowWindow, 0);
    ok_int(g_SizeType, SIZE_RESTORED);
    ok(!IsZoomed(hwnd), "window is still maximized\n");
    GetWindowRect(hwnd, &rc);
    ok(EqualRect(&rc, &g_rcNormal), "rect (%ld,%ld,%ld,%ld) differs from the normal rect (%ld,%ld,%ld,%ld)\n",
       rc.left, rc.top, rc.right, rc.bottom, g_rcNormal.left, g_rcNormal.top, g_rcNormal.right, g_rcNormal.bottom);
    DestroyWindow(hwnd);

    /* hidden minimized window: restored, shown and activated */
    hwnd = CreateTestWindow(WS_MINIMIZE, PREP_NONE);
    if (!hwnd) return;
    SetActiveWindow(g_hwndHelper);
    FlushMessages();
    ResetCounters();
    SendSysCommand(hwnd, SC_RESTORE);
    ok_shown_active(hwnd);
    ok_int(g_ShowWindow, 0);
    ok_int(g_SizeType, SIZE_RESTORED);
    ok(!IsIconic(hwnd), "window is still minimized\n");
    DestroyWindow(hwnd);

    /* visible normal window that is not active: nothing */
    hwnd = CreateTestWindow(0, PREP_VISIBLE);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_RESTORE);
    ok_nothing(hwnd, TRUE);
    DestroyWindow(hwnd);
}

static void
TestMinimize(void)
{
    HWND hwnd;
    BOOL bTaskbar = (FindWindowW(L"Shell_TrayWnd", NULL) != NULL);

    /* hidden normal window: visible and minimized, no WM_SHOWWINDOW, no focus */
    hwnd = CreateTestWindow(0, PREP_NONE);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_MINIMIZE);
    ok(HasVisibleStyle(hwnd), "window is not visible\n");
    ok(IsIconic(hwnd), "window is not minimized\n");
    ok_int(g_ShowWindow, 0);
    ok_int(g_SetFocus, 0);
    ok_int(g_SizeType, SIZE_MINIMIZED);
    if (bTaskbar)
    {
        /* without a taskbar (service window station) Windows activates it as a minimized window */
        ok_int(g_Activate, 0);
        ok(GetActiveWindow() != hwnd, "window became active\n");
    }
    DestroyWindow(hwnd);

    /* hidden maximized window: the same */
    hwnd = CreateTestWindow(WS_MAXIMIZE, PREP_NONE);
    if (!hwnd) return;
    SendSysCommand(hwnd, SC_MINIMIZE);
    ok(HasVisibleStyle(hwnd), "window is not visible\n");
    ok(IsIconic(hwnd), "window is not minimized\n");
    ok_int(g_ShowWindow, 0);
    ok_int(g_SetFocus, 0);
    ok_int(g_SizeType, SIZE_MINIMIZED);
    if (bTaskbar)
        ok_int(g_Activate, 0);
    DestroyWindow(hwnd);

    /* hidden minimized window: nothing, it stays hidden */
    hwnd = CreateTestWindow(WS_MINIMIZE, PREP_NONE);
    if (!hwnd) return;
    SetActiveWindow(g_hwndHelper);
    FlushMessages();
    ResetCounters();
    SendSysCommand(hwnd, SC_MINIMIZE);
    ok_nothing(hwnd, FALSE);
    ok(IsIconic(hwnd), "window is not minimized\n");
    DestroyWindow(hwnd);
}

START_TEST(SysCommandHidden)
{
    WNDCLASSW wc = {0};
    HWND hwnd;

    wc.lpfnWndProc = SysCmdProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SysCommandHiddenClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc))
    {
        skip("RegisterClass failed\n");
        return;
    }

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_rcWork, 0);
    g_hwndHelper = CreateWindowExW(0, L"STATIC", L"SysCommandHidden helper", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                   0, 0, 200, 100, NULL, NULL, wc.hInstance, NULL);
    ok(g_hwndHelper != NULL, "helper window creation failed\n");
    SetForegroundWindow(g_hwndHelper);
    FlushMessages();

    /* the normal rect of a test window */
    hwnd = CreateWindowExW(WS_EX_WINDOWEDGE, wc.lpszClassName, L"SysCommandHidden",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                           50, 60, 300, 200, NULL, NULL, wc.hInstance, NULL);
    GetWindowRect(hwnd, &g_rcNormal);
    DestroyWindow(hwnd);

    TestMaximize();
    TestRestore();
    TestMinimize();

    DestroyWindow(g_hwndHelper);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
