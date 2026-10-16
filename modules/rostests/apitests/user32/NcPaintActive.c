/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Of the top-level windows of a thread that has the foreground, only the active one has the
 *              active frame: the default WM_NCPAINT draws the sizing border of the others as inactive.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define ACTIVE_COLOR   RGB(255, 0, 0)
#define INACTIVE_COLOR RGB(0, 0, 255)
#define FOUND_ACTIVE   1
#define FOUND_INACTIVE 2

static void
Pump(DWORD Ms)
{
    MSG Msg;
    DWORD Start = GetTickCount();

    while (GetTickCount() - Start < Ms)
    {
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
        Sleep(10);
    }
}

static BOOL
IsColor(COLORREF c, COLORREF Expected)
{
    return c != CLR_INVALID && abs((int)GetRValue(c) - (int)GetRValue(Expected)) <= 8 &&
           abs((int)GetGValue(c) - (int)GetGValue(Expected)) <= 8 && abs((int)GetBValue(c) - (int)GetBValue(Expected)) <= 8;
}

/* Which of the two border colours the left border of the window shows, at half its height */
static UINT
FindBorderColors(HWND hWnd, HDC hScreen)
{
    RECT Rect;
    UINT Found = 0;
    int x, y;

    GetWindowRect(hWnd, &Rect);
    y = (Rect.top + Rect.bottom) / 2;
    for (x = Rect.left; x < Rect.left + 12; x++)
    {
        COLORREF c = GetPixel(hScreen, x, y);

        if (IsColor(c, ACTIVE_COLOR))
            Found |= FOUND_ACTIVE;
        if (IsColor(c, INACTIVE_COLOR))
            Found |= FOUND_INACTIVE;
    }
    return Found;
}

static void
CheckFrames(HWND hActive, HWND hOther, HDC hScreen, const char *What)
{
    UINT Found;

    SendMessageW(hOther, WM_NCPAINT, 1, 0);
    SendMessageW(hActive, WM_NCPAINT, 1, 0);

    Found = FindBorderColors(hActive, hScreen);
    ok(Found == FOUND_ACTIVE, "%s: the active window shows border colours %u, expected %u\n", What, Found, FOUND_ACTIVE);
    Found = FindBorderColors(hOther, hScreen);
    ok(Found == FOUND_INACTIVE, "%s: the inactive window shows border colours %u, expected %u\n", What, Found, FOUND_INACTIVE);
}

START_TEST(NcPaintActive)
{
    static const INT Elements[] = { COLOR_ACTIVEBORDER, COLOR_INACTIVEBORDER };
    COLORREF Saved[2], Colors[2] = { ACTIVE_COLOR, INACTIVE_COLOR };
    WNDCLASSW Class = { 0 };
    HWND hFirst, hSecond;
    HDC hScreen;

    Class.lpfnWndProc = DefWindowProcW;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"NcPaintActive";
    Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&Class);

    hFirst = CreateWindowExW(WS_EX_TOPMOST, L"NcPaintActive", L"first", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 300, 200,
                             NULL, NULL, Class.hInstance, NULL);
    ok(hFirst != NULL, "CreateWindowEx failed: %lu\n", GetLastError());
    if (!hFirst)
        return;
    hSecond = CreateWindowExW(WS_EX_TOPMOST, L"NcPaintActive", L"second", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 400, 60, 300, 200,
                              NULL, NULL, Class.hInstance, NULL);
    ok(hSecond != NULL, "CreateWindowEx failed: %lu\n", GetLastError());
    if (!hSecond)
    {
        DestroyWindow(hFirst);
        return;
    }

    SetForegroundWindow(hFirst);
    SetActiveWindow(hFirst);
    Pump(200);
    if (GetForegroundWindow() != hFirst || GetActiveWindow() != hFirst)
    {
        skip("The window is not in the foreground (%p)\n", GetForegroundWindow());
        goto Done;
    }

    Saved[0] = GetSysColor(COLOR_ACTIVEBORDER);
    Saved[1] = GetSysColor(COLOR_INACTIVEBORDER);
    SetSysColors(2, Elements, Colors);
    Pump(200);

    hScreen = GetDC(NULL);
    if (!hScreen || FindBorderColors(hFirst, hScreen) == 0)
    {
        skip("The screen does not show the border colours\n");
    }
    else
    {
        CheckFrames(hFirst, hSecond, hScreen, "first window active");

        SetActiveWindow(hSecond);
        Pump(200);
        ok(GetActiveWindow() == hSecond, "The second window is not active\n");
        CheckFrames(hSecond, hFirst, hScreen, "second window active");
    }
    if (hScreen)
        ReleaseDC(NULL, hScreen);

    SetSysColors(2, Elements, Saved);
    Pump(100);

Done:
    DestroyWindow(hSecond);
    DestroyWindow(hFirst);
}
