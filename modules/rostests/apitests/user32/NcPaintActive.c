/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Of the top-level windows of a thread that has the foreground, only the active one has the
 *              active frame: the default WM_NCPAINT draws the sizing border of the others as inactive.
 *              A window flashed active by FlashWindowEx keeps the active frame while it is not the active
 *              window; a window made active on a background queue, without a flash, stays visually inactive.
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

/* Scenario 2 and 4: a real FlashWindowEx draws a window active without making it the active window.
 * The caption flash toggles the frame through the flash system timer (WM_NCACTIVATE), so the flash
 * state property - not WNDS_ACTIVEFRAME, which IntSendNCPaint also sets for a queue-active window -
 * is what keeps the window active. Pump and sample until the active phase appears, then stop the
 * flash and confirm WM_NCACTIVATE(FALSE) returns the window to the inactive frame. */
static void
CheckRealFlashKeepsActiveFrame(HWND hActive, HWND hFlash, HDC hScreen)
{
    FLASHWINFO fwi;
    DWORD Start;
    BOOL SawActive = FALSE;
    UINT Found;

    fwi.cbSize = sizeof(fwi);
    fwi.hwnd = hFlash;
    fwi.dwFlags = FLASHW_CAPTION;
    fwi.uCount = 200;
    fwi.dwTimeout = 20;
    FlashWindowEx(&fwi);
    ok(GetActiveWindow() == hActive, "flashing changed the active window to %p\n", GetActiveWindow());

    Start = GetTickCount();
    while (GetTickCount() - Start < 2000)
    {
        Pump(40);
        if (GetForegroundWindow() != hActive) /* a flash must never take the foreground */
            break;
        SendMessageW(hFlash, WM_NCPAINT, 1, 0);
        Found = FindBorderColors(hFlash, hScreen);
        if (Found == FOUND_ACTIVE)
        {
            SawActive = TRUE;
            break;
        }
    }
    ok(SawActive, "a FlashWindowEx'd window never drew its active frame during the flash\n");
    ok(GetActiveWindow() == hActive, "the active window changed during the flash to %p\n", GetActiveWindow());

    fwi.dwFlags = FLASHW_STOP;
    FlashWindowEx(&fwi);
    Pump(50);
    SendMessageW(hFlash, WM_NCPAINT, 1, 0);
    Found = FindBorderColors(hFlash, hScreen);
    ok(Found == FOUND_INACTIVE, "after the flash stopped the window shows border colours %u, expected %u\n", Found, FOUND_INACTIVE);
}

typedef struct _BGCTX
{
    HINSTANCE hInstance;
    HANDLE hReady;
    HANDLE hPainted;
    HWND hWnd;
    DWORD ThreadId;
} BGCTX;

#define BG_REPAINT_MSG (WM_APP + 1)

/* A window activated on a background GUI thread becomes that queue active window (spwndActive). The
 * thread keeps a message loop so the foreground handoff (WM_NCACTIVATE) does not deadlock; on request
 * it repaints its own frame, so IntSendNCPaint sets WNDS_ACTIVEFRAME on it while it is not the
 * foreground. The default WM_NCPAINT must still draw it inactive. */
static DWORD WINAPI
BgThreadProc(LPVOID Param)
{
    BGCTX *Ctx = (BGCTX *)Param;
    MSG Msg;

    Ctx->ThreadId = GetCurrentThreadId();
    Ctx->hWnd = CreateWindowExW(WS_EX_TOPMOST, L"NcPaintActive", L"background", WS_OVERLAPPEDWINDOW,
                                60, 320, 300, 200, NULL, NULL, Ctx->hInstance, NULL);
    if (Ctx->hWnd)
    {
        ShowWindow(Ctx->hWnd, SW_SHOWNA);
        SetActiveWindow(Ctx->hWnd);
    }
    SetEvent(Ctx->hReady);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (Msg.hwnd == NULL && Msg.message == BG_REPAINT_MSG)
        {
            /* on this thread the paint pipeline runs, so IntSendNCPaint sets WNDS_ACTIVEFRAME */
            RedrawWindow(Ctx->hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
            SetEvent(Ctx->hPainted);
            continue;
        }
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    return 0;
}

static void
CheckBackgroundActiveDrawsInactive(HWND hForeground, HINSTANCE hInstance, HDC hScreen)
{
    BGCTX Ctx = { 0 };
    HANDLE hThread;
    UINT Found;

    Ctx.hInstance = hInstance;
    Ctx.hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ctx.hPainted = CreateEventW(NULL, TRUE, FALSE, NULL);
    hThread = CreateThread(NULL, 0, BgThreadProc, &Ctx, 0, NULL);
    if (!hThread || WaitForSingleObject(Ctx.hReady, 5000) != WAIT_OBJECT_0 || !Ctx.hWnd)
    {
        skip("The background thread did not create its window\n");
        goto Cleanup;
    }

    /* keep A in the foreground: the background queue active window must not hold it */
    SetForegroundWindow(hForeground);
    SetActiveWindow(hForeground);
    Pump(300);
    if (GetForegroundWindow() != hForeground)
    {
        skip("Could not keep the foreground on the first window (%p)\n", GetForegroundWindow());
        goto Cleanup;
    }

    /* let the background window repaint its frame (sets WNDS_ACTIVEFRAME) while it is not foreground */
    PostThreadMessageW(Ctx.ThreadId, BG_REPAINT_MSG, 0, 0);
    WaitForSingleObject(Ctx.hPainted, 5000);
    Pump(100);

    ok(GetForegroundWindow() == hForeground, "a background SetActiveWindow took the foreground: %p\n", GetForegroundWindow());
    SendMessageW(Ctx.hWnd, WM_NCPAINT, 1, 0);
    Found = FindBorderColors(Ctx.hWnd, hScreen);
    ok(Found == FOUND_INACTIVE, "a background queue-active window shows border colours %u, expected %u\n", Found, FOUND_INACTIVE);

Cleanup:
    if (Ctx.ThreadId)
        PostThreadMessageW(Ctx.ThreadId, WM_QUIT, 0, 0);
    if (hThread)
    {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }
    if (Ctx.hReady)
        CloseHandle(Ctx.hReady);
    if (Ctx.hPainted)
        CloseHandle(Ctx.hPainted);
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
        /* Scenario 1: only the foreground queue active window draws the active frame */
        CheckFrames(hFirst, hSecond, hScreen, "first window active");

        /* Scenarios 2 and 4: a real FlashWindowEx keeps the active frame during the flash,
           and the inactive frame once the flash is stopped */
        CheckRealFlashKeepsActiveFrame(hFirst, hSecond, hScreen);

        /* Scenario 3: a window activated on a background queue draws inactive */
        CheckBackgroundActiveDrawsInactive(hFirst, Class.hInstance, hScreen);

        /* Scenario 4: a genuine activation change deactivates the old active window */
        SetForegroundWindow(hSecond);
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
