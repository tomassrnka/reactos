/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for UpdateLayeredWindow
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define MAX_STEPS 4

typedef struct
{
    HWND hWnd;
    int Steps;
    POINT Pos[MAX_STEPS];
    SIZE Size[MAX_STEPS];
    HANDLE hDone;
    BOOL Result[MAX_STEPS];
} ULW_PARAMS;

static LONG PosMessages;

static LRESULT CALLBACK CountingProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    switch (Msg)
    {
        case WM_WINDOWPOSCHANGING:
        case WM_WINDOWPOSCHANGED:
        case WM_NCCALCSIZE:
        case WM_MOVE:
        case WM_SIZE:
            InterlockedIncrement(&PosMessages);
            break;
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

static BOOL UpdateColor(HWND hWnd, POINT *Pos, SIZE *Size, DWORD Color)
{
    BLENDFUNCTION Blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT Src = { 0, 0 };
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER) } };
    HDC hdc;
    HBITMAP hbm;
    PVOID Bits;
    BOOL Ret;

    bmi.bmiHeader.biWidth = Size->cx;
    bmi.bmiHeader.biHeight = -Size->cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    hdc = CreateCompatibleDC(NULL);
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &Bits, NULL, 0);
    SelectObject(hdc, hbm);
    if (Bits)
    {
        LONG i;
        for (i = 0; i < Size->cx * Size->cy; i++)
            ((DWORD *)Bits)[i] = Color;
    }
    Ret = UpdateLayeredWindow(hWnd, NULL, Pos, Size, hdc, &Src, 0, &Blend, ULW_ALPHA);
    DeleteDC(hdc);
    DeleteObject(hbm);
    return Ret;
}

static BOOL Update(HWND hWnd, POINT *Pos, SIZE *Size)
{
    return UpdateColor(hWnd, Pos, Size, 0x80808080);
}

/* Screen pixel, layered windows included */
static COLORREF ScreenPixel(int x, int y)
{
    HDC hdcScreen = GetDC(NULL), hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, 1, 1);
    COLORREF c;
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    BitBlt(hdcMem, 0, 0, 1, 1, hdcScreen, x, y, SRCCOPY | CAPTUREBLT);
    c = GetPixel(hdcMem, 0, 0);
    SelectObject(hdcMem, old);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return c;
}

static DWORD WINAPI UpdateThread(PVOID Parameter)
{
    ULW_PARAMS *p = Parameter;
    int i;

    for (i = 0; i < p->Steps; i++)
        p->Result[i] = Update(p->hWnd, &p->Pos[i], &p->Size[i]);
    SetEvent(p->hDone);
    return 0;
}

static void Pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static HWND CreateLayered(void)
{
    HWND hWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"UlwTestClass", L"", WS_POPUP,
                                100, 100, 200, 150, NULL, NULL, GetModuleHandle(NULL), NULL);
    ok(hWnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    if (hWnd)
    {
        ShowWindow(hWnd, SW_SHOWNOACTIVATE);
        Pump();
    }
    return hWnd;
}

/*
 * A renderer thread updates a layered popup while the popup's own thread
 * waits for it and pumps no messages (Firefox's menus and panels). Each
 * update must return at once, take effect at once and in order, and call no
 * window procedure.
 */
static void RunCrossThread(const char *Name, int Steps, const POINT *Pos, const SIZE *Size)
{
    ULW_PARAMS *p;
    HANDLE hThread;
    RECT rc;
    DWORD Wait, Start;
    int i;

    p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
    if (!p)
        return;
    p->hWnd = CreateLayered();
    if (!p->hWnd)
    {
        HeapFree(GetProcessHeap(), 0, p);
        return;
    }
    p->Steps = Steps;
    for (i = 0; i < Steps; i++)
    {
        p->Pos[i] = Pos[i];
        p->Size[i] = Size[i];
    }
    p->hDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    PosMessages = 0;
    hThread = CreateThread(NULL, 0, UpdateThread, p, 0, NULL);
    ok(hThread != NULL, "%s: CreateThread failed: %lu\n", Name, GetLastError());
    if (!hThread)
    {
        CloseHandle(p->hDone);
        DestroyWindow(p->hWnd);
        HeapFree(GetProcessHeap(), 0, p);
        return;
    }

    /* Wait without pumping messages, as the popup's thread does */
    Wait = WaitForSingleObject(p->hDone, 5000);
    ok(Wait == WAIT_OBJECT_0, "%s: UpdateLayeredWindow waited for the window's thread (%lu)\n", Name, Wait);

    /* The geometry is in place before the window's thread runs */
    GetWindowRect(p->hWnd, &rc);
    i = Steps - 1;
    ok(rc.left == Pos[i].x && rc.top == Pos[i].y &&
       rc.right - rc.left == Size[i].cx && rc.bottom - rc.top == Size[i].cy,
       "%s: window rectangle (%ld,%ld)-(%ld,%ld), expected (%ld,%ld) %ldx%ld\n", Name,
       rc.left, rc.top, rc.right, rc.bottom, Pos[i].x, Pos[i].y, Size[i].cx, Size[i].cy);

    /* Let the worker finish (pumping unblocks it if it waits for this thread) */
    Start = GetTickCount();
    while (MsgWaitForMultipleObjects(1, &hThread, FALSE, 1000, QS_ALLINPUT) != WAIT_OBJECT_0 &&
           GetTickCount() - Start < 30000)
        Pump();
    if (WaitForSingleObject(hThread, 0) != WAIT_OBJECT_0)
    {
        /* Still running: leave its parameters and window alone */
        ok(FALSE, "%s: the worker did not finish\n", Name);
        CloseHandle(hThread);
        return;
    }
    CloseHandle(hThread);
    for (i = 0; i < Steps; i++)
        ok(p->Result[i], "%s: update %d failed\n", Name, i);

    Pump();
    ok(PosMessages == 0, "%s: %ld window position messages\n", Name, PosMessages);
    GetWindowRect(p->hWnd, &rc);
    i = Steps - 1;
    ok(rc.left == Pos[i].x && rc.top == Pos[i].y &&
       rc.right - rc.left == Size[i].cx && rc.bottom - rc.top == Size[i].cy,
       "%s: window rectangle after pumping (%ld,%ld)-(%ld,%ld)\n", Name, rc.left, rc.top, rc.right, rc.bottom);

    CloseHandle(p->hDone);
    DestroyWindow(p->hWnd);
    HeapFree(GetProcessHeap(), 0, p);
}

/* WinEvent hook tests: the hook runs in context, during or after the update */
static HWND HookWnd;
static int HookMode, HookCalls;
static POINT NestedPos = { 300, 120 };
static SIZE NestedSize = { 200, 150 };

static void CALLBACK LocationHook(HWINEVENTHOOK hHook, DWORD Event, HWND hWnd, LONG idObject, LONG idChild, DWORD Thread, DWORD Time)
{
    if (hWnd != HookWnd || idObject != OBJID_WINDOW || HookCalls++)
        return;
    if (HookMode == 1)
        DestroyWindow(hWnd);
    else if (HookMode == 2)
        UpdateColor(hWnd, &NestedPos, &NestedSize, 0xFF0000FF);   /* blue */
}

static void RunHook(int Mode)
{
    HWINEVENTHOOK hHook;
    POINT Pos = { 160, 120 };
    SIZE Size = { 200, 150 };
    RECT rc;
    BOOL Ret;

    HookWnd = CreateLayered();
    if (!HookWnd)
        return;
    HookMode = Mode;
    HookCalls = 0;
    hHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, GetModuleHandleW(NULL),
                            LocationHook, GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
    if (!hHook)
        hHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL,
                                LocationHook, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    ok(hHook != NULL, "SetWinEventHook failed: %lu\n", GetLastError());
    trace("Mode %d: %s WinEvent hook\n", Mode, hHook ? "a" : "no");

    Ret = UpdateColor(HookWnd, &Pos, &Size, 0xFFFF0000);   /* red */
    Pump();
    UnhookWinEvent(hHook);

    if (Mode == 1)
    {
        /* Destroyed by the hook: no crash, and the window is gone */
        if (HookCalls)
            ok(!IsWindow(HookWnd), "window still exists\n");
        else
        {
            skip("LOCATIONCHANGE hook not called\n");
            DestroyWindow(HookWnd);
        }
        return;
    }

    ok(Ret, "UpdateLayeredWindow failed: %lu\n", GetLastError());
    GetWindowRect(HookWnd, &rc);
    if (HookCalls)
    {
        COLORREF c;
        /* The nested update ran last: its position and its content */
        ok(rc.left == NestedPos.x && rc.top == NestedPos.y, "window at (%ld,%ld), expected the nested update's (%ld,%ld)\n",
           rc.left, rc.top, NestedPos.x, NestedPos.y);
        c = ScreenPixel(rc.left + 100, rc.top + 75);
        ok(c == RGB(0, 0, 255), "content 0x%06lx at the final position, expected the nested update's blue\n", c);
    }
    else
        skip("LOCATIONCHANGE hook not called\n");
    DestroyWindow(HookWnd);
}

/* A shaped layered window moves: the area it really uncovered is repainted */
static void Test_ShapedMoveExposes(void)
{
    WNDCLASSW wc = { 0 };
    HWND hBack, hWnd;
    HRGN hRgn;
    POINT Pos = { 100, 100 }, Pos2 = { 120, 100 };
    SIZE Size = { 200, 150 };
    COLORREF Before, After;

    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 0));
    wc.lpszClassName = L"UlwBackClass";
    RegisterClassW(&wc);
    hBack = CreateWindowExW(0, L"UlwBackClass", L"", WS_POPUP | WS_VISIBLE, 50, 50, 400, 300, NULL, NULL, GetModuleHandle(NULL), NULL);
    ok(hBack != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    UpdateWindow(hBack);

    hWnd = CreateLayered();
    if (!hWnd || !hBack)
        goto done;
    /* Only the right half is part of the window */
    hRgn = CreateRectRgn(100, 0, 200, 150);
    SetWindowRgn(hWnd, hRgn, TRUE);
    ok(UpdateColor(hWnd, &Pos, &Size, 0xFF0000FF), "UpdateLayeredWindow failed: %lu\n", GetLastError());
    Pump();
    Sleep(100);
    Before = ScreenPixel(210, 175);
    ok(Before == RGB(0, 0, 255), "shaped window not drawn: 0x%06lx\n", Before);

    /* Move 20 pixels right: screen x 200..219 is uncovered */
    ok(UpdateColor(hWnd, &Pos2, &Size, 0xFF0000FF), "UpdateLayeredWindow failed: %lu\n", GetLastError());
    Pump();
    Sleep(100);
    Pump();
    After = ScreenPixel(210, 175);
    ok(After == RGB(255, 0, 0), "uncovered area not repainted: 0x%06lx\n", After);

done:
    if (hWnd) DestroyWindow(hWnd);
    if (hBack) DestroyWindow(hBack);
    UnregisterClassW(L"UlwBackClass", GetModuleHandle(NULL));
    DeleteObject(wc.hbrBackground);
}

static void Test_SameThreadNoMessages(void)
{
    HWND hWnd = CreateLayered();
    POINT Pos = { 160, 140 };
    SIZE Size = { 120, 90 };
    RECT rc, rcClient;

    if (!hWnd)
        return;
    PosMessages = 0;
    ok(Update(hWnd, &Pos, &Size), "UpdateLayeredWindow failed: %lu\n", GetLastError());
    Pump();
    ok(PosMessages == 0, "%ld window position messages\n", PosMessages);
    GetWindowRect(hWnd, &rc);
    ok(rc.left == 160 && rc.top == 140 && rc.right == 280 && rc.bottom == 230,
       "window rectangle (%ld,%ld)-(%ld,%ld)\n", rc.left, rc.top, rc.right, rc.bottom);
    GetClientRect(hWnd, &rcClient);
    ok(rcClient.right == 120 && rcClient.bottom == 90, "client rectangle %ldx%ld\n", rcClient.right, rcClient.bottom);
    DestroyWindow(hWnd);
}

START_TEST(UpdateLayeredWindow)
{
    WNDCLASSW wc = { 0 };
    static const POINT SamePos[] = { { 100, 100 } };
    static const SIZE SameSize[] = { { 200, 150 } };
    static const POINT MovePos[] = { { 150, 120 } };
    static const SIZE MoveSize[] = { { 220, 160 } };
    static const POINT OrderPos[] = { { 200, 100 }, { 300, 100 } };
    static const SIZE OrderSize[] = { { 200, 150 }, { 200, 150 } };
    static const POINT BackPos[] = { { 200, 100 }, { 100, 100 } };
    static const SIZE BackSize[] = { { 200, 150 }, { 200, 150 } };

    wc.lpfnWndProc = CountingProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"UlwTestClass";
    RegisterClassW(&wc);

    RunCrossThread("unchanged", 1, SamePos, SameSize);
    RunCrossThread("move and resize", 1, MovePos, MoveSize);
    RunCrossThread("two moves", 2, OrderPos, OrderSize);
    RunCrossThread("move and back", 2, BackPos, BackSize);
    Test_SameThreadNoMessages();
    RunHook(1);
    RunHook(2);
    Test_ShapedMoveExposes();

    UnregisterClassW(L"UlwTestClass", GetModuleHandle(NULL));
}
