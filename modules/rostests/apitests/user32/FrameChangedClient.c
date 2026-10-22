/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     A frame change in place (DrawMenuBar, SWP_FRAMECHANGED) must leave the painted client
 *              area painted and repaint the frame, wherever the window is on the screen. The region
 *              the non-client repaint erased was window-relative but used as screen coordinates, so
 *              border-wide strips of the client of a window away from the origin were erased
 *              without a WM_PAINT. The frame change of a child erases nothing of its
 *              parent.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static LONG Paints;

static LRESULT CALLBACK
WndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (Msg == WM_PAINT)
    {
        PAINTSTRUCT Ps;
        RECT Client;
        HDC hDC = BeginPaint(hWnd, &Ps);

        GetClientRect(hWnd, &Client);
        FillRect(hDC, &Client, GetStockObject(BLACK_BRUSH));
        EndPaint(hWnd, &Ps);
        ++Paints;
        return 0;
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

static void
Pump(void)
{
    MSG Msg;
    DWORD Start = GetTickCount();

    while (GetTickCount() - Start < 500)
    {
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
        Sleep(10);
    }
}

/* Read a screen rectangle back; returns the number of pixels that are not dark, or -1 */
static LONG
LightPixels(const RECT *Screen, LONG *Total)
{
    LONG Width = Screen->right - Screen->left, Height = Screen->bottom - Screen->top;
    BITMAPINFO Info = {{sizeof(BITMAPINFOHEADER), Width, -Height, 1, 32, BI_RGB}};
    DWORD *Bits = NULL;
    LONG i, Light = -1;
    HDC hDC = GetDC(NULL), hMem = CreateCompatibleDC(hDC);
    HBITMAP hBitmap = CreateDIBSection(hDC, &Info, DIB_RGB_COLORS, (void **)&Bits, NULL, 0);
    HGDIOBJ hOld = hBitmap ? SelectObject(hMem, hBitmap) : NULL;

    *Total = Width * Height;
    if (Width > 0 && Height > 0 && hOld &&
        BitBlt(hMem, 0, 0, Width, Height, hDC, Screen->left, Screen->top, SRCCOPY))
    {
        GdiFlush();
        for (Light = 0, i = 0; i < Width * Height; ++i)
        {
            DWORD c = Bits[i];
            if (((c & 0xff) + ((c >> 8) & 0xff) + ((c >> 16) & 0xff)) >= 3 * 0x40)
                ++Light;
        }
    }
    if (hOld)
        SelectObject(hMem, hOld);
    if (hBitmap)
        DeleteObject(hBitmap);
    DeleteDC(hMem);
    ReleaseDC(NULL, hDC);
    return Light;
}

/* The whole client area, as far as it is in the work area */
static LONG
ClientLight(HWND hWnd)
{
    RECT Client, Work;
    LONG Total;

    GetClientRect(hWnd, &Client);
    MapWindowPoints(hWnd, NULL, (POINT *)&Client, 2);
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &Work, 0))
        SetRect(&Work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    IntersectRect(&Client, &Client, &Work);
    return LightPixels(&Client, &Total);
}

/* Percent of dark pixels of a menu bar item: its text is painted */
static LONG
ItemDark(HWND hWnd, HMENU hMenu, UINT Item)
{
    RECT Rect;
    LONG Light, Total;

    if (!GetMenuItemRect(hWnd, hMenu, Item, &Rect))
        return -1;
    Light = LightPixels(&Rect, &Total);
    return Light < 0 || !Total ? -1 : (Total - Light) * 100 / Total;
}

static void
TestPosition(int x, int y, int Width, int Height, BOOL Maximize)
{
    HMENU hMenu = CreateMenu();
    HWND hWnd;
    LONG Initial, Light, Dark, Painted;

    AppendMenuW(hMenu, MF_STRING, 100, L"File");
    hWnd = CreateWindowW(L"FrameChangedClient", L"FrameChangedClient", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         x, y, Width, Height, NULL, hMenu, GetModuleHandleW(NULL), NULL);
    ok(hWnd != NULL, "CreateWindow failed: %lu\n", GetLastError());
    if (!hWnd)
        return;
    if (Maximize)
        ShowWindow(hWnd, SW_MAXIMIZE);
    SetForegroundWindow(hWnd);
    UpdateWindow(hWnd);
    Pump();
    /* A few stray pixels at the left edge of the first client show up on amd64
     * before any frame change; the strips the frame repaint erased were
     * hundreds. The frame changes below must not add a single one. */
    Initial = ClientLight(hWnd);
    ok(Initial >= 0 && Initial <= 8, "(%d,%d) %dx%d%s: %ld client pixels not painted before\n",
       x, y, Width, Height, Maximize ? " maximized" : "", Initial);
    /* The client keeps its bits: it is not erased and painted again either */
    Painted = Paints;

    /* Change the menu bar, then redraw the frame */
    AppendMenuW(hMenu, MF_STRING, 101, L"Edit");
    ok(DrawMenuBar(hWnd), "DrawMenuBar failed\n");
    Pump();
    Light = ClientLight(hWnd);
    Dark = ItemDark(hWnd, hMenu, 1);
    ok(Light >= 0 && Light <= Initial, "(%d,%d) %dx%d%s: %ld client pixels erased by DrawMenuBar (%ld before)\n",
       x, y, Width, Height, Maximize ? " maximized" : "", Light, Initial);
    ok(Dark >= 2 && Dark < 60, "(%d,%d) %dx%d%s: new menu item %ld%% dark after DrawMenuBar\n",
       x, y, Width, Height, Maximize ? " maximized" : "", Dark);
    ok(Paints == Painted, "(%d,%d) %dx%d%s: %ld WM_PAINT after DrawMenuBar\n",
       x, y, Width, Height, Maximize ? " maximized" : "", Paints - Painted);

    ok(SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOZORDER | SWP_NOACTIVATE), "SetWindowPos failed\n");
    Pump();
    Light = ClientLight(hWnd);
    Dark = ItemDark(hWnd, hMenu, 1);
    ok(Light >= 0 && Light <= Initial, "(%d,%d) %dx%d%s: %ld client pixels erased by SWP_FRAMECHANGED (%ld before)\n",
       x, y, Width, Height, Maximize ? " maximized" : "", Light, Initial);
    ok(Dark >= 2 && Dark < 60, "(%d,%d) %dx%d%s: new menu item %ld%% dark after SWP_FRAMECHANGED\n",
       x, y, Width, Height, Maximize ? " maximized" : "", Dark);
    ok(Paints == Painted, "(%d,%d) %dx%d%s: %ld WM_PAINT after SWP_FRAMECHANGED\n",
       x, y, Width, Height, Maximize ? " maximized" : "", Paints - Painted);
    DestroyWindow(hWnd);
}

/* The frame of a child changes. Its repaint goes to the frame of its parent, but
 * the child's visible region says nothing about that frame: the parent at the
 * screen origin, without WS_CLIPCHILDREN, had its client and the child erased
 * where that region fell. */
static void
TestChild(void)
{
    HMENU hMenu = CreateMenu();
    HWND hParent, hChild;
    LONG Initial, Light, Total, Painted;
    RECT Child;

    AppendMenuW(hMenu, MF_STRING, 100, L"File");
    hParent = CreateWindowW(L"FrameChangedClient", L"FrameChangedClient", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            0, 0, 400, 300, NULL, hMenu, GetModuleHandleW(NULL), NULL);
    ok(hParent != NULL, "CreateWindow failed: %lu\n", GetLastError());
    if (!hParent)
        return;
    /* No WS_CLIPSIBLINGS: the region path, not the whole-frame one */
    hChild = CreateWindowW(L"FrameChangedClient", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER,
                           20, 20, 150, 100, hParent, NULL, GetModuleHandleW(NULL), NULL);
    ok(hChild != NULL, "CreateWindow failed: %lu\n", GetLastError());
    SetForegroundWindow(hParent);
    UpdateWindow(hParent);
    Pump();
    Initial = ClientLight(hParent);
    ok(Initial >= 0 && Initial <= 8, "child: %ld parent client pixels not painted before\n", Initial);
    /* Kept: neither client is erased and painted again either */
    Painted = Paints;

    ok(SetWindowPos(hChild, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOZORDER | SWP_NOACTIVATE), "SetWindowPos failed\n");
    Pump();
    Light = ClientLight(hParent);
    ok(Light >= 0 && Light <= Initial, "child: %ld parent client pixels erased (%ld before)\n", Light, Initial);
    GetClientRect(hChild, &Child);
    MapWindowPoints(hChild, NULL, (POINT *)&Child, 2);
    Light = LightPixels(&Child, &Total);
    ok(Light == 0, "child: %ld child client pixels erased\n", Light);
    ok(Paints == Painted, "child: %ld WM_PAINT after the child's SWP_FRAMECHANGED\n", Paints - Painted);
    DestroyWindow(hParent);
}

START_TEST(FrameChangedClient)
{
    WNDCLASSW Class = {0};

    Class.lpfnWndProc = WndProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.hbrBackground = GetStockObject(WHITE_BRUSH);
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.lpszClassName = L"FrameChangedClient";
    ok(RegisterClassW(&Class) != 0, "RegisterClass failed: %lu\n", GetLastError());
    /* Keep the pointer out of the client areas sampled */
    SetCursorPos(0, 0);

    TestPosition(60, 60, 320, 220, FALSE);
    /* Moved by less than its size: the misplaced strips fall inside the client */
    TestPosition(100, 100, 400, 300, FALSE);
    /* Further away than its size: the misplaced region missed the window */
    TestPosition(330, 260, 300, 200, FALSE);
    /* Maximized, the window sits at minus the border width */
    TestPosition(100, 100, 400, 300, TRUE);
    TestChild();
    UnregisterClassW(L"FrameChangedClient", GetModuleHandleW(NULL));
}
