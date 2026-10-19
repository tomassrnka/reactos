/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the right button in the caption (WM_NCRBUTTONDOWN in DefWindowProc)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static LRESULT s_HitBefore, s_HitAfter;
static BOOL s_bDown, s_bInDown;
static INT s_nHitInDown, s_nContextMenu, s_nNCRUp, s_nRUp, s_nSysCommand;
static WPARAM s_wpContextMenu;
static LPARAM s_lpContextMenu;

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LRESULT ret;

    switch (uMsg)
    {
        case WM_NCHITTEST:
            if (s_bInDown)
                ++s_nHitInDown;
            if (!s_bDown && s_HitBefore != -1)
                return s_HitBefore;
            if (s_bDown && s_HitAfter != -1)
                return s_HitAfter;
            break;

        case WM_NCRBUTTONDOWN:
            s_bDown = TRUE;
            s_bInDown = TRUE;
            ret = DefWindowProcW(hwnd, uMsg, wParam, lParam);
            s_bInDown = FALSE;
            return ret;

        case WM_NCRBUTTONUP:
            ++s_nNCRUp;
            break;

        case WM_RBUTTONUP:
            ++s_nRUp;
            break;

        case WM_CONTEXTMENU:
            ++s_nContextMenu;
            s_wpContextMenu = wParam;
            s_lpContextMenu = lParam;
            return 0;

        case WM_SYSCOMMAND:
            ++s_nSysCommand;
            break;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void
PumpMessages(DWORD dwMilliseconds)
{
    DWORD dwStart = GetTickCount();
    MSG msg;

    while (GetTickCount() - dwStart < dwMilliseconds)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

static void
RightClick(INT x, INT y)
{
    INPUT input[2];
    /* With swapped buttons the logical right button is the physical left one */
    BOOL bSwapped = GetSystemMetrics(SM_SWAPBUTTON);

    ZeroMemory(input, sizeof(input));
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = bSwapped ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = bSwapped ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;

    SetCursorPos(x, y);
    PumpMessages(100);
    SendInput(2, input, sizeof(INPUT));
    PumpMessages(300);
}

typedef struct tagNCRB_CASE
{
    const char *name;
    DWORD dwStyle;
    LRESULT HitBefore;  /* -1: DefWindowProc */
    LRESULT HitAfter;   /* -1: DefWindowProc */
    BOOL bInCaption;    /* press in the frame's caption, else in the middle */
    INT nContextMenu;   /* expected WM_CONTEXTMENU count */
    INT nNCRUp;         /* expected WM_NCRBUTTONUP count */
    INT nRUp;           /* expected WM_RBUTTONUP count */
} NCRB_CASE;

/*
 * Windows 7 and 10 track the press (and eat the release) only when the frame's
 * own hit test of the press point is the caption or the system menu, with or
 * without WS_SYSMENU; WM_CONTEXTMENU then follows when the frame's own hit test
 * of the release point is still the caption. The window is not asked with
 * WM_NCHITTEST during the press. A window that answers HTCAPTION elsewhere gets
 * the release as an ordinary message.
 */
static const NCRB_CASE s_Cases[] =
{
    { "popup, HTCAPTION", WS_POPUP | WS_BORDER, HTCAPTION, HTCAPTION, FALSE, 0, 1, 0 },
    { "popup+sysmenu, HTCAPTION", WS_POPUP | WS_BORDER | WS_SYSMENU, HTCAPTION, HTCAPTION, FALSE, 0, 1, 0 },
    { "popup+sysmenu, HTSYSMENU", WS_POPUP | WS_BORDER | WS_SYSMENU, HTSYSMENU, HTSYSMENU, FALSE, 0, 1, 0 },
    { "popup+sysmenu, HTCAPTION then HTCLIENT", WS_POPUP | WS_BORDER | WS_SYSMENU, HTCAPTION, HTCLIENT, FALSE, 1, 0, 1 },
    { "overlapped, caption", WS_OVERLAPPEDWINDOW, -1, -1, TRUE, 1, 0, 0 },
    { "overlapped, caption, then HTCLIENT", WS_OVERLAPPEDWINDOW, -1, HTCLIENT, TRUE, 1, 0, 0 },
    { "overlapped, caption, then HTNOWHERE", WS_OVERLAPPEDWINDOW, -1, HTNOWHERE, TRUE, 1, 0, 0 },
    { "overlapped, client, HTCAPTION", WS_OVERLAPPEDWINDOW, HTCAPTION, HTCAPTION, FALSE, 0, 1, 0 },
    { "caption without sysmenu", WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME, -1, -1, TRUE, 1, 0, 0 },
    { "popup+caption+sysmenu", WS_POPUP | WS_CAPTION | WS_SYSMENU, -1, -1, TRUE, 1, 0, 0 },
};

static void
RunCase(const NCRB_CASE *pCase)
{
    WNDCLASSW wc;
    HWND hwnd;
    RECT rc;
    INT x, y;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NCRButtonDownTest";
    RegisterClassW(&wc);

    s_HitBefore = pCase->HitBefore;
    s_HitAfter = pCase->HitAfter;
    s_bDown = s_bInDown = FALSE;
    s_nHitInDown = s_nContextMenu = s_nNCRUp = s_nRUp = s_nSysCommand = 0;
    s_wpContextMenu = 0;
    s_lpContextMenu = 0;

    hwnd = CreateWindowExW(WS_EX_TOPMOST, L"NCRButtonDownTest", L"NCRButtonDown",
                           pCase->dwStyle | WS_VISIBLE, 100, 100, 240, 160,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "%s: CreateWindowExW failed (%lu)\n", pCase->name, GetLastError());
    if (!hwnd)
        return;

    SetForegroundWindow(hwnd);
    PumpMessages(200);

    GetWindowRect(hwnd, &rc);
    x = (rc.left + rc.right) / 2;
    if (pCase->bInCaption)
        y = rc.top + GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYCAPTION) / 2;
    else
        y = (rc.top + rc.bottom) / 2;

    RightClick(x, y);

    trace("%s: down %d hit-in-down %d contextmenu %d (wp %s, lp %d,%d) ncrbuttonup %d rbuttonup %d syscommand %d\n",
          pCase->name, s_bDown, s_nHitInDown, s_nContextMenu,
          (s_wpContextMenu == (WPARAM)hwnd) ? "hwnd" : "other",
          (SHORT)LOWORD(s_lpContextMenu), (SHORT)HIWORD(s_lpContextMenu),
          s_nNCRUp, s_nRUp, s_nSysCommand);

    ok(s_bDown, "%s: no WM_NCRBUTTONDOWN\n", pCase->name);
    ok(s_nHitInDown == 0, "%s: %d WM_NCHITTEST during the press\n", pCase->name, s_nHitInDown);
    ok(s_nContextMenu == pCase->nContextMenu, "%s: %d WM_CONTEXTMENU, expected %d\n",
       pCase->name, s_nContextMenu, pCase->nContextMenu);
    ok(s_nNCRUp == pCase->nNCRUp, "%s: %d WM_NCRBUTTONUP, expected %d\n", pCase->name, s_nNCRUp, pCase->nNCRUp);
    ok(s_nRUp == pCase->nRUp, "%s: %d WM_RBUTTONUP, expected %d\n", pCase->name, s_nRUp, pCase->nRUp);
    if (s_nContextMenu)
    {
        ok(s_wpContextMenu == (WPARAM)hwnd, "%s: WM_CONTEXTMENU wParam %p\n", pCase->name, (PVOID)s_wpContextMenu);
        ok((SHORT)LOWORD(s_lpContextMenu) == x && (SHORT)HIWORD(s_lpContextMenu) == y,
           "%s: WM_CONTEXTMENU at %d,%d, expected %d,%d\n", pCase->name,
           (SHORT)LOWORD(s_lpContextMenu), (SHORT)HIWORD(s_lpContextMenu), x, y);
    }

    DestroyWindow(hwnd);
    PumpMessages(50);
}

START_TEST(NCRButtonDown)
{
    UINT i;

    for (i = 0; i < _countof(s_Cases); ++i)
        RunCase(&s_Cases[i]);
}
