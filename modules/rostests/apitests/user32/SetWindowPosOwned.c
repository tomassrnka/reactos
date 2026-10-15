/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests SetWindowPos with SWP_NOOWNERZORDER on a window that owns popups
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define FLAGS (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)

static BOOL IsTopmost(HWND hwnd)
{
    return (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

/* The position among the top-level windows, counted from the top */
static INT ZPos(HWND hwnd)
{
    HWND hwndNext;
    INT i = 0;

    for (hwndNext = GetTopWindow(NULL); hwndNext; hwndNext = GetWindow(hwndNext, GW_HWNDNEXT), i++)
    {
        if (hwndNext == hwnd)
            return i;
    }
    return -1;
}

static BOOL CreateOwnerAndPopups(BOOL bTopmost, HWND *phwndOwner, HWND *phwndPopup1, HWND *phwndPopup2)
{
    HINSTANCE hInst = GetModuleHandleW(NULL);

    *phwndOwner = CreateWindowExW(WS_EX_TOOLWINDOW | (bTopmost ? WS_EX_TOPMOST : 0), L"SetWindowPosOwnedTest",
                                  L"owner", WS_POPUP | WS_VISIBLE, 10, 10, 200, 60, NULL, NULL, hInst, NULL);
    *phwndPopup1 = CreateWindowExW(WS_EX_TOOLWINDOW, L"SetWindowPosOwnedTest", L"popup 1", WS_POPUP | WS_VISIBLE,
                                   20, 20, 100, 40, *phwndOwner, NULL, hInst, NULL);
    *phwndPopup2 = CreateWindowExW(WS_EX_TOOLWINDOW, L"SetWindowPosOwnedTest", L"popup 2", WS_POPUP | WS_VISIBLE,
                                   30, 30, 100, 40, *phwndOwner, NULL, hInst, NULL);
    return *phwndOwner && *phwndPopup1 && *phwndPopup2;
}

static void DestroyOwnerAndPopups(HWND hwndOwner, HWND hwndPopup1, HWND hwndPopup2)
{
    if (hwndPopup2)
        DestroyWindow(hwndPopup2);
    if (hwndPopup1)
        DestroyWindow(hwndPopup1);
    if (hwndOwner)
        DestroyWindow(hwndOwner);
}

START_TEST(SetWindowPosOwned)
{
    WNDCLASSW wc;
    HWND hwndOwner, hwndPopup1, hwndPopup2;
    HINSTANCE hInst = GetModuleHandleW(NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SetWindowPosOwnedTest";
    if (!RegisterClassW(&wc))
    {
        skip("RegisterClassW failed\n");
        return;
    }

    /* SWP_NOOWNERZORDER: only the owner changes, the popups keep their state */
    if (CreateOwnerAndPopups(FALSE, &hwndOwner, &hwndPopup1, &hwndPopup2))
    {
        ok(!IsTopmost(hwndOwner) && !IsTopmost(hwndPopup1) && !IsTopmost(hwndPopup2), "windows are topmost at the start\n");

        ok(SetWindowPos(hwndOwner, HWND_TOPMOST, 0, 0, 0, 0, FLAGS), "SetWindowPos failed\n");
        ok(IsTopmost(hwndOwner), "owner is not topmost after HWND_TOPMOST\n");
        ok(!IsTopmost(hwndPopup1) && !IsTopmost(hwndPopup2), "popups became topmost\n");

        ok(SetWindowPos(hwndOwner, HWND_NOTOPMOST, 0, 0, 0, 0, FLAGS), "SetWindowPos failed\n");
        ok(!IsTopmost(hwndOwner), "owner is still topmost after HWND_NOTOPMOST\n");
        ok(!IsTopmost(hwndPopup1) && !IsTopmost(hwndPopup2), "popups changed their state\n");
    }
    else
    {
        skip("CreateWindowExW failed\n");
    }
    DestroyOwnerAndPopups(hwndOwner, hwndPopup1, hwndPopup2);

    /* A topmost owner that goes to the bottom and comes back, as the taskbar does for a full-screen window */
    if (CreateOwnerAndPopups(TRUE, &hwndOwner, &hwndPopup1, &hwndPopup2))
    {
        ok(IsTopmost(hwndOwner) && IsTopmost(hwndPopup1) && IsTopmost(hwndPopup2), "windows are not topmost at the start\n");

        ok(SetWindowPos(hwndOwner, HWND_BOTTOM, 0, 0, 0, 0, FLAGS), "SetWindowPos failed\n");
        ok(!IsTopmost(hwndOwner), "owner is still topmost after HWND_BOTTOM\n");
        ok(IsTopmost(hwndPopup1) && IsTopmost(hwndPopup2), "popups lost their topmost state\n");
        ok(ZPos(hwndOwner) > ZPos(hwndPopup1) && ZPos(hwndOwner) > ZPos(hwndPopup2), "owner is not below the popups\n");

        ok(SetWindowPos(hwndOwner, HWND_TOPMOST, 0, 0, 0, 0, FLAGS), "SetWindowPos failed\n");
        ok(IsTopmost(hwndOwner), "owner did not get topmost back\n");
        ok(IsTopmost(hwndPopup1) && IsTopmost(hwndPopup2), "popups lost their topmost state\n");
        ok(ZPos(hwndOwner) < ZPos(hwndPopup1) && ZPos(hwndOwner) < ZPos(hwndPopup2), "owner is not above the popups\n");

        /* The owner can leave the topmost group while its popups stay in it */
        ok(SetWindowPos(hwndOwner, HWND_NOTOPMOST, 0, 0, 0, 0, FLAGS), "SetWindowPos failed\n");
        ok(!IsTopmost(hwndOwner), "owner is still topmost after HWND_NOTOPMOST\n");
        ok(IsTopmost(hwndPopup1) && IsTopmost(hwndPopup2), "popups lost their topmost state\n");
    }
    else
    {
        skip("CreateWindowExW failed\n");
    }
    DestroyOwnerAndPopups(hwndOwner, hwndPopup1, hwndPopup2);

    UnregisterClassW(wc.lpszClassName, hInst);
}
