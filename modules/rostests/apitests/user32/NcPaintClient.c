/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     A program that draws its own title bar makes the client area cover the caption
 *              (WM_NCCALCSIZE). Drawing the caption bar of the window must not paint over what the program
 *              shows there: the caption is not part of the non-client area any more.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define CLIENT_GREEN RGB(0, 200, 0)
#define WM_NCUAHDRAWCAPTION_ 0x00AE
#define SIDE 4

static LRESULT CALLBACK
WndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg)
    {
        case WM_NCCALCSIZE:
        {
            /* The client area starts at the top of the window, over the caption; the sides and the bottom keep a border */
            RECT *Rect = wParam ? &((NCCALCSIZE_PARAMS *)lParam)->rgrc[0] : (RECT *)lParam;

            Rect->left += SIDE;
            Rect->right -= SIDE;
            Rect->bottom -= SIDE;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT Ps;
            HDC hDC = BeginPaint(hWnd, &Ps);
            HBRUSH Brush = CreateSolidBrush(CLIENT_GREEN);

            FillRect(hDC, &Ps.rcPaint, Brush);
            DeleteObject(Brush);
            EndPaint(hWnd, &Ps);
            return 0;
        }
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

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
IsGreen(COLORREF c)
{
    return c != CLR_INVALID && abs((int)GetRValue(c) - 0) <= 8 && abs((int)GetGValue(c) - 200) <= 8 && abs((int)GetBValue(c) - 0) <= 8;
}

/* Where the caption would be, inside the client area: the middle of the caption, near the icon, near the buttons, below it */
static void
CheckCaption(HWND hWnd, HDC hScreen, const char *What)
{
    RECT Rect;
    COLORREF c;
    int i;
    POINT Points[4];

    GetWindowRect(hWnd, &Rect);
    Points[0].x = (Rect.left + Rect.right) / 2; Points[0].y = Rect.top + 10;
    Points[1].x = Rect.left + 24; Points[1].y = Rect.top + 10;
    Points[2].x = Rect.right - 20; Points[2].y = Rect.top + 10;
    Points[3].x = (Rect.left + Rect.right) / 2; Points[3].y = Rect.top + 20;
    for (i = 0; i < 4; i++)
    {
        c = GetPixel(hScreen, Points[i].x, Points[i].y);
        ok(IsGreen(c), "%s: point %d (%ld,%ld) is %06lx, not the client area\n", What, i, Points[i].x, Points[i].y, (unsigned long)c);
    }
}

START_TEST(NcPaintClient)
{
    WNDCLASSW Class = { 0 };
    HWND hWnd;
    HDC hScreen;
    RECT Rect, Client;
    POINT Origin = { 0, 0 };
    COLORREF c;

    Class.lpfnWndProc = WndProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"NcPaintClient";
    RegisterClassW(&Class);

    hWnd = CreateWindowExW(WS_EX_TOPMOST, L"NcPaintClient", L"caption", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300,
                           NULL, NULL, Class.hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowEx failed: %lu\n", GetLastError());
    if (!hWnd)
        return;
    UpdateWindow(hWnd);
    Pump(300);

    GetWindowRect(hWnd, &Rect);
    GetClientRect(hWnd, &Client);
    ClientToScreen(hWnd, &Origin);
    ok(Origin.x - Rect.left == SIDE && Origin.y == Rect.top, "client origin (%ld,%ld) in the window at (%ld,%ld)\n",
       Origin.x, Origin.y, Rect.left, Rect.top);

    hScreen = GetDC(NULL);
    c = hScreen ? GetPixel(hScreen, (Rect.left + Rect.right) / 2, Rect.top + 10) : CLR_INVALID;
    if (!IsGreen(c))
    {
        skip("The screen does not show the window (pixel %06lx)\n", (unsigned long)c);
        goto Done;
    }
    CheckCaption(hWnd, hScreen, "painted");

    SendMessageW(hWnd, WM_NCPAINT, 1, 0);
    CheckCaption(hWnd, hScreen, "WM_NCPAINT");

    SendMessageW(hWnd, WM_NCACTIVATE, FALSE, 0);
    CheckCaption(hWnd, hScreen, "WM_NCACTIVATE inactive");
    SendMessageW(hWnd, WM_NCACTIVATE, TRUE, 0);
    CheckCaption(hWnd, hScreen, "WM_NCACTIVATE active");

    SetWindowTextW(hWnd, L"another caption");
    Pump(100);
    CheckCaption(hWnd, hScreen, "WM_SETTEXT");

    SendMessageW(hWnd, WM_NCUAHDRAWCAPTION_, 0, 0);
    CheckCaption(hWnd, hScreen, "WM_NCUAHDRAWCAPTION");

    RedrawWindow(hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    Pump(100);
    CheckCaption(hWnd, hScreen, "RedrawWindow");

Done:
    if (hScreen)
        ReleaseDC(NULL, hScreen);
    DestroyWindow(hWnd);
}
